#pragma once

#include "streaming/video/pyrowave/pyrowavesurfaces.h"

#include <libplacebo/vulkan.h>

#include <array>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

// plvk side of the PyroWave surface pool, the counterpart of
// D3D11PyroWaveSurfaces.
//
// Because libplacebo and the codec end up on one VkDevice, a surface here is a
// plain VkImage allocated on that device and wrapped back into a pl_tex with
// pl_vulkan_wrap(). Nothing is exported or imported: a VkImage handle is already
// meaningful to both sides, and so is a VkSemaphore. That is what
// PyroWaveSharingModel::SharedVulkanDevice means.
//
// The two timeline semaphores carry the same two edges as the D3D11 fences:
//
//   decode:  the codec signals it after writing a surface; libplacebo waits on it
//            through pl_vulkan_release_ex() before it samples the planes.
//   release: libplacebo signals it through pl_vulkan_hold_ex() once its last read
//            of a surface retired; the codec waits on it before overwriting.
//
// Every plane lives in VK_IMAGE_LAYOUT_GENERAL for its whole life. The codec
// writes it as a storage image and libplacebo reads it as a texture, and doing
// the transition dance per frame would buy nothing that GENERAL does not already
// give, on a path that is trying to save microseconds.
class PlVkPyroWaveSurfaces : public IPyroWaveSurfacePool
{
public:
    // Enough for the pacer's outstanding frames, the VRR worker's extra queued
    // frame, and one decode plus one render in flight. Same depth as D3D11.
    static constexpr int k_SurfaceCount = 10;

    bool initialize(pl_vulkan vulkan, pl_vk_inst instance,
                    int width, int height, bool chroma444, bool tenBit);
    ~PlVkPyroWaveSurfaces();

    PlVkPyroWaveSurfaces() = default;
    PlVkPyroWaveSurfaces(const PlVkPyroWaveSurfaces&) = delete;
    PlVkPyroWaveSurfaces& operator=(const PlVkPyroWaveSurfaces&) = delete;

    // IPyroWaveSurfacePool
    PyroWaveSharingModel pyroWaveSharingModel() const override {
        return PyroWaveSharingModel::SharedVulkanDevice;
    }
    bool pyroWaveAdapterLuid(uint8_t luid[8]) override;
    bool pyroWaveVulkanDevice(PyroWaveVulkanDevice* device) override;
    bool pyroWaveVulkanSync(VkSemaphore* decode, VkSemaphore* release) override;
    int pyroWaveSurfaceCount() const override { return (int)m_Surfaces.size(); }
    bool exportPyroWaveSurface(int index, PyroWaveSharedPlane planes[3]) override;
    // The shared device needs no OS handles at all.
    uintptr_t exportPyroWaveDecodeFence() override { return 0; }
    uintptr_t exportPyroWaveReleaseFence() override { return 0; }

    // Textures for the Y, Cb and Cr planes of a surface, or null if the index is
    // out of range.
    const std::array<pl_tex, 3>* planeTextures(int surface) const;

    // Hands a surface over to libplacebo, telling it to wait for the decode that
    // wrote it. Must happen before the frame is mapped for drawing.
    bool handToRenderer(const PyroWaveFrameRef* ref);

    // Takes a surface back after the renderer's last read and records the
    // timeline value that makes it safe for the codec to overwrite.
    bool takeFromRenderer(PyroWaveFrameRef* ref);

    // Whether a surface is currently in libplacebo's hands rather than ours, so
    // teardown can tell which wrappers still need taking back.
    bool isHeldByRenderer(int surface) const;

    // Blocks this thread until the decode that wrote the frame's planes is done,
    // which is what the VRR path asks for before it treats a frame as ready to
    // present. The GPU-side wait in handToRenderer() is the correctness mechanism;
    // this exists to keep the presenter's timing honest.
    bool waitForDecodeCpu(const PyroWaveFrameRef* ref, uint64_t timeoutNs);

