#include "plvkpyrowave.h"

#include <SDL.h>

#include <algorithm>

// The planes are written by a compute (or fragment) shader and read as textures.
static const VkImageUsageFlags k_PlaneUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

// libplacebo requires external users of its VkQueues to take its own queue lock
// around submission. The codec submits decode work from the decode thread while
// this thread renders, so it has to be routed through the same lock rather than
// straight to vkQueueSubmit.
static void plvkQueueLock(void* userdata)
{
    static_cast<PlVkPyroWaveSurfaces*>(userdata)->lockGraphicsQueue();
}

static void plvkQueueUnlock(void* userdata)
{
    static_cast<PlVkPyroWaveSurfaces*>(userdata)->unlockGraphicsQueue();
}

bool PlVkPyroWaveSurfaces::initialize(pl_vulkan vulkan, pl_vk_inst inst,
                                      int width, int height, bool chroma444, bool tenBit)
{
    m_Vulkan = vulkan;
    m_Inst = inst;
    if (m_Vulkan == nullptr || m_Inst == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    m_Device = m_Vulkan->device;
    m_GetProcAddr = m_Vulkan->get_proc_addr;
    m_Width = width;
    m_Height = height;
    m_Chroma444 = chroma444;
    m_PlaneFormat = tenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;

    // Resolve every entry point we need through the instance libplacebo opened.
    // None of them may end up as a global symbol: this process also contains
    // FFmpeg and libplacebo, and an interposing vk* symbol would be a spectacular
    // thing to debug.
#define PLVK_PYROWAVE_PFN(name) \
    m_##name = reinterpret_cast<PFN_##name>(m_GetProcAddr(m_Inst->instance, #name)); \
    if (m_##name == nullptr) { \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: %s is unavailable", #name); \
        return false; \
    }
    PLVK_PYROWAVE_PFN(vkCreateImage)
    PLVK_PYROWAVE_PFN(vkDestroyImage)
    PLVK_PYROWAVE_PFN(vkGetImageMemoryRequirements)
    PLVK_PYROWAVE_PFN(vkAllocateMemory)
    PLVK_PYROWAVE_PFN(vkFreeMemory)
    PLVK_PYROWAVE_PFN(vkBindImageMemory)
    PLVK_PYROWAVE_PFN(vkGetPhysicalDeviceMemoryProperties)
    PLVK_PYROWAVE_PFN(vkGetDeviceQueue)
    PLVK_PYROWAVE_PFN(vkCreateSemaphore)
    PLVK_PYROWAVE_PFN(vkDestroySemaphore)
    PLVK_PYROWAVE_PFN(vkCreateCommandPool)
    PLVK_PYROWAVE_PFN(vkDestroyCommandPool)
    PLVK_PYROWAVE_PFN(vkAllocateCommandBuffers)
    PLVK_PYROWAVE_PFN(vkBeginCommandBuffer)
    PLVK_PYROWAVE_PFN(vkEndCommandBuffer)
    PLVK_PYROWAVE_PFN(vkCmdPipelineBarrier)
    PLVK_PYROWAVE_PFN(vkQueueSubmit)
    PLVK_PYROWAVE_PFN(vkQueueWaitIdle)
    PLVK_PYROWAVE_PFN(vkGetSemaphoreCounterValue)
    PLVK_PYROWAVE_PFN(vkWaitSemaphores)
#undef PLVK_PYROWAVE_PFN

    m_QueueFamily = m_Vulkan->queue_graphics.index;
    m_vkGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_Queue);
    if (m_Queue == VK_NULL_HANDLE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: no graphics queue to decode on");
        return false;
    }

    VkCommandPoolCreateInfo cmdPoolInfo = {};
    cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cmdPoolInfo.queueFamilyIndex = m_QueueFamily;
    if (m_vkCreateCommandPool(m_Device, &cmdPoolInfo, nullptr, &m_CommandPool) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: creating a command pool failed");
        return false;
    }

    // The codec reads the enabled extensions and features back out of the create
    // infos to decide what it may assume, so those have to describe the device
    // libplacebo actually made rather than what we would have asked for.
    for (int i = 0; i < m_Inst->num_extensions; i++) {
        m_EnabledInstanceExtensions.push_back(m_Inst->extensions[i]);
    }
    for (int i = 0; i < m_Vulkan->num_extensions; i++) {
        // libplacebo says this list may contain duplicates. vkCreateDevice would
        // have rejected those, so drop them on the way into the create info.
        const char* name = m_Vulkan->extensions[i];
        if (std::find(m_EnabledExtensions.begin(), m_EnabledExtensions.end(), name) == m_EnabledExtensions.end()) {
            m_EnabledExtensions.push_back(name);
        }
    }
    // Our own copy of what libplacebo enabled, so the create info points at
    // something that belongs to us. The pNext chain it copies still belongs to
    // libplacebo and lives as long as the device, which is the lifetime we need.
    m_EnabledFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    if (m_Vulkan->features != nullptr) {
        m_EnabledFeatures = *m_Vulkan->features;
        m_EnabledFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    }

    m_ApplicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    m_ApplicationInfo.apiVersion = m_Vulkan->api_version;
    m_ApplicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    m_ApplicationInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    m_InstanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    m_InstanceCreateInfo.pApplicationInfo = &m_ApplicationInfo;
    m_InstanceCreateInfo.enabledExtensionCount = (uint32_t)m_EnabledInstanceExtensions.size();
    m_InstanceCreateInfo.ppEnabledExtensionNames = m_EnabledInstanceExtensions.data();

    m_QueuePriority = 1.0f;
    m_QueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    m_QueueCreateInfo.queueFamilyIndex = m_QueueFamily;
    m_QueueCreateInfo.queueCount = 1;
    m_QueueCreateInfo.pQueuePriorities = &m_QueuePriority;
    m_DeviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    m_DeviceCreateInfo.pNext = &m_EnabledFeatures;
    m_DeviceCreateInfo.enabledExtensionCount = (uint32_t)m_EnabledExtensions.size();
    m_DeviceCreateInfo.ppEnabledExtensionNames = m_EnabledExtensions.data();
    m_DeviceCreateInfo.pQueueCreateInfos = &m_QueueCreateInfo;
    m_DeviceCreateInfo.queueCreateInfoCount = 1;

    VkSemaphoreTypeCreateInfo timelineInfo = {};
    timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue = 0;
    VkSemaphoreCreateInfo semInfo = {};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semInfo.pNext = &timelineInfo;

    if (m_vkCreateSemaphore(m_Device, &semInfo, nullptr, &m_DecodeSemaphore) != VK_SUCCESS ||
            m_vkCreateSemaphore(m_Device, &semInfo, nullptr, &m_ReleaseSemaphore) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: creating the shared timelines failed");
        return false;
    }

    const uint32_t chromaWidth = chroma444 ? (uint32_t)width : ((uint32_t)width + 1) / 2;
    const uint32_t chromaHeight = chroma444 ? (uint32_t)height : ((uint32_t)height + 1) / 2;

    m_Surfaces.resize(k_SurfaceCount);
    for (int surface = 0; surface < k_SurfaceCount; surface++) {
        for (int plane = 0; plane < 3; plane++) {
            const uint32_t planeWidth = plane == 0 ? (uint32_t)width : chromaWidth;
            const uint32_t planeHeight = plane == 0 ? (uint32_t)height : chromaHeight;
            if (!createPlane(surface, plane, planeWidth, planeHeight)) {
                return false;
            }
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave: %d surfaces of %dx%d %s %s planes on the renderer's VkDevice",
                k_SurfaceCount, width, height, chroma444 ? "4:4:4" : "4:2:0",
                tenBit ? "16-bit" : "8-bit");
    return true;
}

bool PlVkPyroWaveSurfaces::createPlane(int surface, int planeIndex, uint32_t width, uint32_t height)
{
    Plane& plane = m_Surfaces[surface].planes[planeIndex];

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    // The wrapper libplacebo builds may view the image through another format.
    imageInfo.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = m_PlaneFormat;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Concurrent, because libplacebo may sample from a queue family other than the
    // one we decode on. With concurrent sharing there is no ownership transfer for
    // either side to perform on the frame path.
    imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
    const uint32_t families[] = {
        m_Vulkan->queue_graphics.index,
        m_Vulkan->queue_compute.index,
        m_Vulkan->queue_transfer.index,
    };
    imageInfo.queueFamilyIndexCount = sizeof(families) / sizeof(families[0]);
    imageInfo.pQueueFamilyIndices = families;
    imageInfo.usage = k_PlaneUsage;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (m_vkCreateImage(m_Device, &imageInfo, nullptr, &plane.image) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: creating a %ux%u plane image failed", width, height);
        plane.image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements memReq = {};
    m_vkGetImageMemoryRequirements(m_Device, plane.image, &memReq);

    VkPhysicalDeviceMemoryProperties memProps = {};
    m_vkGetPhysicalDeviceMemoryProperties(m_Vulkan->phys_device, &memProps);

    bool found = false;
    uint32_t memoryType = 0;
    for (; memoryType < memProps.memoryTypeCount; memoryType++) {
        if ((memReq.memoryTypeBits & (1u << memoryType)) &&
                (memProps.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            found = true;
            break;
        }
    }
    if (!found) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: no device local memory for the planes");
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryType;
    if (m_vkAllocateMemory(m_Device, &allocInfo, nullptr, &plane.memory) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: allocating %llu bytes of plane memory failed",
                     (unsigned long long)memReq.size);
        plane.memory = VK_NULL_HANDLE;
        return false;
    }

    if (m_vkBindImageMemory(m_Device, plane.image, plane.memory, 0) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: binding plane memory failed");
        return false;
    }

    if (!transitionPlaneToGeneral(plane)) {
        return false;
    }

    pl_vulkan_wrap_params wrapParams = {};
    wrapParams.image = plane.image;
    wrapParams.width = (int)width;
    wrapParams.height = (int)height;
    wrapParams.format = m_PlaneFormat;
    wrapParams.usage = k_PlaneUsage;

    plane.texture = pl_vulkan_wrap(m_Vulkan->gpu, &wrapParams);
    if (plane.texture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: wrapping plane %d of surface %d as a pl_tex failed", planeIndex, surface);
        return false;
    }
    m_Surfaces[surface].textures[planeIndex] = plane.texture;

    // A fresh wrapper counts as held by us; the renderer only gets a surface
    // through handToRenderer().
    plane.heldByRenderer = false;
    return true;
}

// One transition per plane, once, at startup. After this an image never leaves
// VK_IMAGE_LAYOUT_GENERAL, so neither the codec nor the renderer has to own a
// layout change on the frame path, which is where the microseconds matter.
bool PlVkPyroWaveSurfaces::transitionPlaneToGeneral(Plane& plane)
{
    VkCommandBufferAllocateInfo cmdInfo = {};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool = m_CommandPool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (m_vkAllocateCommandBuffers(m_Device, &cmdInfo, &cmd) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: allocating a setup command buffer failed");
        return false;
    }

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (m_vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: beginning a setup command buffer failed");
        return false;
    }

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = plane.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    m_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);

    if (m_vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: ending a setup command buffer failed");
        return false;
    }

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    m_Vulkan->lock_queue(m_Vulkan, m_QueueFamily, 0);
    VkResult result = m_vkQueueSubmit(m_Queue, 1, &submit, VK_NULL_HANDLE);
    if (result == VK_SUCCESS) {
        result = m_vkQueueWaitIdle(m_Queue);
    }
    m_Vulkan->unlock_queue(m_Vulkan, m_QueueFamily, 0);

    if (result != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: putting a plane in GENERAL layout failed: %d", result);
        return false;
    }
    return true;
}

void PlVkPyroWaveSurfaces::lockGraphicsQueue()
{
    if (m_Vulkan != nullptr && m_Vulkan->lock_queue != nullptr) {
        m_Vulkan->lock_queue(m_Vulkan, m_QueueFamily, 0);
    }
}

void PlVkPyroWaveSurfaces::unlockGraphicsQueue()
{
    if (m_Vulkan != nullptr && m_Vulkan->unlock_queue != nullptr) {
        m_Vulkan->unlock_queue(m_Vulkan, m_QueueFamily, 0);
    }
}

PlVkPyroWaveSurfaces::~PlVkPyroWaveSurfaces()
{
    // The renderer goes away with the stream, so anything still in its hands is
    // not going to be read again. Take it back anyway: a pl_tex wrapper must not
    // be destroyed while the image is not held by us.
    if (m_Vulkan != nullptr) {
        for (size_t surface = 0; surface < m_Surfaces.size(); surface++) {
            for (int plane = 0; plane < 3; plane++) {
                Plane& p = m_Surfaces[surface].planes[plane];
                m_Surfaces[surface].textures[plane] = nullptr;
                if (p.texture == nullptr) {
                    continue;
                }
                if (p.heldByRenderer) {
                    pl_vulkan_hold_params holdParams = {};
                    holdParams.tex = p.texture;
                    holdParams.layout = VK_IMAGE_LAYOUT_GENERAL;
                    holdParams.qf = VK_QUEUE_FAMILY_IGNORED;
                    holdParams.semaphore.sem = m_ReleaseSemaphore;
                    holdParams.semaphore.value = ++m_ReleaseValue;
                    pl_vulkan_hold_ex(m_Vulkan->gpu, &holdParams);
                    p.heldByRenderer = false;
                }
                pl_tex_destroy(m_Vulkan->gpu, &p.texture);
            }
        }
    }

    destroySurfaces();
}

void PlVkPyroWaveSurfaces::destroySurfaces()
{
    for (auto& surface : m_Surfaces) {
        for (auto& plane : surface.planes) {
            if (plane.memory != VK_NULL_HANDLE) {
                m_vkFreeMemory(m_Device, plane.memory, nullptr);
                plane.memory = VK_NULL_HANDLE;
            }
            if (plane.image != VK_NULL_HANDLE) {
                m_vkDestroyImage(m_Device, plane.image, nullptr);
                plane.image = VK_NULL_HANDLE;
            }
        }
    }
    m_Surfaces.clear();

    if (m_DecodeSemaphore != VK_NULL_HANDLE) {
        m_vkDestroySemaphore(m_Device, m_DecodeSemaphore, nullptr);
        m_DecodeSemaphore = VK_NULL_HANDLE;
    }
    if (m_ReleaseSemaphore != VK_NULL_HANDLE) {
        m_vkDestroySemaphore(m_Device, m_ReleaseSemaphore, nullptr);
        m_ReleaseSemaphore = VK_NULL_HANDLE;
    }
    if (m_CommandPool != VK_NULL_HANDLE) {
        m_vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
        m_CommandPool = VK_NULL_HANDLE;
    }
}

bool PlVkPyroWaveSurfaces::pyroWaveAdapterLuid(uint8_t luid[8])
{
    // On this path the codec does not pick a device by adapter identity; there is
    // no adapter identity to report, only the device it was handed.
    (void)luid;
    return false;
}

bool PlVkPyroWaveSurfaces::pyroWaveVulkanDevice(PyroWaveVulkanDevice* device)
{
    if (device == nullptr || m_Device == VK_NULL_HANDLE) {
        return false;
    }

    device->instance = m_Inst->instance;
    device->physicalDevice = m_Vulkan->phys_device;
    device->device = m_Device;
    device->getInstanceProcAddr = m_GetProcAddr;
    device->instanceCreateInfo = &m_InstanceCreateInfo;
    device->deviceCreateInfo = &m_DeviceCreateInfo;
    device->queue = m_Queue;
    device->queueFamily = m_QueueFamily;
    device->queueIndex = 0;
    device->queueLock = &plvkQueueLock;
    device->queueUnlock = &plvkQueueUnlock;
    device->queueLockUserdata = this;
    return true;
}

bool PlVkPyroWaveSurfaces::pyroWaveVulkanSync(VkSemaphore* decode, VkSemaphore* release)
{
    if (m_DecodeSemaphore == VK_NULL_HANDLE || m_ReleaseSemaphore == VK_NULL_HANDLE) {
        return false;
    }
    if (decode != nullptr) {
        *decode = m_DecodeSemaphore;
    }
    if (release != nullptr) {
        *release = m_ReleaseSemaphore;
    }
    return true;
}

bool PlVkPyroWaveSurfaces::exportPyroWaveSurface(int index, PyroWaveSharedPlane planes[3])
{
    if (index < 0 || index >= (int)m_Surfaces.size()) {
        return false;
    }

    const bool tenBit = m_PlaneFormat == VK_FORMAT_R16_UNORM;
    const uint32_t chromaWidth = m_Chroma444 ? (uint32_t)m_Width : ((uint32_t)m_Width + 1) / 2;
    const uint32_t chromaHeight = m_Chroma444 ? (uint32_t)m_Height : ((uint32_t)m_Height + 1) / 2;

    for (int plane = 0; plane < 3; plane++) {
        planes[plane].handle = reinterpret_cast<uintptr_t>(m_Surfaces[index].planes[plane].image);
        planes[plane].width = plane == 0 ? (uint32_t)m_Width : chromaWidth;
        planes[plane].height = plane == 0 ? (uint32_t)m_Height : chromaHeight;
        planes[plane].format = tenBit ? PyroWavePlaneFormat::R16Unorm : PyroWavePlaneFormat::R8Unorm;
    }

    return true;
}

const std::array<pl_tex, 3>* PlVkPyroWaveSurfaces::planeTextures(int surface) const
{
    if (surface < 0 || surface >= (int)m_Surfaces.size()) {
        return nullptr;
    }
    if (m_Surfaces[surface].textures[0] == nullptr) {
        return nullptr;
    }
    return &m_Surfaces[surface].textures;
}

bool PlVkPyroWaveSurfaces::isHeldByRenderer(int surface) const
{
    if (surface < 0 || surface >= (int)m_Surfaces.size()) {
        return false;
    }
    return m_Surfaces[surface].planes[0].heldByRenderer;
}

bool PlVkPyroWaveSurfaces::waitForDecodeCpu(const PyroWaveFrameRef* ref, uint64_t timeoutNs)
{
    if (ref == nullptr || m_DecodeSemaphore == VK_NULL_HANDLE) {
        return false;
    }

    // Already there? This is the common case at 120 Hz, and it keeps us off the
    // driver's wait path entirely.
    uint64_t completed = 0;
    if (m_vkGetSemaphoreCounterValue(m_Device, m_DecodeSemaphore, &completed) == VK_SUCCESS &&
            completed >= ref->decodeFenceValue) {
        return true;
    }

    VkSemaphoreWaitInfo waitInfo = {};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &m_DecodeSemaphore;
    waitInfo.pValues = &ref->decodeFenceValue;

    const VkResult result = m_vkWaitSemaphores(m_Device, &waitInfo, timeoutNs);
    if (result != VK_SUCCESS && result != VK_TIMEOUT) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: waiting for the decode failed: %d (target=%llu)",
                     result, (unsigned long long)ref->decodeFenceValue);
        return false;
    }
    return result == VK_SUCCESS;
}

