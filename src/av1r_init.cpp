// Vulkan instance initialisation for AV1R, shared by the AV1 video-encode
// path (AV1R_USE_VULKAN) and the FFV1 compute path (AV1R_FFV1_GPU).

#if defined(AV1R_USE_VULKAN) || defined(AV1R_FFV1_GPU)

#include <vulkan/vulkan.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include "av1r_vk_instance.h"

#include "av1r_stderr_suppress.h"

VkInstance av1r_create_instance()
{
    VkApplicationInfo appInfo{};
    appInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "AV1R";
    appInfo.apiVersion       = VK_API_VERSION_1_3;

    const char* extensions[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &appInfo;
    ci.enabledExtensionCount   = 1;
    ci.ppEnabledExtensionNames = extensions;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult res;
    {
        StderrSuppressor quiet;
        res = vkCreateInstance(&ci, nullptr, &instance);
    }
    if (res != VK_SUCCESS) {
        throw std::runtime_error("vkCreateInstance failed: " + std::to_string(res));
    }
    return instance;
}

void av1r_destroy_instance(VkInstance instance)
{
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
    }
}

#endif // AV1R_USE_VULKAN || AV1R_FFV1_GPU
