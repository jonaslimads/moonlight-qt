# Native Linux build (Ubuntu 24.04, no root)

These scripts build a native `moonlight` binary plus the `CONFIG+=tests` test
tree on a machine where we do not install anything system-wide. `build/` stays
git ignored; only these scripts are versioned.

    scripts/linux/build-moonlight.sh              configure (if needed) + make
    scripts/linux/build-moonlight.sh clean        wipe build/tree first
    scripts/linux/build-moonlight.sh bootstrap    also fetch the .deb sysroot
    scripts/linux/build-moonlight.sh CONFIG+=tests   also build the test tree
    scripts/linux/run-tests.sh [filter]           run every built test binary
    scripts/linux/install-native.sh               build, install, register with the desktop
    scripts/linux/install-native.sh --no-build    install what is already built
    scripts/linux/install-native.sh --no-desktop  install without touching the desktop

`install-native.sh` puts everything under `~/.local/opt/moonlight-pyrowave` (a
stable path, so the desktop entry and the dock pin keep working after a rebuild),
writes `bin/moonlight` as a launcher that points Qt at the bundled runtime next to
it, registers `com.moonlight_stream.Moonlight.desktop` and appends it to
`org.gnome.shell favorite-apps` instead of replacing the list.

What gets bundled is decided per library, and the interesting part is what does
*not*: the system's ffmpeg, libplacebo, libGL and X libraries stay system
provided, because a GPU driver or codec loaded out of a bundled tree instead of the
system's fails as a black screen and nothing else. Qt and its icu do get bundled,
because noble ships Qt 6.4.2 and this does not build against it, and Qt's own
libraries have to stay internally consistent. Qt's private copies of ffmpeg and
friends are filtered out even though `ldd` happily resolves to them.

`env.sh` is sourced by the others and may be sourced by hand to get `qmake6`,
the compiler include path and the link path on one line.

## Where the dependencies come from

Qt and the -dev packages are identical for every checkout on this machine, so
`ML_TOOLS` is shared: it points at the first directory that carries a usable
`Qt/6.8.3/gcc_64`, defaulting to `build/tools` and falling back to the sibling
`../vibemis/build/tools`. Export `ML_TOOLS` to pin it. `fetch-debs.sh` adds only
what pkg-config still cannot resolve, extracted with `dpkg -x` into
`build/tools/sysroot`, which `env.sh` searches before the shared sysroot.

The system Qt in Ubuntu 24.04 is 6.4.2, which this project does not build
against, so the Qt runtime has to be shipped next to the binary (see
`package.sh`, still to come) rather than taken from apt.

## Machine notes that shaped these scripts

* `/etc/vulkan/icd.d/` is empty; the ICD manifests are in
  `/usr/share/vulkan/icd.d/`. `run-tests.sh` exports every manifest it finds
  there as `VK_DRIVER_FILES`, because pointing at one vendor only silently
  halves the visible GPUs. The NVIDIA ICD library is `libGLX_nvidia.so.0`.
* The laptop panel (`eDP-1`, 120 Hz, VRR capable) is driven by the Intel GPU;
  the RTX 3050 can only render offload to it. Both drivers must therefore stay
  visible, and the PyroWave round trip is expected to pass on either.
* `x11.pc` Requires `xproto` and `kbproto`, which only `x11proto-dev` ships, so
  the probe list in `fetch-debs.sh` starts from `app/app.pro`'s own probes
  instead of guessing.

## Test baseline on this machine

`scripts/linux/build-moonlight.sh CONFIG+=tests && scripts/linux/run-tests.sh`
gives 23 passed, 1 failed:

* `tst_dualsensehaptics` fails here ("right leaks into left", occasionally
  "rendered both actuators") and did before any Linux PyroWave work. It is
  deterministic under load and is not touched by this branch's codec work.
* `tst_overlay` needs the directory holding `res/ModeSeven.ttf` as `argv[1]`;
  `run-tests.sh` passes `$ML_REPO/app` the way the Windows workflow does, and
  passes `-o -,txt` to the three tests that write fixtures.
* The VRR tests, both PyroWave host-less tests (`tst_pyrowaveframing`,
  `tst_pyrowaveroundtrip`) and everything else pass.
