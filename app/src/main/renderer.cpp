#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <vector>
#include <string>
#include <cassert>

#define LOG_TAG "GaussianSplat"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static inline bool vk_ok(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    LOGE("%s failed: %d", what, (int)r);
    return false;
}

class VulkanRenderer {
public:
    void init();
    void onSurfaceCreated(ANativeWindow* window);
    void onSurfaceChanged(int width, int height);
    void onSurfaceDestroyed();
    void render();

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};

    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    static constexpr uint32_t kFramesInFlight = 2;
    std::vector<VkSemaphore> imageAvailable_;
    std::vector<VkSemaphore> renderFinished_;
    std::vector<VkFence> inFlight_;
    uint32_t frameIndex_ = 0;

    void createInstance();
    void pickPhysicalDevice();
    void createDevice();
    void createSurface(ANativeWindow* window);
    void destroySurface();

    void createSwapchain(int width, int height);
    void destroySwapchain();

    void createRenderPass();
    void destroyRenderPass();

    void createFramebuffers();
    void destroyFramebuffers();

    void createCommandPool();
    void destroyCommandPool();

    void allocateCommandBuffers();
    void createSync();
    void destroySync();

    void record(VkCommandBuffer cmd, uint32_t imageIndex);
};

static VulkanRenderer g;

void VulkanRenderer::init() {
    if (instance_) return;
    createInstance();
    pickPhysicalDevice();
    createDevice();
    LOGI("Vulkan core initialized");
}

void VulkanRenderer::createInstance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "OnDeviceGaussianSplatting";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "none";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_1;

    const char* exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)(sizeof(exts) / sizeof(exts[0]));
    ci.ppEnabledExtensionNames = exts;

    vk_ok(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance");
}

void VulkanRenderer::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    assert(count > 0);
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance_, &count, devs.data());
    phys_ = devs[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys_, &props);
    LOGI("Using GPU: %s", props.deviceName);

    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qCount, qprops.data());

    queueFamily_ = 0;
    for (uint32_t i = 0; i < qCount; i++) {
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamily_ = i;
            break;
        }
    }
}

void VulkanRenderer::createDevice() {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = queueFamily_;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;

    const char* exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = (uint32_t)(sizeof(exts) / sizeof(exts[0]));
    ci.ppEnabledExtensionNames = exts;

    vk_ok(vkCreateDevice(phys_, &ci, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
}

void VulkanRenderer::createSurface(ANativeWindow* window) {
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = window;
    vk_ok(vkCreateAndroidSurfaceKHR(instance_, &ci, nullptr, &surface_), "vkCreateAndroidSurfaceKHR");
}

void VulkanRenderer::destroySurface() {
    if (!surface_) return;
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
}

void VulkanRenderer::createSwapchain(int width, int height) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmtCount, fmts.data());

    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = f;
            break;
        }
    }

    swapchainFormat_ = chosen.format;

    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pms(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pmCount, pms.data());

    VkPresentModeKHR present = VK_PRESENT_MODE_FIFO_KHR;
    for (auto pm : pms) {
        if (pm == VK_PRESENT_MODE_MAILBOX_KHR) {
            present = pm;
            break;
        }
    }

    extent_.width = (uint32_t)width;
    extent_.height = (uint32_t)height;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = present;
    ci.clipped = VK_TRUE;

    vk_ok(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "vkCreateSwapchainKHR");

    uint32_t scCount = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &scCount, nullptr);
    images_.resize(scCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &scCount, images_.data());

    imageViews_.resize(scCount);
    for (uint32_t i = 0; i < scCount; i++) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = images_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapchainFormat_;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vk_ok(vkCreateImageView(device_, &vi, nullptr, &imageViews_[i]), "vkCreateImageView");
    }

    createRenderPass();
    createFramebuffers();
    createCommandPool();
    allocateCommandBuffers();
    createSync();

    LOGI("Swapchain ready (%ux%u, %u images)", extent_.width, extent_.height, (uint32_t)images_.size());
}

