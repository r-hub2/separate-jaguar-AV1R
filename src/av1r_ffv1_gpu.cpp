// Vulkan compute context for the FFV1 GPU encoder.

#ifdef AV1R_FFV1_GPU

#include "av1r_ffv1_gpu.h"
#include "av1r_stderr_suppress.h"
#include "av1r_vk_instance.h"

#include <cstring>
#include <stdexcept>

namespace av1r {
namespace ffv1 {
namespace gpu {

static void vk_check(VkResult r, const char* what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string("FFV1 GPU: ") + what +
                                 " failed (VkResult " + std::to_string(r) + ")");
}

// Device preference: discrete > integrated > virtual > CPU (llvmpipe).
static int device_rank(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 1;
    default:                                     return 0;
    }
}

Context::Context() {
    try {
        // Shared with the AV1 path; driver warnings are suppressed there.
        instance_ = av1r_create_instance();

        // Drivers print warnings (e.g. RADV "not a conformant Vulkan
        // implementation") while initialising physical devices: silence
        // fd 2 for these calls only. Failures still surface via VkResult.
        uint32_t n_dev = 0;
        std::vector<VkPhysicalDevice> devs;
        {
            StderrSuppressor quiet;
            vk_check(vkEnumeratePhysicalDevices(instance_, &n_dev, nullptr),
                     "vkEnumeratePhysicalDevices");
            devs.resize(n_dev);
            if (n_dev > 0)
                vk_check(vkEnumeratePhysicalDevices(instance_, &n_dev, devs.data()),
                         "vkEnumeratePhysicalDevices");
        }

        int best_rank = -1;
        for (VkPhysicalDevice d : devs) {
            uint32_t n_q = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &n_q, nullptr);
            std::vector<VkQueueFamilyProperties> qp(n_q);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &n_q, qp.data());
            for (uint32_t i = 0; i < n_q; ++i) {
                if (!(qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(d, &props);
                const int rank = device_rank(props.deviceType);
                if (rank > best_rank) {
                    best_rank = rank;
                    phys_ = d;
                    queue_family_ = i;
                    device_name_ = props.deviceName;
                    ts_valid_bits_ = qp[i].timestampValidBits;
                    ts_period_ = props.limits.timestampPeriod;
                }
                break;
            }
        }
        if (phys_ == VK_NULL_HANDLE)
            throw std::runtime_error("FFV1 GPU: no Vulkan device with a compute queue");

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queue_family_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;

        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        {
            StderrSuppressor quiet;
            vk_check(vkCreateDevice(phys_, &dci, nullptr, &device_), "vkCreateDevice");
        }
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queue_family_;
        vk_check(vkCreateCommandPool(device_, &pci, nullptr, &cmd_pool_),
                 "vkCreateCommandPool");

        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cmd_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(device_, &cai, &cmd_),
                 "vkAllocateCommandBuffers");

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vk_check(vkCreateFence(device_, &fci, nullptr, &fence_), "vkCreateFence");
    } catch (...) {
        cleanup();
        throw;
    }
}

Context::~Context() { cleanup(); }

void Context::cleanup() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (query_pool_ != VK_NULL_HANDLE)
            vkDestroyQueryPool(device_, query_pool_, nullptr);
        if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
        if (cmd_pool_ != VK_NULL_HANDLE)
            vkDestroyCommandPool(device_, cmd_pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    av1r_destroy_instance(instance_);
    query_pool_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    cmd_pool_ = VK_NULL_HANDLE;
    cmd_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

// UINT32_MAX if no memory type allowed by type_bits has all of flags.
uint32_t Context::find_memory_type(uint32_t type_bits,
                                   VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

Buffer Context::create_buffer(VkDeviceSize size, Mem mem) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_check(vkCreateBuffer(device_, &bci, nullptr, &b.buffer), "vkCreateBuffer");

    try {
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, b.buffer, &req);

        // Memory property flags in order of preference.
        const VkMemoryPropertyFlags DL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                    HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                    HC = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    CA = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        std::vector<VkMemoryPropertyFlags> prefs;
        switch (mem) {
        case Mem::Shared:   prefs = {HV | HC}; break;
        case Mem::Device:   prefs = {DL}; break;
        case Mem::Download: prefs = {HV | CA | HC, HV | CA, HV | HC}; break;
        }
        b.mem_type = UINT32_MAX;
        for (VkMemoryPropertyFlags f : prefs) {
            b.mem_type = find_memory_type(req.memoryTypeBits, f);
            if (b.mem_type != UINT32_MAX) break;
        }
        if (b.mem_type == UINT32_MAX)
            throw std::runtime_error("FFV1 GPU: no suitable memory type");

        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
        const VkMemoryPropertyFlags got = mp.memoryTypes[b.mem_type].propertyFlags;
        b.coherent = (got & HC) != 0;

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = b.mem_type;
        vk_check(vkAllocateMemory(device_, &mai, nullptr, &b.memory),
                 "vkAllocateMemory");
        vk_check(vkBindBufferMemory(device_, b.buffer, b.memory, 0),
                 "vkBindBufferMemory");
        if (mem != Mem::Device)
            vk_check(vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped),
                     "vkMapMemory");
    } catch (...) {
        destroy_buffer(b);
        throw;
    }
    return b;
}