    // Takes libplacebo's lock on the queue the codec submits on. The codec calls
    // these through pyrowave_device_create_info around its own submissions, from
    // the decode thread, while this thread renders.
    void lockGraphicsQueue();
    void unlockGraphicsQueue();

private:
    struct Plane {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        pl_tex texture = nullptr;
        // The renderer is expected to have taken the surface back before the
        // wrapper goes away.
        bool heldByRenderer = true;
    };

    struct Surface {
        std::array<Plane, 3> planes;
        // The same pl_tex handles, laid out the way a renderer wants to hand them
        // to pl_map_avframe_ex(): one texture per plane, in Y, Cb, Cr order.
        std::array<pl_tex, 3> textures = { nullptr, nullptr, nullptr };
    };

    bool createPlane(int surface, int planeIndex, uint32_t width, uint32_t height);
    bool transitionPlaneToGeneral(Plane& plane);
    void destroySurfaces();

    // Device entry points, resolved through the instance libplacebo opened. Kept
    // as members rather than globals: the application must not put Vulkan entry
    // points on the global symbol table, where they could interpose the loader
    // calls made by FFmpeg or libplacebo itself.
    PFN_vkGetInstanceProcAddr m_GetProcAddr = nullptr;
    PFN_vkCreateImage m_vkCreateImage = nullptr;
    PFN_vkDestroyImage m_vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements m_vkGetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory m_vkAllocateMemory = nullptr;
    PFN_vkFreeMemory m_vkFreeMemory = nullptr;
    PFN_vkBindImageMemory m_vkBindImageMemory = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties m_vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetDeviceQueue m_vkGetDeviceQueue = nullptr;
    PFN_vkCreateSemaphore m_vkCreateSemaphore = nullptr;
    PFN_vkDestroySemaphore m_vkDestroySemaphore = nullptr;
    PFN_vkCreateCommandPool m_vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool m_vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers m_vkAllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer m_vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer m_vkEndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier m_vkCmdPipelineBarrier = nullptr;
    PFN_vkQueueSubmit m_vkQueueSubmit = nullptr;
    PFN_vkQueueWaitIdle m_vkQueueWaitIdle = nullptr;
    PFN_vkGetSemaphoreCounterValue m_vkGetSemaphoreCounterValue = nullptr;
    PFN_vkWaitSemaphores m_vkWaitSemaphores = nullptr;

    pl_vulkan m_Vulkan = nullptr;
    pl_vk_inst m_Inst = nullptr;
    VkDevice m_Device = VK_NULL_HANDLE;
    VkQueue m_Queue = VK_NULL_HANDLE;
    uint32_t m_QueueFamily = 0;
    // Retained for pyrowave_create_device(), which keeps pointers to them for as
    // long as its device object exists. They describe what libplacebo actually
    // created, because that is what the codec goes looking for when it decides
    // which features it may assume.
    std::vector<const char*> m_EnabledExtensions;
    std::vector<const char*> m_EnabledInstanceExtensions;
    VkPhysicalDeviceFeatures2 m_EnabledFeatures = {};
    VkInstanceCreateInfo m_InstanceCreateInfo = {};
    VkApplicationInfo m_ApplicationInfo = {};
    VkDeviceCreateInfo m_DeviceCreateInfo = {};
    VkDeviceQueueCreateInfo m_QueueCreateInfo = {};
    float m_QueuePriority = 1.0f;

    VkCommandPool m_CommandPool = VK_NULL_HANDLE;

    VkSemaphore m_DecodeSemaphore = VK_NULL_HANDLE;
    VkSemaphore m_ReleaseSemaphore = VK_NULL_HANDLE;
    uint64_t m_ReleaseValue = 0;

    VkFormat m_PlaneFormat = VK_FORMAT_R8_UNORM;
    int m_Width = 0;
    int m_Height = 0;
    bool m_Chroma444 = false;
    std::vector<Surface> m_Surfaces;
};
