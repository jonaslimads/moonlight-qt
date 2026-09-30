# PyroWave on Linux through the plvk renderer

Status: implemented incrementally on the `linux-pyrowave` branch.

Audience: anyone who has to change this code later, on Linux or on Windows.

## What PyroWave is and why it needs no new renderer

PyroWave is Themaister's intra-only Vulkan compute wavelet codec (JPEG 2000-like
CDF 9/7, hundreds of Mbps, LAN only). We vendor it under `pyrowave/` as a static
library and consume it through its C API. It is a *decoder*: it writes decoded
planes into images that somebody else allocated, and it is not a display path.

Upstream ships it Windows-only because the only surface pool in the tree is
`d3d11pyrowave.cpp`, which hands the codec D3D11 textures as NT handles. Nothing
in the codec itself is Windows-specific: `tests/pyrowave/roundtrip` compiles the
vendored codec and decodes correctly on this laptop's NVIDIA and Intel drivers
with no OS layer at all (luma PSNR 44-47 dB, 4:2:0 and 4:4:4, packet loss
recovery). That test is the reason the Linux port is a plumbing job rather than a
port of a codec.

## The invariant: exactly one VkDevice

The renderer (`plvk`, i.e. libplacebo's Vulkan backend) creates the `VkInstance`
and `VkDevice` today. PyroWave's C API has two entry points:

    pyrowave_create_device_by_compat()   // picks its own device, interops by adapter identity
    pyrowave_create_device()             // "Direct API that shares a VkDevice.
                                         //  Avoids needing to use external memory
                                         //  to encode and decode."  (pyrowave.h)

We use the second one. **Decode and present always share one `VkDevice`.** Consequences:

* Zero DMA-BUF, zero `VK_EXTERNAL_MEMORY_HANDLE_TYPE_*`, zero exported handles.
  On Windows the codec must import D3D11 textures because D3D11 owns them; here
  there is nothing to import, both sides are the same device.
* No Granite symbols leak into the app. The static library hides them
  (`-fvisibility=hidden`, `pyrowave/pyrowave.pri`) and they must stay hidden: the
  moment a `vk*` symbol from volk were interposable it could hijack the loader
  calls made by libplacebo or FFmpeg.
* Panel-with-dGPU decode needs no code branch. This laptop's `eDP-1` is driven by
  the Intel GPU and the RTX 3050 can only render offload to it (Provider1 has the
  Sink Output capability only), so the pixel copy to the panel is done by X/PRIME
  below us, exactly as it is for every other game we run here.
* Decode and render submit to one queue, so submission order alone orders
  decode -> render without a CPU stall.

Rejected alternatives: importing a codec-owned device into libplacebo
(`pl_vulkan_import()`, needs Granite's device anyway), and a standalone PyroWave
renderer (would have to reimplement cursor, overlays, colour management, tone
mapping and the VRR present path for nothing).

## Which side allocates

Under the shared-device model the *application* allocates plain `VkImage`s and
fills the `pyrowave_image_view` descriptors itself; `pyrowave_image_create()` is
documented as external-memory-only, so it is the wrong tool here.

Per surface: three planes (Y, Cb, Cr) as separate single-plane images, matching
what `PyroWaveSharedPlane` already describes and what the D3D11 pool does.

* `format`: `VK_FORMAT_R8_UNORM` or `VK_FORMAT_R16_UNORM` (`PyroWavePlaneFormat`).
* `usage`: `VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`
  (storage because the codec writes it as an image, sampled because libplacebo reads it).
* `flags`: `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` (the plvk wrapper asks for it).
* `layout`: `VK_IMAGE_LAYOUT_GENERAL`, the only layout both sides agree on without
  extra transitions.
* memory allocated `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT` through libplacebo, so
  libplacebo owns the allocator and the memory type choice.

Then `pl_vulkan_wrap()` gives us a `pl_tex` per plane, `pl_update_avframe()` /
`pl_map_avframe_ex()` consume them through the `mapParams.tex` slots `plvk.cpp`
already fills, and the rest of the render path (colour, tone mapping, cursor,
overlays, VRR present) is untouched.

## Synchronization

Both directions have first-class support in libplacebo 6.338; we deliberately use
none of the `PL_DEPRECATED` `pl_sync` / `pl_tex_export` API.

* **decode -> render.** pyrowave signals a timeline `VkSemaphore` at value V. The
  app calls `pl_vulkan_release_ex()` with `{tex, layout, qf, semaphore = {sem, V}}`:
  "the semaphore to wait on before libplacebo will actually use or modify the
  image". That call returns `void`, because handing an image to libplacebo cannot
  fail, so there is no error path to write here. For planar textures that semaphore
  has to be a timeline one, which is what we create.
* **render -> decode.** Before handing an image back to the codec, the app calls
  `pl_vulkan_hold_ex()`, passing the timeline semaphore and value that should fire
  once libplacebo's reads are done, and checking the `bool` it returns. The
  decoder's existing `SurfaceFreeList` already keys reuse on "release fence value
  not yet reached", so the Linux pool feeds that same free list.

Each plane gets its own release value: signalling a Vulkan timeline semaphore twice
with the same value is not legal, and a surface has three planes.

Ordering rule, and the reason the call sites are where they are: a release signal
has to be recorded *after* the `vkQueueSubmit` that carried the reads, because
libplacebo submits its drawing in `pl_swapchain_submit_frame()`, not while it
records it. Signalling earlier would let the codec overwrite a surface that is still
being sampled. So `PlVkRenderer::releasePyroWaveSurface()` runs at the end of
`renderFrame()` and after every `submitPendingSwapchainFrame()` that succeeded -
including the one that abandons a VRR frame, which was still drawn. Both run on the
render thread, and the AVFrame owning the `PyroWaveFrameRef` has to still be alive
there, since that is what the release value is written into. If a PyroWave stream
ever shows torn or impossibly recycled surfaces, check that invariant first.

The semaphores are plain `vkCreateSemaphore` calls with `VK_SEMAPHORE_TYPE_TIMELINE`
on the shared device. Nothing is exported: `pyrowave_sync_point` takes a
`VkSemaphore` and so does `pl_vulkan_sem`, so on one device both sides can use the
same handle. `pyrowave_sync_object` is only needed on the Windows path, to pull an
NT fence into the codec's device.

The codec submits on plvk's own graphics queue, so it has to take libplacebo's queue
lock, which `pl_vulkan_t::lock_queue` hands out; the pool passes it on as pyrowave's
`queue_lock_callback`. Decoding happens on the decode thread and presenting on the
render thread, so that lock is the only thing keeping them apart.

## What is shared with the decoder

`IPyroWaveSurfacePool` gained an explicit model instead of a platform assumption:

| model | planes | fences | device choice |
|---|---|---|---|
| `ExternalHandles` (Windows/D3D11, default) | NT handles | NT handles | `pyrowave_create_device_by_compat()` + LUID |
| `SharedVulkanDevice` (Linux/plvk) | plain `VkImage` | plain timeline `VkSemaphore` | `pyrowave_create_device()` |

`PyroWaveVulkanDevice` carries the instance, physical device, device,
`GetInstanceProcAddr`, the queue to submit on and - required by the API - the
`VkInstanceCreateInfo` / `VkDeviceCreateInfo` the objects were created with: the
codec keeps those pointers for its whole lifetime, so the pool must outlive the
decoder and nothing may rewrite them.

`pyroWaveAdapterLuid()` stays on the interface for the D3D11 model and is unused
by the Linux one. The Windows build is the regression gate for every change here:
if `d3d11pyrowave.cpp` or `pyrowavedecoder.cpp` stops compiling for `win32`, the
change is wrong.

## Colour

`plvk.cpp` forces `mappedFrame->repr.levels = PL_COLOR_LEVELS_FULL` as a labelled
hack. PyroWave carries its own range in the stream, so the Linux path must check
it against that hack rather than inherit it silently; a wrong range here is a
washed-out or crushed picture with no other symptom.

## Telemetry (the point of the exercise)

The comparison against the Windows build needs, at 5 s intervals: decoded
frames/s, presented frames/s, rejected frames, partial frames
(`lastFramePartial()`), and **decode ms/frame**. Per-frame GPU time comes from
`pyrowave_device_report_performance_stats()`, which the codec already implements
with Vulkan timestamps, and is folded into the existing `.vrrtrace` schema so a
capture from this build is A/B-able against a Windows capture. Wall-clock probe
timings are not a benchmark: the CPU-side PSNR work dominates them.

`pyrowave_device_confirm_interop_support()` runs after device creation as a
sanity probe, and the decoder must fall back to a non-PyroWave path rather than
kill the session if it fails.

## Build gates

* `pyrowave/pyrowave.pri` needs `-msse3` for x86-64 GCC/Clang: Granite picks its
  vector maths on `__SSE3__`, whose baseline is SSE2, and otherwise hits the
  "Implement me." `#error`. Upstream's CMake passes the same flag.
* `moonlight-qt.pro` and `app/app.pro` enable `pyrowave` for
  `unix:!macx:contains(QT_ARCH, x86_64)` in addition to `win32`. macOS stays out:
  it would mean a MoltenVK/Vulkan story nobody has tested.
* `tests/pyrowave/pyrowave.pro` builds the round trip on Linux x86-64 too; the
  D3D11 pool test stays Windows-only.
* No `-lvulkan`: volk dlopens the loader, and `vulkan/vulkan.h` comes from the
  sysroot (Linux) or the SDK (Windows) the same way `pyrowavedecoder.cpp` already
  gets it.
* `app/backend/systemproperties.cpp` must report PyroWave support on Linux
  (compile-time plus a runtime loader/GPU check) or the Settings toggle never
  appears.

## Known baseline on this machine

`scripts/linux/run-tests.sh` gives 23 passed / 1 failed before any Linux PyroWave
code runs: `tst_dualsensehaptics` fails deterministically here ("right leaks into
left"). It is unrelated to this work and untouched by it. See
`scripts/linux/README.md`.

## Status of the Linux path

Built and linked, with the whole test tree at its pre-existing baseline, but not
yet seen running against a host. What that means in practice:

* The colour path is unverified. `pl_frame_from_avframe()` supplies the colourimetry
  and plane semantics, and this renderer only replaces the texture pointers, so a
  mistake here shows up as a washed-out or crushed picture and nothing else. It has
  to be looked at against the Windows build before the numbers are trusted.
* `pyrowave_create_device()` has never been called on a Linux driver with a
  libplacebo-made device. It validates itself against the create infos we
  reconstruct from `pl_vulkan_t::extensions` and `::features`, so a mismatch will
  surface as a device creation failure with a log line, not as silent corruption.
* The round trip test decodes with CPU buffers, so it does not exercise the GPU
  submit path, the timeline semaphores or the queue lock at all. Those need a real
  stream, or a new test that decodes into images the way this code does.
* Whether the host's bitstream ID matches `PYROWAVE_BITSTREAM_ID` decides whether a
  stream starts at all; check the handshake log before debugging anything else.
