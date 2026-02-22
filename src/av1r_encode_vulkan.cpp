// Vulkan AV1 video encoding for AV1R
// Структура адаптирована из vulkan-video-encode-simple-main/videoencoder.cpp (H.264 → AV1)
// Compiled only when AV1R_VULKAN_VIDEO_AV1 is defined

#ifdef AV1R_VULKAN_VIDEO_AV1

#include <vulkan/vulkan.h>
#include <vk_video/vulkan_video_codec_av1std.h>
#include <vk_video/vulkan_video_codec_av1std_encode.h>
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <cstring>
#include <limits>
#include "av1r_vulkan_ctx.h"

// av1r_commands.cpp
VkCommandPool   av1r_create_command_pool(VkDevice, uint32_t);
VkCommandBuffer av1r_alloc_command_buffer(VkDevice, VkCommandPool);
void            av1r_begin_command_buffer(VkCommandBuffer);
void            av1r_end_command_buffer(VkCommandBuffer);
VkFence         av1r_create_fence(VkDevice);
void            av1r_wait_fence(VkDevice, VkFence);
void            av1r_reset_fence(VkDevice, VkFence);

// av1r_memory.cpp
Av1rBuffer av1r_buffer_create(VkPhysicalDevice, VkDevice, size_t,
                               VkBufferUsageFlags, VkMemoryPropertyFlags,
                               VkMemoryPropertyFlags);
void       av1r_buffer_destroy(VkDevice, Av1rBuffer&);

// ============================================================================
// Вспомогательные функции
// ============================================================================