void VulkanRenderer::destroySwapchain() {
    if (!device_) return;
    if (!swapchain_) return;

    vkDeviceWaitIdle(device_);

    destroySync();
    destroyCommandPool();
    destroyFramebuffers();
    destroyRenderPass();

    for (auto iv : imageViews_) vkDestroyImageView(device_, iv, nullptr);
    imageViews_.clear();
    images_.clear();

    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VulkanRenderer::createRenderPass() {
    VkAttachmentDescription color{};
    color.format = swapchainFormat_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;

    vk_ok(vkCreateRenderPass(device_, &ci, nullptr, &renderPass_), "vkCreateRenderPass");
}

void VulkanRenderer::destroyRenderPass() {
    if (!renderPass_) return;
    vkDestroyRenderPass(device_, renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
}

void VulkanRenderer::createFramebuffers() {
    framebuffers_.resize(imageViews_.size());
    for (size_t i = 0; i < imageViews_.size(); i++) {
        VkImageView atts[] = { imageViews_[i] };
        VkFramebufferCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass = renderPass_;
        ci.attachmentCount = 1;
        ci.pAttachments = atts;
        ci.width = extent_.width;
        ci.height = extent_.height;
        ci.layers = 1;
        vk_ok(vkCreateFramebuffer(device_, &ci, nullptr, &framebuffers_[i]), "vkCreateFramebuffer");
    }
}

void VulkanRenderer::destroyFramebuffers() {
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
}

void VulkanRenderer::createCommandPool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.queueFamilyIndex = queueFamily_;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vk_ok(vkCreateCommandPool(device_, &ci, nullptr, &commandPool_), "vkCreateCommandPool");
}

void VulkanRenderer::destroyCommandPool() {
    if (!commandPool_) return;
    vkDestroyCommandPool(device_, commandPool_, nullptr);
    commandPool_ = VK_NULL_HANDLE;
    commandBuffers_.clear();
}

void VulkanRenderer::allocateCommandBuffers() {
    commandBuffers_.resize(kFramesInFlight);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = commandPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (uint32_t)commandBuffers_.size();
    vk_ok(vkAllocateCommandBuffers(device_, &ai, commandBuffers_.data()), "vkAllocateCommandBuffers");
}

void VulkanRenderer::createSync() {
    imageAvailable_.resize(kFramesInFlight);
    renderFinished_.resize(kFramesInFlight);
    inFlight_.resize(kFramesInFlight);

    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        vk_ok(vkCreateSemaphore(device_, &si, nullptr, &imageAvailable_[i]), "vkCreateSemaphore(imageAvail)");
        vk_ok(vkCreateSemaphore(device_, &si, nullptr, &renderFinished_[i]), "vkCreateSemaphore(renderFinished)");
        vk_ok(vkCreateFence(device_, &fi, nullptr, &inFlight_[i]), "vkCreateFence");
    }
}

void VulkanRenderer::destroySync() {
    for (auto s : imageAvailable_) vkDestroySemaphore(device_, s, nullptr);
    for (auto s : renderFinished_) vkDestroySemaphore(device_, s, nullptr);
    for (auto f : inFlight_) vkDestroyFence(device_, f, nullptr);
    imageAvailable_.clear();
    renderFinished_.clear();
    inFlight_.clear();
    frameIndex_ = 0;
}

void VulkanRenderer::record(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vk_ok(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");

    VkClearValue clear{};
    clear.color.float32[0] = 0.07f;
    clear.color.float32[1] = 0.07f;
    clear.color.float32[2] = 0.12f;
    clear.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = renderPass_;
    rbi.framebuffer = framebuffers_[imageIndex];
    rbi.renderArea.extent = extent_;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    // TODO: splat draw pass goes here.
    vkCmdEndRenderPass(cmd);

    vk_ok(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
}

void VulkanRenderer::onSurfaceCreated(ANativeWindow* window) {
    init();
    if (surface_) return;
    createSurface(window);
}

void VulkanRenderer::onSurfaceChanged(int width, int height) {
    if (!surface_) return;
    if (swapchain_) destroySwapchain();
    createSwapchain(width, height);
}

void VulkanRenderer::onSurfaceDestroyed() {
    if (swapchain_) destroySwapchain();
    destroySurface();
}

void VulkanRenderer::render() {
    if (!swapchain_) return;

    vkWaitForFences(device_, 1, &inFlight_[frameIndex_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(
        device_, swapchain_, UINT64_MAX,
        imageAvailable_[frameIndex_], VK_NULL_HANDLE, &imageIndex);

    if (acquire == VK_ERROR_OUT_OF_DATE_KHR || acquire == VK_SUBOPTIMAL_KHR) {
        return;
    }

    vkResetFences(device_, 1, &inFlight_[frameIndex_]);

    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    vkResetCommandBuffer(cmd, 0);
    record(cmd, imageIndex);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imageAvailable_[frameIndex_];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderFinished_[frameIndex_];

    vk_ok(vkQueueSubmit(queue_, 1, &si, inFlight_[frameIndex_]), "vkQueueSubmit");

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinished_[frameIndex_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;

    vkQueuePresentKHR(queue_, &pi);

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

// -------------------------------------------------------------------------
// JNI
// -------------------------------------------------------------------------
extern "C" {

JNIEXPORT void JNICALL
Java_com_ondevice_gaussiansplatting_MainActivity_nativeOnSurfaceCreated(
        JNIEnv* env, jobject /*thiz*/, jobject surface) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        LOGE("ANativeWindow_fromSurface failed");
        return;
    }
    g.onSurfaceCreated(window);
    ANativeWindow_release(window);
}

JNIEXPORT void JNICALL
Java_com_ondevice_gaussiansplatting_MainActivity_nativeOnSurfaceChanged(
        JNIEnv* /*env*/, jobject /*thiz*/, jint width, jint height) {
    g.onSurfaceChanged((int)width, (int)height);
}

JNIEXPORT void JNICALL
Java_com_ondevice_gaussiansplatting_MainActivity_nativeOnSurfaceDestroyed(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    g.onSurfaceDestroyed();
}

JNIEXPORT void JNICALL
Java_com_ondevice_gaussiansplatting_MainActivity_nativeRender(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    g.render();
}

}
