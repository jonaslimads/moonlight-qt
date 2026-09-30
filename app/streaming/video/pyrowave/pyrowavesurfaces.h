#pragma once

// Contract between the PyroWave decoder (Vulkan) and a renderer that owns the
// decoded planes. The renderer allocates a pool of plane surfaces and shares
// them with the decoder along with two timeline fences; the decoder decodes
// into a free surface and signals the decode fence. The renderer waits for that
// value before sampling and signals the release fence after its last read, which
// the decoder waits for before reusing the surface. Neither side blocks the CPU
// on the other.
//
// Two sharing models fit that contract, and PyroWaveSharingModel says which one
// a pool implements. They differ only in how objects cross the boundary:
//
//   ExternalHandles    The renderer owns a D3D11 device on Windows, so planes
//                      and fences cross as NT handles and the codec picks its
//                      own Vulkan device by adapter identity.
//   SharedVulkanDevice The renderer owns the VkDevice and lends it to the codec,
//                      so a plane is a plain VkImage and the fences are plain
//                      VkSemaphores. Nothing is exported, because there is
//                      nothing to export to: both sides are the same device.

#include <atomic>
#include <cstdint>

#include <vulkan/vulkan.h>

extern "C" {
#include <libavutil/frame.h>
}

enum class PyroWavePlaneFormat {
    R8Unorm,
    R16Unorm,
};

enum class PyroWaveSharingModel {
    ExternalHandles,
    SharedVulkanDevice,
};

struct PyroWaveSharedPlane {
    // ExternalHandles: an OS handle (Windows NT HANDLE) the decoder takes
    // ownership of. SharedVulkanDevice: a VkImage created on the shared device,
    // which the renderer keeps and the decoder only borrows.
    uintptr_t handle = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    PyroWavePlaneFormat format = PyroWavePlaneFormat::R8Unorm;
};

// The renderer's Vulkan device for the SharedVulkanDevice model.
struct PyroWaveVulkanDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;

    // The create infos the instance and device were made with. The codec keeps
    // pointers to them for the whole life of its device object, so the pool must
    // outlive the decoder and nothing may modify what they point to.
    const VkInstanceCreateInfo* instanceCreateInfo = nullptr;
    const VkDeviceCreateInfo* deviceCreateInfo = nullptr;

    // A queue the codec may submit spurious work on. This is the same queue the
    // renderer presents on, which is what makes the decode -> render order free:
    // two submissions to one queue execute in submission order.
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    uint32_t queueIndex = 0;

    // Locks around queue submission. The renderer also submits on this queue, and
    // libplacebo requires external users to take its queue lock, so the codec has
    // to be told about it rather than left to vkQueueSubmit freely from the decode
    // thread. Called with the userdata below; may be NULL only if nothing else
    // submits on this device from another thread.
    void (*queueLock)(void* userdata) = nullptr;
    void (*queueUnlock)(void* userdata) = nullptr;
    void* queueLockUserdata = nullptr;
};

class IPyroWaveSurfacePool {
public:
    virtual ~IPyroWaveSurfacePool() = default;

    // Which model the handles below carry. D3D11 stays on the default.
    virtual PyroWaveSharingModel pyroWaveSharingModel() const {
        return PyroWaveSharingModel::ExternalHandles;
    }

    // Identity of the adapter that owns the surfaces (8-byte Windows LUID).
    // Only used by the ExternalHandles model.
    virtual bool pyroWaveAdapterLuid(uint8_t luid[8]) = 0;

    // Only used by the SharedVulkanDevice model: the device to decode on.
    virtual bool pyroWaveVulkanDevice(PyroWaveVulkanDevice* device) {
        (void)device;
        return false;
    }

    virtual int pyroWaveSurfaceCount() const = 0;

    // Returns fresh handles for the three planes (Y, Cb, Cr) of a surface.
    // Under ExternalHandles these are OS handles the caller owns and must close
    // if it does not consume them; under SharedVulkanDevice they are VkImages
    // that belong to the renderer and stay valid for its whole life.
    virtual bool exportPyroWaveSurface(int index, PyroWaveSharedPlane planes[3]) = 0;

    // Timeline semaphores shared across the boundary. The decoder signals the
    // decode one when it finished writing a surface and waits on the release one
    // before overwriting it.
    //
    // Under ExternalHandles they are fresh OS handles the caller owns and must
    // close if it does not consume them, and the decoder imports them as
    // pyrowave_sync_objects.
    virtual uintptr_t exportPyroWaveDecodeFence() = 0;
    virtual uintptr_t exportPyroWaveReleaseFence() = 0;

    // Under SharedVulkanDevice the two semaphores are plain VkSemaphores of the
    // shared device, which need no importing at all: pyrowave's sync points take
    // a VkSemaphore and libplacebo's pl_vulkan_sem takes one too. They belong to
    // the renderer and must outlive the decoder.
    virtual bool pyroWaveVulkanSync(VkSemaphore* decode, VkSemaphore* release) {
        (void)decode;
        (void)release;
        return false;
    }
};

// Owned by each PyroWave AVFrame through frame->buf[0]. Freeing the last
// reference returns the surface to the decoder with the release fence value
// that makes it safe to overwrite.
struct PyroWaveFrameRef {
    static constexpr uint32_t k_Magic = 0x50595257; // "PYRW"

    uint32_t magic = k_Magic;
    int surface = -1;
    // Value of the decode fence signalled when the planes are written.
    uint64_t decodeFenceValue = 0;
    // Highest release fence value the renderer signalled after sampling.
    std::atomic<uint64_t> releaseFenceValue {0};

    static PyroWaveFrameRef* fromFrame(const AVFrame* frame)
    {
        if (frame == nullptr || frame->buf[0] == nullptr ||
                frame->buf[0]->size != sizeof(PyroWaveFrameRef)) {
            return nullptr;
        }
        auto ref = reinterpret_cast<PyroWaveFrameRef*>(frame->buf[0]->data);
        return ref->magic == k_Magic ? ref : nullptr;
    }

    void noteRelease(uint64_t value)
    {
        uint64_t current = releaseFenceValue.load(std::memory_order_relaxed);
        while (current < value &&
               !releaseFenceValue.compare_exchange_weak(current, value, std::memory_order_release)) {
        }
    }
};