// Найти тип памяти (из av1r_memory.cpp паттерна)
static uint32_t find_mem_type(VkPhysicalDevice phys,
                               uint32_t type_bits,
                               VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

// ============================================================================
// Av1rEncoder — инкапсулирует состояние Vulkan AV1 сессии
// Аналог класса VideoEncoder из vulkan-video-encode-simple-main
// ============================================================================
struct Av1rEncoder {
    // Vulkan объекты — не владеем, берём из Av1rVulkanCtx
    VkPhysicalDevice physDevice  = VK_NULL_HANDLE;
    VkDevice         device      = VK_NULL_HANDLE;
    VkQueue          encodeQueue = VK_NULL_HANDLE;
    uint32_t         encodeQFam  = UINT32_MAX;

    // Video session
    VkVideoSessionKHR           videoSession           = VK_NULL_HANDLE;
    VkVideoSessionParametersKHR videoSessionParameters = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> sessionMemory;

    // DPB (Decoded Picture Buffer) — ping-pong, как в примере строки 738-744
    static constexpr uint32_t DPB_COUNT = 2;
    VkImage     dpbImages[DPB_COUNT]     = {};
    VkImageView dpbImageViews[DPB_COUNT] = {};
    VkDeviceMemory dpbMemory[DPB_COUNT]  = {};

    // Промежуточный NV12 образ — вход для encode
    VkImage        srcImage     = VK_NULL_HANDLE;
    VkImageView    srcImageView = VK_NULL_HANDLE;
    VkDeviceMemory srcMemory    = VK_NULL_HANDLE;

    // Bitstream output buffer (GPU→CPU)
    VkBuffer       bitstreamBuf    = VK_NULL_HANDLE;
    VkDeviceMemory bitstreamMemory = VK_NULL_HANDLE;
    void*          bitstreamPtr    = nullptr;
    static constexpr VkDeviceSize BITSTREAM_BUF_SIZE = 8 * 1024 * 1024; // 8 MB per frame

    // Query pool для получения размера bitstream (строки 464-476 примера)
    VkQueryPool queryPool = VK_NULL_HANDLE;

    // Command pool + fence
    VkCommandPool encodeCommandPool = VK_NULL_HANDLE;
    VkFence       encodeFence       = VK_NULL_HANDLE;

    // Семафор между очередями (если compute и encode — разные очереди)
    VkSemaphore interQueueSemaphore = VK_NULL_HANDLE;

    // Параметры
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t fps    = 0;
    uint32_t frameCount = 0;

    // AV1 codec state (аналог m_sps/m_pps из примера)
    StdVideoAV1SequenceHeader seqHeader{};

    // Rate control (из initRateControl примера)
    VkVideoEncodeRateControlInfoKHR     rateControlInfo{};
    VkVideoEncodeRateControlLayerInfoKHR rateControlLayer{};
    VkVideoEncodeAV1RateControlInfoKHR  av1RateControlInfo{};
    VkVideoEncodeAV1RateControlLayerInfoKHR av1RateControlLayer{};
    VkVideoEncodeRateControlModeFlagBitsKHR chosenRateControlMode =
        VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;

    // Выбранный формат (определяется при createVideoSession)
    VkFormat srcFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; // NV12
    VkFormat dpbFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

    VkVideoProfileInfoKHR     videoProfile{};
    VkVideoProfileListInfoKHR videoProfileList{};
    VkVideoEncodeAV1ProfileInfoKHR av1ProfileInfo{};

    bool initialized = false;
};

// ============================================================================
// createVideoSession
// Адаптировано из VideoEncoder::createVideoSession() строки 139-250
// H.264 profile/capabilities → AV1 profile/capabilities
// ============================================================================
static void createVideoSession(Av1rEncoder& enc, int crf)
{
    // AV1 profile (аналог строки 140-148 примера)
    enc.av1ProfileInfo.sType      = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR;
    enc.av1ProfileInfo.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;

    enc.videoProfile.sType               = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    enc.videoProfile.pNext               = &enc.av1ProfileInfo;
    enc.videoProfile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    enc.videoProfile.chromaSubsampling   = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    enc.videoProfile.lumaBitDepth        = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    enc.videoProfile.chromaBitDepth      = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

    enc.videoProfileList.sType        = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    enc.videoProfileList.profileCount = 1;
    enc.videoProfileList.pProfiles    = &enc.videoProfile;

    // Запрос capabilities (строки 154-174 примера, AV1 структуры)
    VkVideoEncodeAV1CapabilitiesKHR av1Caps{};
    av1Caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR;

    VkVideoEncodeCapabilitiesKHR encodeCaps{};
    encodeCaps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;
    encodeCaps.pNext = &av1Caps;

    VkVideoCapabilitiesKHR caps{};
    caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    caps.pNext = &encodeCaps;

    VkResult res = vkGetPhysicalDeviceVideoCapabilitiesKHR(enc.physDevice,
                                                           &enc.videoProfile, &caps);
    if (res != VK_SUCCESS)
        throw std::runtime_error("vkGetPhysicalDeviceVideoCapabilitiesKHR failed: " +
                                 std::to_string(res));

    // Выбор rate control mode (строки 167-174 примера)
    if (encodeCaps.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR)
        enc.chosenRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
    else if (encodeCaps.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR)
        enc.chosenRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    else if (encodeCaps.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR)
        enc.chosenRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;

    // Определение поддерживаемых форматов (строки 190-235 примера)
    VkPhysicalDeviceVideoFormatInfoKHR fmtInfo{};
    fmtInfo.sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
    fmtInfo.pNext       = &enc.videoProfileList;
    fmtInfo.imageUsage  = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceVideoFormatPropertiesKHR(enc.physDevice, &fmtInfo, &fmtCount, nullptr);
    std::vector<VkVideoFormatPropertiesKHR> fmtProps(fmtCount);
    for (auto& f : fmtProps) f.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    vkGetPhysicalDeviceVideoFormatPropertiesKHR(enc.physDevice, &fmtInfo,
                                                &fmtCount, fmtProps.data());

    enc.srcFormat = VK_FORMAT_UNDEFINED;
    for (const auto& f : fmtProps) {
        if (f.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
            f.format == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM) {
            enc.srcFormat = f.format;
            break;
        }
    }
    if (enc.srcFormat == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("No supported NV12/YUV420 format for AV1 encode src");

    // DPB формат (строки 223-235 примера)
    fmtInfo.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;
    vkGetPhysicalDeviceVideoFormatPropertiesKHR(enc.physDevice, &fmtInfo, &fmtCount, nullptr);
    std::vector<VkVideoFormatPropertiesKHR> dpbFmtProps(fmtCount);
    for (auto& f : dpbFmtProps) f.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    vkGetPhysicalDeviceVideoFormatPropertiesKHR(enc.physDevice, &fmtInfo,
                                                &fmtCount, dpbFmtProps.data());
    if (dpbFmtProps.empty())
        throw std::runtime_error("No supported DPB format for AV1 encode");
    enc.dpbFormat = dpbFmtProps[0].format;

    // AV1 sequence header (аналог SPS/PPS, строки 237-249 примера)
    static const VkExtensionProperties av1StdExt = {
        VK_STD_VULKAN_VIDEO_CODEC_AV1_ENCODE_EXTENSION_NAME,
        VK_STD_VULKAN_VIDEO_CODEC_AV1_ENCODE_SPEC_VERSION
    };

    VkVideoSessionCreateInfoKHR sessionCI{};
    sessionCI.sType                       = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    sessionCI.pVideoProfile               = &enc.videoProfile;
    sessionCI.queueFamilyIndex            = enc.encodeQFam;
    sessionCI.pictureFormat               = enc.srcFormat;
    sessionCI.maxCodedExtent              = {enc.width, enc.height};
    sessionCI.maxDpbSlots                 = Av1rEncoder::DPB_COUNT + 1;
    sessionCI.maxActiveReferencePictures  = Av1rEncoder::DPB_COUNT;
    sessionCI.referencePictureFormat      = enc.dpbFormat;
    sessionCI.pStdHeaderVersion           = &av1StdExt;

    res = vkCreateVideoSessionKHR(enc.device, &sessionCI, nullptr, &enc.videoSession);
    if (res != VK_SUCCESS)
        throw std::runtime_error("vkCreateVideoSessionKHR failed: " + std::to_string(res));
}

// ============================================================================
// allocateVideoSessionMemory
// Прямой перенос из VideoEncoder::allocateVideoSessionMemory() строки 252-288
// ============================================================================
static void allocateVideoSessionMemory(Av1rEncoder& enc)
{
    uint32_t count = 0;
    vkGetVideoSessionMemoryRequirementsKHR(enc.device, enc.videoSession, &count, nullptr);

    std::vector<VkVideoSessionMemoryRequirementsKHR> reqs(count);
    for (auto& r : reqs) r.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
    vkGetVideoSessionMemoryRequirementsKHR(enc.device, enc.videoSession, &count, reqs.data());

    enc.sessionMemory.resize(count, VK_NULL_HANDLE);
    std::vector<VkBindVideoSessionMemoryInfoKHR> binds(count);

    for (uint32_t i = 0; i < count; i++) {
        const auto& mr = reqs[i].memoryRequirements;
        uint32_t mt = find_mem_type(enc.physDevice, mr.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX)
            throw std::runtime_error("No device-local memory for video session");

        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = mr.size;
        ai.memoryTypeIndex = mt;
        vkAllocateMemory(enc.device, &ai, nullptr, &enc.sessionMemory[i]);

        binds[i].sType            = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
        binds[i].memoryBindIndex  = reqs[i].memoryBindIndex;
        binds[i].memory           = enc.sessionMemory[i];
        binds[i].memoryOffset     = 0;
        binds[i].memorySize       = mr.size;
    }
    vkBindVideoSessionMemoryKHR(enc.device, enc.videoSession, count, binds.data());
}

// ============================================================================
// createVideoSessionParameters (AV1 sequence header вместо SPS/PPS)
// Аналог строк 290-318 примера
// ============================================================================
static void createVideoSessionParameters(Av1rEncoder& enc)
{
    // AV1 sequence header — аналог SPS в H.264
    memset(&enc.seqHeader, 0, sizeof(enc.seqHeader));
    enc.seqHeader.flags.film_grain_params_present = 0;
    enc.seqHeader.flags.frame_id_numbers_present_flag = 0;
    enc.seqHeader.seq_profile = STD_VIDEO_AV1_PROFILE_MAIN;
    enc.seqHeader.frame_width_bits_minus_1  = 15; // 16 бит max для размера кадра
    enc.seqHeader.frame_height_bits_minus_1 = 15;
    enc.seqHeader.max_frame_width_minus_1   = enc.width  - 1;
    enc.seqHeader.max_frame_height_minus_1  = enc.height - 1;

    VkVideoEncodeAV1SessionParametersCreateInfoKHR av1ParamsCI{};
    av1ParamsCI.sType             = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR;
    av1ParamsCI.pStdSequenceHeader = &enc.seqHeader;

    VkVideoSessionParametersCreateInfoKHR paramsCI{};
    paramsCI.sType                       = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
    paramsCI.pNext                       = &av1ParamsCI;
    paramsCI.videoSessionParametersTemplate = VK_NULL_HANDLE;
    paramsCI.videoSession                = enc.videoSession;

    VkResult res = vkCreateVideoSessionParametersKHR(enc.device, &paramsCI,
                                                     nullptr, &enc.videoSessionParameters);
    if (res != VK_SUCCESS)
        throw std::runtime_error("vkCreateVideoSessionParametersKHR failed: " +
                                 std::to_string(res));
}

// ============================================================================
// Выделение VkImage с памятью (DPB и src образы)
// ============================================================================
static void createImage(VkPhysicalDevice phys, VkDevice device,
                        uint32_t width, uint32_t height,
                        VkFormat format,
                        VkImageUsageFlags usage,
                        const void* pNext,
                        VkImage& outImage, VkDeviceMemory& outMemory)
{
    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext         = pNext;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = format;
    ici.extent        = {width, height, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = usage;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vkCreateImage(device, &ici, nullptr, &outImage);

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device, outImage, &mr);

    uint32_t mt = UINT32_MAX;
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mt = i; break;
        }
    }

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = mt;
    vkAllocateMemory(device, &ai, nullptr, &outMemory);
    vkBindImageMemory(device, outImage, outMemory, 0);
}

