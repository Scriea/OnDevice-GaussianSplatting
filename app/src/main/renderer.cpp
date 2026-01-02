#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

#define LOG_TAG "G-Splat-Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

class VulkanRenderer {
public:
    void onSurfaceCreated(ANativeWindow* window) {
        nativeWindow = window;
        if (!instance) {
            createInstance();
        }
        if (surface) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        createSurface();
        if (!device) {
            pickPhysicalDevice();
            createDevice();
            createCommandPool();
            createSyncObjects();
        }
        recreateSwapchain();
        LOGI("Vulkan ready (swapchain images: %zu)", swapchainImages.size());
    }

    void onSurfaceChanged(int /*width*/, int /*height*/) {
        if (device && surface) {
            recreateSwapchain();
        }
    }

    void onSurfaceDestroyed() {
        cleanupSwapchain();
        if (surface) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (nativeWindow) {
            ANativeWindow_release(nativeWindow);
            nativeWindow = nullptr;
        }
    }

    void renderFrame() {
        if (!device || !swapchain) return;

        vkWaitForFences(device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &inFlightFence);

        uint32_t imageIndex = 0;
        VkResult acquireRes = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
        if (acquireRes == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }
        if (acquireRes != VK_SUCCESS && acquireRes != VK_SUBOPTIMAL_KHR) {
            LOGE("vkAcquireNextImageKHR failed: %d", acquireRes);
            return;
        }

        VkCommandBuffer cmd = commandBuffers[imageIndex];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &beginInfo);

        VkClearValue clearColor{};
        clearColor.color.float32[0] = 0.07f;
        clearColor.color.float32[1] = 0.07f;
        clearColor.color.float32[2] = 0.10f;
        clearColor.color.float32[3] = 1.0f;

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = renderPass;
        rpBegin.framebuffer = framebuffers[imageIndex];
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = swapchainExtent;
        rpBegin.clearValueCount = 1;
        rpBegin.pClearValues = &clearColor;
        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cmd);

        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &imageAvailableSemaphore;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinishedSemaphore;

        VkResult submitRes = vkQueueSubmit(graphicsQueue, 1, &submit, inFlightFence);
        if (submitRes != VK_SUCCESS) {
            LOGE("vkQueueSubmit failed: %d", submitRes);
            return;
        }

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinishedSemaphore;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;

        VkResult presentRes = vkQueuePresentKHR(presentQueue, &present);
        if (presentRes == VK_ERROR_OUT_OF_DATE_KHR || presentRes == VK_SUBOPTIMAL_KHR) {
            recreateSwapchain();
        } else if (presentRes != VK_SUCCESS) {
            LOGE("vkQueuePresentKHR failed: %d", presentRes);
        }
    }

