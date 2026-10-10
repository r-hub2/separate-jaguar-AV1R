// Shared Vulkan instance creation for the AV1 video-encode path and the
// FFV1 compute path (av1r_init.cpp). Driver warnings printed during
// instance creation are suppressed there (StderrSuppressor).
#pragma once

#if defined(AV1R_USE_VULKAN) || defined(AV1R_FFV1_GPU)

#include <vulkan/vulkan.h>

VkInstance av1r_create_instance();
void       av1r_destroy_instance(VkInstance instance);

#endif