// ============================================================================
// allocateImages: DPB + src NV12
// Адаптировано из allocateReferenceImages() строки 360-396 и
// allocateIntermediateImage() строк 399-461
// ============================================================================
static void allocateImages(Av1rEncoder& enc)
{
    // DPB образы (строки 360-396 примера)
    for (uint32_t i = 0; i < Av1rEncoder::DPB_COUNT; i++) {
        createImage(enc.physDevice, enc.device,
                    enc.width, enc.height,
                    enc.dpbFormat,
                    VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
                    &enc.videoProfileList,
                    enc.dpbImages[i], enc.dpbMemory[i]);

        VkImageViewCreateInfo vci{};
        vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image    = enc.dpbImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = enc.dpbFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(enc.device, &vci, nullptr, &enc.dpbImageViews[i]);
    }

    // Src образ NV12 (строки 399-461 примера, без compute шейдера)
    // В AV1R данные NV12 приходят от ffmpeg, загружаем напрямую через staging
    createImage(enc.physDevice, enc.device,
                enc.width, enc.height,
                enc.srcFormat,
                VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                &enc.videoProfileList,
                enc.srcImage, enc.srcMemory);

    VkImageViewCreateInfo vci{};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = enc.srcImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = enc.srcFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(enc.device, &vci, nullptr, &enc.srcImageView);
}