private:
    struct QueueFamilyIndices {
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;
        bool isComplete() const { return graphicsFamily.has_value() && presentFamily.has_value(); }
    };

    ANativeWindow* nativeWindow = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainImageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;

    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;

    void createInstance() {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "OnDevice-GaussianSplatting";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "None";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = VK_API_VERSION_1_1;

        const char* extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = 2;
        createInfo.ppEnabledExtensionNames = extensions;

        VkResult res = vkCreateInstance(&createInfo, nullptr, &instance);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateInstance failed: %d", res);
        }
    }

    void createSurface() {
        VkAndroidSurfaceCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        createInfo.window = nativeWindow;
        VkResult res = vkCreateAndroidSurfaceKHR(instance, &createInfo, nullptr, &surface);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateAndroidSurfaceKHR failed: %d", res);
        }
    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice deviceToCheck) {
        QueueFamilyIndices indices;
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(deviceToCheck, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(deviceToCheck, &queueFamilyCount, families.data());

        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            const auto& family = families[i];
            if (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                indices.graphicsFamily = i;
            }

            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(deviceToCheck, i, surface, &presentSupport);
            if (presentSupport) {
                indices.presentFamily = i;
            }

            if (indices.isComplete()) break;
        }
        return indices;
    }

    bool checkDeviceExtensionSupport(VkPhysicalDevice deviceToCheck) {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(deviceToCheck, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> available(extCount);
        vkEnumerateDeviceExtensionProperties(deviceToCheck, nullptr, &extCount, available.data());

        for (const auto& ext : available) {
            if (strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) return true;
        }
        return false;
    }

    struct SwapchainSupportDetails {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice deviceToCheck) {
        SwapchainSupportDetails details;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(deviceToCheck, surface, &details.capabilities);

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(deviceToCheck, surface, &formatCount, nullptr);
        details.formats.resize(formatCount);
        if (formatCount > 0) {
            vkGetPhysicalDeviceSurfaceFormatsKHR(deviceToCheck, surface, &formatCount, details.formats.data());
        }

        uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(deviceToCheck, surface, &presentModeCount, nullptr);
        details.presentModes.resize(presentModeCount);
        if (presentModeCount > 0) {
            vkGetPhysicalDeviceSurfacePresentModesKHR(deviceToCheck, surface, &presentModeCount, details.presentModes.data());
        }

        return details;
    }

    void pickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            LOGE("No Vulkan physical devices found");
            return;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        for (auto candidate : devices) {
            if (!checkDeviceExtensionSupport(candidate)) continue;
            auto indices = findQueueFamilies(candidate);
            if (!indices.isComplete()) continue;

            auto support = querySwapchainSupport(candidate);
            if (support.formats.empty() || support.presentModes.empty()) continue;

            physicalDevice = candidate;
            break;
        }

        if (!physicalDevice) {
            LOGE("Failed to pick a suitable physical device");
        }
    }

    void createDevice() {
        auto indices = findQueueFamilies(physicalDevice);
        std::vector<uint32_t> uniqueFamilies;
        uniqueFamilies.push_back(indices.graphicsFamily.value());
        if (indices.presentFamily.value() != indices.graphicsFamily.value()) {
            uniqueFamilies.push_back(indices.presentFamily.value());
        }

        float queuePriority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        queueInfos.reserve(uniqueFamilies.size());
        for (uint32_t family : uniqueFamilies) {
            VkDeviceQueueCreateInfo qi{};
            qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qi.queueFamilyIndex = family;
            qi.queueCount = 1;
            qi.pQueuePriorities = &queuePriority;
            queueInfos.push_back(qi);
        }

        const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledExtensionCount = 1;
        createInfo.ppEnabledExtensionNames = deviceExtensions;

        VkResult res = vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateDevice failed: %d", res);
            return;
        }

        vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
        vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
    }

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
        }
        return formats[0];
    }

    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes) {
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
        if (capabilities.currentExtent.width != UINT32_MAX) {
            return capabilities.currentExtent;
        }
        int32_t w = ANativeWindow_getWidth(nativeWindow);
        int32_t h = ANativeWindow_getHeight(nativeWindow);
        VkExtent2D actual{ static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
        actual.width = std::clamp(actual.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actual.height = std::clamp(actual.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return actual;
    }

    void createSwapchain() {
        auto support = querySwapchainSupport(physicalDevice);
        VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
        VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);
        VkExtent2D extent = chooseExtent(support.capabilities);

        uint32_t imageCount = support.capabilities.minImageCount + 1;
        if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
            imageCount = support.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.preTransform = support.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;

        auto indices = findQueueFamilies(physicalDevice);
        uint32_t queueFamilyIndices[] = { indices.graphicsFamily.value(), indices.presentFamily.value() };
        if (indices.graphicsFamily.value() != indices.presentFamily.value()) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        VkResult res = vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateSwapchainKHR failed: %d", res);
            return;
        }

        swapchainImageFormat = surfaceFormat.format;
        swapchainExtent = extent;

        uint32_t actualCount = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &actualCount, nullptr);
        swapchainImages.resize(actualCount);
        vkGetSwapchainImagesKHR(device, swapchain, &actualCount, swapchainImages.data());
    }

    void createImageViews() {
        swapchainImageViews.resize(swapchainImages.size());
        for (size_t i = 0; i < swapchainImages.size(); i++) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = swapchainImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = swapchainImageFormat;
            viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            VkResult res = vkCreateImageView(device, &viewInfo, nullptr, &swapchainImageViews[i]);
            if (res != VK_SUCCESS) {
                LOGE("vkCreateImageView failed: %d", res);
            }
        }
    }

    void createRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapchainImageFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &colorAttachment;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dep;

        VkResult res = vkCreateRenderPass(device, &rpInfo, nullptr, &renderPass);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateRenderPass failed: %d", res);
        }
    }

    void createFramebuffers() {
        framebuffers.resize(swapchainImageViews.size());
        for (size_t i = 0; i < swapchainImageViews.size(); i++) {
            VkImageView attachments[] = { swapchainImageViews[i] };
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = renderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = attachments;
            fbInfo.width = swapchainExtent.width;
            fbInfo.height = swapchainExtent.height;
            fbInfo.layers = 1;

            VkResult res = vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffers[i]);
            if (res != VK_SUCCESS) {
                LOGE("vkCreateFramebuffer failed: %d", res);
            }
        }
    }

    void createCommandPool() {
        auto indices = findQueueFamilies(physicalDevice);
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = indices.graphicsFamily.value();
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        VkResult res = vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateCommandPool failed: %d", res);
        }
    }

    void createCommandBuffers() {
        commandBuffers.resize(swapchainImages.size());

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

        VkResult res = vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data());
        if (res != VK_SUCCESS) {
            LOGE("vkAllocateCommandBuffers failed: %d", res);
        }
    }

    void createSyncObjects() {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device, &semInfo, nullptr, &imageAvailableSemaphore);
        vkCreateSemaphore(device, &semInfo, nullptr, &renderFinishedSemaphore);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device, &fenceInfo, nullptr, &inFlightFence);
    }

    void cleanupSwapchain() {
        if (!device) return;
        vkDeviceWaitIdle(device);

        for (auto fb : framebuffers) {
            if (fb) vkDestroyFramebuffer(device, fb, nullptr);
        }
        framebuffers.clear();

        if (renderPass) {
            vkDestroyRenderPass(device, renderPass, nullptr);
            renderPass = VK_NULL_HANDLE;
        }

        for (auto iv : swapchainImageViews) {
            if (iv) vkDestroyImageView(device, iv, nullptr);
        }
        swapchainImageViews.clear();

        if (!commandBuffers.empty()) {
            vkFreeCommandBuffers(device, commandPool, static_cast<uint32_t>(commandBuffers.size()), commandBuffers.data());
            commandBuffers.clear();
        }

        if (swapchain) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
        swapchainImages.clear();
    }

    void recreateSwapchain() {
        if (!device || !nativeWindow) return;
        cleanupSwapchain();
        createSwapchain();
        createImageViews();
        createRenderPass();
        createFramebuffers();
        createCommandBuffers();
    }
};

VulkanRenderer gRenderer;

extern "C" JNIEXPORT void JNICALL
Java_com_ondevice_gaussiansplatting_MainActivity_nativeOnSurfaceCreated(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject surface) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    gRenderer.onSurfaceCreated(window);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ondevice_gaussiansplatting_MainActivity_nativeOnSurfaceChanged(
        JNIEnv* /*env*/,
        jobject /*thiz*/,
        jint width,
        jint height) {
    gRenderer.onSurfaceChanged(width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ondevice_gaussiansplatting_MainActivity_nativeOnSurfaceDestroyed(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {
    gRenderer.onSurfaceDestroyed();
}

extern "C" JNIEXPORT void JNICALL
Java_com_ondevice_gaussiansplatting_MainActivity_nativeRender(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {
    gRenderer.renderFrame();
}