void Context::destroy_buffer(Buffer& b) {
    if (b.mapped) vkUnmapMemory(device_, b.memory);
    if (b.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory != VK_NULL_HANDLE) vkFreeMemory(device_, b.memory, nullptr);
    b = Buffer{};
}

std::string Context::describe_memory(const Buffer& b) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    const VkMemoryType& t = mp.memoryTypes[b.mem_type];
    const VkMemoryHeap& h = mp.memoryHeaps[t.heapIndex];
    std::string flags;
    const struct { VkMemoryPropertyFlags bit; const char* name; } names[] = {
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,  "DEVICE_LOCAL"},
        {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,  "HOST_VISIBLE"},
        {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HOST_COHERENT"},
        {VK_MEMORY_PROPERTY_HOST_CACHED_BIT,   "HOST_CACHED"}};
    for (const auto& n : names)
        if (t.propertyFlags & n.bit) flags += flags.empty() ? n.name : std::string("|") + n.name;
    return "type " + std::to_string(b.mem_type) + " [" + flags + "] heap " +
           std::to_string(t.heapIndex) + " (" +
           std::to_string(h.size >> 20) + " MiB" +
           ((h.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? ", DEVICE_LOCAL" : "") +
           ")";
}

Pipeline Context::create_pipeline(const uint32_t* spv, size_t spv_bytes,
                                  uint32_t n_buffers, uint32_t push_size) {
    Pipeline p;
    p.n_buffers = n_buffers;
    p.push_size = push_size;
    try {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = spv_bytes;
        smci.pCode = spv;
        vk_check(vkCreateShaderModule(device_, &smci, nullptr, &p.module),
                 "vkCreateShaderModule");

        std::vector<VkDescriptorSetLayoutBinding> binds(n_buffers);
        for (uint32_t i = 0; i < n_buffers; ++i) {
            binds[i] = VkDescriptorSetLayoutBinding{};
            binds[i].binding = i;
            binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binds[i].descriptorCount = 1;
            binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = n_buffers;
        dslci.pBindings = binds.data();
        vk_check(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &p.set_layout),
                 "vkCreateDescriptorSetLayout");

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.size = push_size;
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &p.set_layout;
        plci.pushConstantRangeCount = push_size > 0 ? 1 : 0;
        plci.pPushConstantRanges = &pcr;
        vk_check(vkCreatePipelineLayout(device_, &plci, nullptr, &p.layout),
                 "vkCreatePipelineLayout");

        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = p.module;
        cpci.stage.pName = "main";
        cpci.layout = p.layout;
        vk_check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci,
                                          nullptr, &p.pipeline),
                 "vkCreateComputePipelines");

        VkDescriptorPoolSize ps{};
        ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ps.descriptorCount = n_buffers;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &ps;
        vk_check(vkCreateDescriptorPool(device_, &dpci, nullptr, &p.pool),
                 "vkCreateDescriptorPool");

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = p.pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &p.set_layout;
        vk_check(vkAllocateDescriptorSets(device_, &dsai, &p.set),
                 "vkAllocateDescriptorSets");
    } catch (...) {
        destroy_pipeline(p);
        throw;
    }
    return p;
}

void Context::destroy_pipeline(Pipeline& p) {
    if (p.pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, p.pool, nullptr);
    if (p.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, p.pipeline, nullptr);
    if (p.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, p.layout, nullptr);
    if (p.set_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, p.set_layout, nullptr);
    if (p.module != VK_NULL_HANDLE) vkDestroyShaderModule(device_, p.module, nullptr);
    p = Pipeline{};
}