// ============================================================================
// allocateBitstreamBuffer (строки 345-358 примера)
// ============================================================================
static void allocateBitstreamBuffer(Av1rEncoder& enc)
{
    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext       = &enc.videoProfileList;
    bci.size        = Av1rEncoder::BITSTREAM_BUF_SIZE;
    bci.usage       = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(enc.device, &bci, nullptr, &enc.bitstreamBuf);

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(enc.device, enc.bitstreamBuf, &mr);
    uint32_t mt = find_mem_type(enc.physDevice, mr.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = mt;
    vkAllocateMemory(enc.device, &ai, nullptr, &enc.bitstreamMemory);
    vkBindBufferMemory(enc.device, enc.bitstreamBuf, enc.bitstreamMemory, 0);
    vkMapMemory(enc.device, enc.bitstreamMemory, 0, VK_WHOLE_SIZE, 0, &enc.bitstreamPtr);
}

// ============================================================================
// createQueryPool (строки 464-476 примера)
// ============================================================================
static void createQueryPool(Av1rEncoder& enc)
{
    VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedbackCI{};
    feedbackCI.sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR;
    feedbackCI.pNext = &enc.videoProfile;
    feedbackCI.encodeFeedbackFlags =
        VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
        VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;

    VkQueryPoolCreateInfo qpci{};
    qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.pNext      = &feedbackCI;
    qpci.queryType  = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
    qpci.queryCount = 1;
    vkCreateQueryPool(enc.device, &qpci, nullptr, &enc.queryPool);
}

// ============================================================================
// initRateControl
// Адаптировано из VideoEncoder::initRateControl() строки 575-619
// H.264 rate control → AV1 rate control структуры
// ============================================================================
static void initRateControl(Av1rEncoder& enc, VkCommandBuffer cmd)
{
    VkVideoBeginCodingInfoKHR beginInfo{};
    beginInfo.sType                  = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    beginInfo.videoSession           = enc.videoSession;
    beginInfo.videoSessionParameters = enc.videoSessionParameters;

    // AV1 rate control layer (аналог VkVideoEncodeH264RateControlLayerInfoKHR)
    enc.av1RateControlLayer.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_LAYER_INFO_KHR;

    enc.rateControlLayer.pNext               = &enc.av1RateControlLayer;
    enc.rateControlLayer.sType               = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR;
    enc.rateControlLayer.frameRateNumerator  = enc.fps;
    enc.rateControlLayer.frameRateDenominator = 1;
    enc.rateControlLayer.averageBitrate      = 5000000;  // 5 Mbps
    enc.rateControlLayer.maxBitrate          = 20000000; // 20 Mbps

    // AV1 rate control info (аналог VkVideoEncodeH264RateControlInfoKHR)
    enc.av1RateControlInfo.sType          = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_INFO_KHR;
    enc.av1RateControlInfo.flags          = VK_VIDEO_ENCODE_AV1_RATE_CONTROL_REGULAR_GOP_BIT_KHR;
    enc.av1RateControlInfo.gopFrameCount  = 16;
    enc.av1RateControlInfo.keyFramePeriod = 16;
    enc.av1RateControlInfo.temporalLayerCount = 1;

    enc.rateControlInfo.sType               = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR;
    enc.rateControlInfo.pNext               = &enc.av1RateControlInfo;
    enc.rateControlInfo.rateControlMode     = enc.chosenRateControlMode;
    enc.rateControlInfo.layerCount          = 1;
    enc.rateControlInfo.pLayers             = &enc.rateControlLayer;
    enc.rateControlInfo.initialVirtualBufferSizeInMs = 100;
    enc.rateControlInfo.virtualBufferSizeInMs        = 200;

    if (enc.chosenRateControlMode & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR)
        enc.rateControlLayer.averageBitrate = enc.rateControlLayer.maxBitrate;

    if (enc.chosenRateControlMode & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR ||
        enc.chosenRateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR) {
        enc.av1RateControlInfo.temporalLayerCount = 0;
        enc.rateControlInfo.layerCount = 0;
    }

    VkVideoCodingControlInfoKHR controlInfo{};
    controlInfo.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
    controlInfo.pNext = &enc.rateControlInfo;
    controlInfo.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR |
                        VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;

    VkVideoEndCodingInfoKHR endInfo{};
    endInfo.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;

    vkCmdBeginVideoCodingKHR(cmd, &beginInfo);
    vkCmdControlVideoCodingKHR(cmd, &controlInfo);
    vkCmdEndVideoCodingKHR(cmd, &endInfo);
}

// ============================================================================
// transitionDpbImagesInitial
// Адаптировано из transitionImagesInitial() строки 622-644
// ============================================================================
static void transitionDpbImagesInitial(Av1rEncoder& enc, VkCommandBuffer cmd)
{
    std::vector<VkImageMemoryBarrier2> barriers;
    for (uint32_t i = 0; i < Av1rEncoder::DPB_COUNT; i++) {
        VkImageMemoryBarrier2 b{};
        b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask     = VK_PIPELINE_STAGE_2_NONE;
        b.dstStageMask     = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        b.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout        = VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR;
        b.image            = enc.dpbImages[i];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barriers.push_back(b);
    }
    VkDependencyInfoKHR dep{};
    dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
    dep.imageMemoryBarrierCount  = static_cast<uint32_t>(barriers.size());
    dep.pImageMemoryBarriers     = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

// ============================================================================
// uploadNV12Frame: CPU NV12 → GPU src image через staging buffer
// Заменяет RGB→YCbCr compute shader из примера (у нас данные уже NV12 от ffmpeg)
// ============================================================================
static void uploadNV12Frame(Av1rEncoder& enc,
                             VkCommandBuffer cmd,
                             const uint8_t* nv12,
                             VkBuffer stagingBuf, void* stagingPtr)
{
    size_t frameBytes = enc.width * enc.height * 3 / 2;
    memcpy(stagingPtr, nv12, frameBytes);

    // Transition src image UNDEFINED → TRANSFER_DST
    VkImageMemoryBarrier2 toTransfer{};
    toTransfer.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toTransfer.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    toTransfer.dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
    toTransfer.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.image         = enc.srcImage;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfoKHR dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &toTransfer;
    vkCmdPipelineBarrier2(cmd, &dep);

    // Copy Y plane
    VkBufferImageCopy yRegion{};
    yRegion.bufferOffset      = 0;
    yRegion.bufferRowLength   = enc.width;
    yRegion.imageSubresource  = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
    yRegion.imageExtent       = {enc.width, enc.height, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuf, enc.srcImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &yRegion);

    // Copy UV plane
    VkBufferImageCopy uvRegion{};
    uvRegion.bufferOffset     = enc.width * enc.height;
    uvRegion.bufferRowLength  = enc.width;
    uvRegion.imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
    uvRegion.imageExtent      = {enc.width / 2, enc.height / 2, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuf, enc.srcImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &uvRegion);

    // Transition src image TRANSFER_DST → VIDEO_ENCODE_SRC (строки 786-806 примера)
    VkImageMemoryBarrier2 toEncode{};
    toEncode.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toEncode.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
    toEncode.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toEncode.dstStageMask  = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    toEncode.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
    toEncode.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toEncode.newLayout     = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
    toEncode.image         = enc.srcImage;
    toEncode.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    dep.pImageMemoryBarriers = &toEncode;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// ============================================================================
// encodeOneFrame
// Прямой перенос структуры из VideoEncoder::encodeVideoFrame() строки 718-857
// H.264 picture info → AV1 picture info
// ============================================================================
static void encodeOneFrame(Av1rEncoder& enc, VkCommandBuffer cmd)
{
    const uint32_t GOP_LENGTH   = 16;
    const uint32_t gopIdx       = enc.frameCount % GOP_LENGTH;
    const bool     isKeyFrame   = (gopIdx == 0);
    const uint32_t querySlotId  = 0;

    vkCmdResetQueryPool(cmd, enc.queryPool, querySlotId, 1);

    // DPB ping-pong (строки 737-776 примера)
    uint32_t curDpbSlot = gopIdx & 1u;
    uint32_t refDpbSlot = curDpbSlot ^ 1u;

    VkVideoPictureResourceInfoKHR dpbPicRes{};
    dpbPicRes.sType            = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    dpbPicRes.imageViewBinding = enc.dpbImageViews[curDpbSlot];
    dpbPicRes.codedOffset      = {0, 0};
    dpbPicRes.codedExtent      = {enc.width, enc.height};

    VkVideoPictureResourceInfoKHR refPicRes{};
    refPicRes.sType            = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    refPicRes.imageViewBinding = enc.dpbImageViews[refDpbSlot];
    refPicRes.codedOffset      = {0, 0};
    refPicRes.codedExtent      = {enc.width, enc.height};

    // AV1 DPB slot info (аналог VkVideoEncodeH264DpbSlotInfoKHR)
    StdVideoEncodeAV1ReferenceInfo stdDpbRef{};
    stdDpbRef.flags.disable_frame_end_update_cdf = 0;
    stdDpbRef.frame_type = isKeyFrame ? STD_VIDEO_AV1_FRAME_TYPE_KEY
                                      : STD_VIDEO_AV1_FRAME_TYPE_INTER;
    stdDpbRef.OrderHint  = static_cast<uint8_t>(enc.frameCount & 0xFF);

    VkVideoEncodeAV1DpbSlotInfoKHR dpbSlotInfo{};
    dpbSlotInfo.sType            = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR;
    dpbSlotInfo.pStdReferenceInfo = &stdDpbRef;

    StdVideoEncodeAV1ReferenceInfo stdRefRef{};
    stdRefRef.frame_type = STD_VIDEO_AV1_FRAME_TYPE_INTER;
    stdRefRef.OrderHint  = static_cast<uint8_t>((enc.frameCount - 1) & 0xFF);

    VkVideoEncodeAV1DpbSlotInfoKHR refSlotInfo{};
    refSlotInfo.sType             = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR;
    refSlotInfo.pStdReferenceInfo = &stdRefRef;

    VkVideoReferenceSlotInfoKHR referenceSlots[2]{};
    referenceSlots[0].sType            = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    referenceSlots[0].pNext            = &dpbSlotInfo;
    referenceSlots[0].slotIndex        = -1; // setup slot (текущий кадр)
    referenceSlots[0].pPictureResource = &dpbPicRes;

    referenceSlots[1].sType            = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    referenceSlots[1].pNext            = &refSlotInfo;
    referenceSlots[1].slotIndex        = static_cast<int32_t>(refDpbSlot);
    referenceSlots[1].pPictureResource = &refPicRes;

    // Begin video coding (строки 778-784 примера)
    VkVideoBeginCodingInfoKHR beginCodingInfo{};
    beginCodingInfo.sType                  = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    beginCodingInfo.pNext                  = &enc.rateControlInfo;
    beginCodingInfo.videoSession           = enc.videoSession;
    beginCodingInfo.videoSessionParameters = enc.videoSessionParameters;
    beginCodingInfo.referenceSlotCount     = isKeyFrame ? 1u : 2u;
    beginCodingInfo.pReferenceSlots        = referenceSlots;
    vkCmdBeginVideoCodingKHR(cmd, &beginCodingInfo);

    // AV1 picture info (аналог VkVideoEncodeH264PictureInfoKHR, строки 815-818)
    StdVideoEncodeAV1PictureInfo stdPicInfo{};
    stdPicInfo.flags.error_resilient_mode       = 0;
    stdPicInfo.flags.disable_cdf_update         = 0;
    stdPicInfo.flags.use_superres               = 0;
    stdPicInfo.flags.render_and_frame_size_different = 0;
    stdPicInfo.flags.allow_screen_content_tools = 0;
    stdPicInfo.flags.is_motion_mode_switchable  = 0;
    stdPicInfo.flags.use_ref_frame_mvs          = !isKeyFrame ? 1u : 0u;
    stdPicInfo.flags.show_frame                 = 1;
    stdPicInfo.flags.showable_frame             = 1;
    stdPicInfo.frame_type = isKeyFrame ? STD_VIDEO_AV1_FRAME_TYPE_KEY
                                       : STD_VIDEO_AV1_FRAME_TYPE_INTER;
    stdPicInfo.current_frame_id = 0;
    stdPicInfo.OrderHint        = static_cast<uint8_t>(enc.frameCount & 0xFF);
    stdPicInfo.primary_ref_frame = isKeyFrame ? STD_VIDEO_AV1_PRIMARY_REF_NONE : 0;
    stdPicInfo.refresh_frame_flags = isKeyFrame ? 0xFF : (1u << curDpbSlot);

    // Reference frame setup для inter-кадров
    StdVideoEncodeAV1ReferenceListsInfo refLists{};
    if (!isKeyFrame) {
        // Все reference типы указывают на предыдущий кадр
        for (int r = 0; r < STD_VIDEO_AV1_REFS_PER_FRAME; r++) {
            refLists.referenceNameSlotIndices[r] = static_cast<uint8_t>(refDpbSlot);
        }
        stdPicInfo.pReferenceListsInfo = &refLists;
    }

    VkVideoEncodeAV1PictureInfoKHR av1PicInfo{};
    av1PicInfo.sType              = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PICTURE_INFO_KHR;
    av1PicInfo.predictionMode     = isKeyFrame
        ? VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_INTRA_ONLY_KHR
        : VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_SINGLE_REFERENCE_KHR;
    av1PicInfo.rateControlGroup   = VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_INTER_KHR;
    av1PicInfo.constantQIndex     = 0; // используется только при DISABLED rate control
    av1PicInfo.pStdPictureInfo    = &stdPicInfo;

    // src picture resource
    VkVideoPictureResourceInfoKHR inputPicRes{};
    inputPicRes.sType            = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    inputPicRes.imageViewBinding = enc.srcImageView;
    inputPicRes.codedOffset      = {0, 0};
    inputPicRes.codedExtent      = {enc.width, enc.height};

    // setup reference slot — текущий кадр становится reference (строка 827)
    referenceSlots[0].slotIndex = static_cast<int32_t>(curDpbSlot);

    // VkVideoEncodeInfoKHR (строки 821-833 примера)
    VkVideoEncodeInfoKHR encodeInfo{};
    encodeInfo.sType               = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;
    encodeInfo.pNext               = &av1PicInfo;
    encodeInfo.dstBuffer           = enc.bitstreamBuf;
    encodeInfo.dstBufferOffset     = 0;
    encodeInfo.dstBufferRange      = Av1rEncoder::BITSTREAM_BUF_SIZE;
    encodeInfo.srcPictureResource  = inputPicRes;
    encodeInfo.pSetupReferenceSlot = &referenceSlots[0];
    if (!isKeyFrame) {
        encodeInfo.referenceSlotCount = 1;
        encodeInfo.pReferenceSlots    = &referenceSlots[1];
    }

    // Query + encode + end (строки 835-842 примера)
    vkCmdBeginQuery(cmd, enc.queryPool, querySlotId, 0);
    vkCmdEncodeVideoKHR(cmd, &encodeInfo);
    vkCmdEndQuery(cmd, enc.queryPool, querySlotId);

    VkVideoEndCodingInfoKHR endInfo{};
    endInfo.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
    vkCmdEndVideoCodingKHR(cmd, &endInfo);
}

// ============================================================================
// getOutputPacket: читаем bitstream после encode fence
// Прямой перенос из getOutputVideoPacket() строки 860-883
// ============================================================================
static size_t getOutputPacket(Av1rEncoder& enc, std::vector<uint8_t>& out)
{
    struct EncodeStatus {
        uint32_t bitstreamOffset;
        uint32_t bitstreamSize;
        VkQueryResultStatusKHR status;
    } result{};

    vkGetQueryPoolResults(enc.device, enc.queryPool, 0, 1,
                          sizeof(result), &result, sizeof(result),
                          VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);

    if (result.status != VK_QUERY_RESULT_STATUS_COMPLETE_KHR || result.bitstreamSize == 0)
        return 0;

    const uint8_t* src = static_cast<const uint8_t*>(enc.bitstreamPtr)
                         + result.bitstreamOffset;
    out.insert(out.end(), src, src + result.bitstreamSize);
    return result.bitstreamSize;
}

// ============================================================================
// Cleanup
// ============================================================================
static void destroyEncoder(Av1rEncoder& enc)
{
    if (enc.bitstreamPtr)
        vkUnmapMemory(enc.device, enc.bitstreamMemory);
    if (enc.bitstreamBuf != VK_NULL_HANDLE)
        vkDestroyBuffer(enc.device, enc.bitstreamBuf, nullptr);
    if (enc.bitstreamMemory != VK_NULL_HANDLE)
        vkFreeMemory(enc.device, enc.bitstreamMemory, nullptr);

    if (enc.queryPool != VK_NULL_HANDLE)
        vkDestroyQueryPool(enc.device, enc.queryPool, nullptr);

    vkDestroyImageView(enc.device, enc.srcImageView, nullptr);
    vkDestroyImage(enc.device, enc.srcImage, nullptr);
    vkFreeMemory(enc.device, enc.srcMemory, nullptr);

    for (uint32_t i = 0; i < Av1rEncoder::DPB_COUNT; i++) {
        vkDestroyImageView(enc.device, enc.dpbImageViews[i], nullptr);
        vkDestroyImage(enc.device, enc.dpbImages[i], nullptr);
        vkFreeMemory(enc.device, enc.dpbMemory[i], nullptr);
    }

    if (enc.videoSessionParameters != VK_NULL_HANDLE)
        vkDestroyVideoSessionParametersKHR(enc.device, enc.videoSessionParameters, nullptr);
    if (enc.videoSession != VK_NULL_HANDLE)
        vkDestroyVideoSessionKHR(enc.device, enc.videoSession, nullptr);
    for (auto& m : enc.sessionMemory)
        vkFreeMemory(enc.device, m, nullptr);

    if (enc.interQueueSemaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(enc.device, enc.interQueueSemaphore, nullptr);
    if (enc.encodeFence != VK_NULL_HANDLE)
        vkDestroyFence(enc.device, enc.encodeFence, nullptr);
    if (enc.encodeCommandPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(enc.device, enc.encodeCommandPool, nullptr);
}

// ============================================================================
// Публичный API: av1r_vulkan_encode
// ============================================================================
void av1r_vulkan_encode(
    Av1rVulkanCtx&        ctx,
    const uint8_t*        frames_nv12,   // все кадры подряд, NV12
    int                   n_frames,
    int                   width,
    int                   height,
    int                   fps,
    int                   crf,
    std::vector<uint8_t>& out_bitstream,
    std::vector<size_t>*  out_frame_sizes = nullptr)
{
    if (!ctx.initialized)
        throw std::runtime_error("Vulkan context not initialized");

    Av1rEncoder enc{};
    enc.physDevice  = ctx.physDevice;
    enc.device      = ctx.device;
    enc.encodeQueue = ctx.encodeQueue.queue;
    enc.encodeQFam  = ctx.encodeQueue.queue_family_index;
    enc.width       = static_cast<uint32_t>(width  & ~1);
    enc.height      = static_cast<uint32_t>(height & ~1);
    enc.fps         = static_cast<uint32_t>(fps);

    // Инициализация
    createVideoSession(enc, crf);
    allocateVideoSessionMemory(enc);
    createVideoSessionParameters(enc);
    allocateImages(enc);
    allocateBitstreamBuffer(enc);
    createQueryPool(enc);

    enc.encodeCommandPool = av1r_create_command_pool(enc.device, enc.encodeQFam);
    enc.encodeFence       = av1r_create_fence(enc.device);

    // Staging buffer для загрузки NV12 кадров
    size_t frameBytes = static_cast<size_t>(width * height * 3 / 2);
    Av1rBuffer staging = av1r_buffer_create(
        enc.physDevice, enc.device, frameBytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // Первичная инициализация: rate control + DPB layout
    {
        VkCommandBuffer initCmd = av1r_alloc_command_buffer(enc.device, enc.encodeCommandPool);
        av1r_begin_command_buffer(initCmd);
        initRateControl(enc, initCmd);
        transitionDpbImagesInitial(enc, initCmd);
        av1r_end_command_buffer(initCmd);

        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &initCmd;
        av1r_reset_fence(enc.device, enc.encodeFence);
        vkQueueSubmit(enc.encodeQueue, 1, &si, enc.encodeFence);
        av1r_wait_fence(enc.device, enc.encodeFence);
        vkFreeCommandBuffers(enc.device, enc.encodeCommandPool, 1, &initCmd);
    }

    // Основной цикл кодирования
    out_bitstream.clear();
    const uint8_t* frame_ptr = frames_nv12;

    for (int f = 0; f < n_frames; f++, frame_ptr += frameBytes) {
        enc.frameCount = static_cast<uint32_t>(f);

        VkCommandBuffer cmd = av1r_alloc_command_buffer(enc.device, enc.encodeCommandPool);
        av1r_begin_command_buffer(cmd);

        uploadNV12Frame(enc, cmd, frame_ptr, staging.buffer, staging.ptr);
        encodeOneFrame(enc, cmd);

        av1r_end_command_buffer(cmd);

        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cmd;
        av1r_reset_fence(enc.device, enc.encodeFence);
        vkQueueSubmit(enc.encodeQueue, 1, &si, enc.encodeFence);
        av1r_wait_fence(enc.device, enc.encodeFence);

        size_t before = out_bitstream.size();
        getOutputPacket(enc, out_bitstream);
        if (out_frame_sizes)
            out_frame_sizes->push_back(out_bitstream.size() - before);

        vkFreeCommandBuffers(enc.device, enc.encodeCommandPool, 1, &cmd);
    }

    av1r_buffer_destroy(enc.device, staging);
    destroyEncoder(enc);
}

#endif // AV1R_VULKAN_VIDEO_AV1