bool PlVkPyroWaveSurfaces::handToRenderer(const PyroWaveFrameRef* ref)
{
    if (ref == nullptr || ref->surface < 0 || ref->surface >= (int)m_Surfaces.size()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: frame references an unknown surface");
        return false;
    }

    // The images are libplacebo's property now, and it has to wait for the decode
    // that filled them before it touches them. There is nothing for this to fail
    // on - libplacebo returns void here precisely because handing an image over
    // cannot go wrong - so the wait is simply recorded.
    for (int plane = 0; plane < 3; plane++) {
        Plane& target = m_Surfaces[ref->surface].planes[plane];
        pl_vulkan_release_params releaseParams = {};
        releaseParams.tex = target.texture;
        releaseParams.layout = VK_IMAGE_LAYOUT_GENERAL;
        releaseParams.qf = VK_QUEUE_FAMILY_IGNORED;
        releaseParams.semaphore.sem = m_DecodeSemaphore;
        releaseParams.semaphore.value = ref->decodeFenceValue;

        pl_vulkan_release_ex(m_Vulkan->gpu, &releaseParams);
        target.heldByRenderer = true;
    }

    return true;
}

bool PlVkPyroWaveSurfaces::takeFromRenderer(PyroWaveFrameRef* ref)
{
    if (ref == nullptr || ref->surface < 0 || ref->surface >= (int)m_Surfaces.size()) {
        return false;
    }

    // Every plane gets its own timeline value: signalling a Vulkan timeline
    // semaphore twice with the same value is not legal. Waiting for the last value
    // of a surface covers the other two, because they were submitted after this
    // thread's own rendering on the same queue.
    uint64_t value = ref->releaseFenceValue.load(std::memory_order_acquire);
    for (int plane = 0; plane < 3; plane++) {
        Plane& target = m_Surfaces[ref->surface].planes[plane];
        value = ++m_ReleaseValue;

        pl_vulkan_hold_params holdParams = {};
        holdParams.tex = target.texture;
        holdParams.layout = VK_IMAGE_LAYOUT_GENERAL;
        holdParams.qf = VK_QUEUE_FAMILY_IGNORED;
        // Fires once the renderer's last read of this plane retired.
        holdParams.semaphore.sem = m_ReleaseSemaphore;
        holdParams.semaphore.value = value;

        if (!pl_vulkan_hold_ex(m_Vulkan->gpu, &holdParams)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: taking surface %d plane %d back failed (value=%llu)",
                         ref->surface, plane, (unsigned long long)value);
            return false;
        }
        target.heldByRenderer = false;
    }

    ref->noteRelease(value);
    return true;
}