void Context::dispatch(Pipeline& p, const std::vector<Buffer*>& buffers,
                       const void* push, uint32_t gx, uint32_t gy, uint32_t gz,
                       const std::vector<Copy>& before,
                       const std::vector<Copy>& after,
                       const std::vector<Fill>& fills) {
    if (buffers.size() != p.n_buffers)
        throw std::runtime_error("FFV1 GPU: dispatch buffer count mismatch");

    std::vector<VkDescriptorBufferInfo> infos(p.n_buffers);
    std::vector<VkWriteDescriptorSet> writes(p.n_buffers);
    for (uint32_t i = 0; i < p.n_buffers; ++i) {
        infos[i] = VkDescriptorBufferInfo{buffers[i]->buffer, 0, VK_WHOLE_SIZE};
        writes[i] = VkWriteDescriptorSet{};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = p.set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, p.n_buffers, writes.data(), 0, nullptr);

    vk_check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");

    // Timestamp i once all previously recorded commands have completed.
    auto stamp = [this](uint32_t i) {
        if (query_pool_ != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                query_pool_, i);
    };
    // Writes of earlier submissions (previous dispatch parts: states, coder
    // registers, output) complete before this one accesses the buffers.
    memory_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    if (query_pool_ != VK_NULL_HANDLE) vkCmdResetQueryPool(cmd_, query_pool_, 0, 4);
    stamp(0);

    if (!before.empty() || !fills.empty()) {
        for (const Fill& fl : fills)
            vkCmdFillBuffer(cmd_, fl.dst->buffer, 0, fl.size, fl.value);
        for (const Copy& c : before)
            record_copy(c);
        // Fills and copies complete before the shader accesses them.
        memory_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    stamp(1);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1,
                            &p.set, 0, nullptr);
    if (p.push_size > 0)
        vkCmdPushConstants(cmd_, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           p.push_size, push);
    vkCmdDispatch(cmd_, gx, gy, gz);
    stamp(2);

    if (!after.empty()) {
        // Shader writes complete before they are copied out.
        memory_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT);
        for (const Copy& c : after)
            record_copy(c);
    }

    stamp(3);

    // Make shader and copy writes visible to host reads of mapped memory.
    memory_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    vk_check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
    submit_and_wait();
    read_times();

    for (const Copy& c : after)
        if (c.dst->mapped && !c.dst->coherent) invalidate(*c.dst);
}

bool Context::enable_timestamps() {
    if (query_pool_ != VK_NULL_HANDLE) return true;
    if (ts_valid_bits_ == 0 || !(ts_period_ > 0)) return false;
    VkQueryPoolCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = 4;
    vk_check(vkCreateQueryPool(device_, &qci, nullptr, &query_pool_),
             "vkCreateQueryPool");
    return true;
}

void Context::read_times() {
    last_times_ = Times{};
    if (query_pool_ == VK_NULL_HANDLE) return;
    uint64_t t[4];
    vk_check(vkGetQueryPoolResults(device_, query_pool_, 0, 4, sizeof(t), t,
                                   sizeof(uint64_t),
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
             "vkGetQueryPoolResults");
    const uint64_t mask = ts_valid_bits_ >= 64 ? ~uint64_t(0)
                                               : (uint64_t(1) << ts_valid_bits_) - 1;
    auto ns = [&](int a, int b) {
        return double((t[b] - t[a]) & mask) * double(ts_period_);
    };
    last_times_.valid     = true;
    last_times_.before_ns = ns(0, 1);
    last_times_.shader_ns = ns(1, 2);
    last_times_.after_ns  = ns(2, 3);
}

void Context::upload(Buffer& dst, const void* data, VkDeviceSize size) {
    if (size > dst.size)
        throw std::invalid_argument("FFV1 GPU: upload larger than buffer");
    Buffer staging = create_buffer(size, Mem::Shared);
    try {
        std::memcpy(staging.mapped, data, size_t(size));
        vk_check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
        record_copy(Copy{&staging, &dst, size});
        // Visible to shaders of later submissions on this queue.
        memory_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
        vk_check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
        submit_and_wait();
    } catch (...) {
        destroy_buffer(staging);
        throw;
    }
    destroy_buffer(staging);
}

void Context::record_copy(const Copy& c) {
    VkBufferCopy region{0, 0, c.size};
    vkCmdCopyBuffer(cmd_, c.src->buffer, c.dst->buffer, 1, &region);
}

void Context::memory_barrier(VkPipelineStageFlags src_stage,
                             VkAccessFlags src_access,
                             VkPipelineStageFlags dst_stage,
                             VkAccessFlags dst_access) {
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = src_access;
    mb.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd_, src_stage, dst_stage, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
}

void Context::invalidate(const Buffer& b) {
    VkMappedMemoryRange r{};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = b.memory;
    r.offset = 0;
    r.size = VK_WHOLE_SIZE;
    vk_check(vkInvalidateMappedMemoryRanges(device_, 1, &r),
             "vkInvalidateMappedMemoryRanges");
}

void Context::submit_and_wait() {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    vk_check(vkResetFences(device_, 1, &fence_), "vkResetFences");
    vk_check(vkQueueSubmit(queue_, 1, &si, fence_), "vkQueueSubmit");
    vk_check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX),
             "vkWaitForFences");
}

} // namespace gpu
} // namespace ffv1
} // namespace av1r

#endif // AV1R_FFV1_GPU
