// Vulkan compute context for the FFV1 GPU encoder.
// Separate from the AV1 video-encode context: needs only a compute queue.
// Built only with -DAV1R_FFV1_GPU (Vulkan headers + glslc found by configure).
#pragma once

#ifdef AV1R_FFV1_GPU

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace av1r {
namespace ffv1 {
namespace gpu {

// Where a buffer lives:
//   Shared   - host-visible, host-coherent, mapped (upload staging, self-tests)
//   Device   - device-local, not mapped (filled by upload() or copies)
//   Download - host-visible, preferably host-cached, mapped (copy target)
enum class Mem { Shared, Device, Download };

// Storage buffer, also usable as transfer source and destination.
struct Buffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize   size   = 0;
    void*          mapped = nullptr;   // null for Mem::Device
    uint32_t       mem_type = 0;       // memory type index
    bool           coherent = true;    // false: invalidate before host reads
};

// Fill of a buffer with a 32-bit value, recorded before a dispatch.
struct Fill {
    Buffer*      dst;
    VkDeviceSize size;    // multiple of 4
    uint32_t     value;
};

// Buffer-to-buffer copy recorded around a dispatch.
struct Copy {
    Buffer*      src;
    Buffer*      dst;
    VkDeviceSize size;
};

// Compute pipeline: n_buffers storage buffers at bindings 0..n-1 plus an
// optional push-constant block.
struct Pipeline {
    VkShaderModule        module      = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout  = VK_NULL_HANDLE;
    VkPipelineLayout      layout      = VK_NULL_HANDLE;
    VkPipeline            pipeline    = VK_NULL_HANDLE;
    VkDescriptorPool      pool        = VK_NULL_HANDLE;
    VkDescriptorSet       set         = VK_NULL_HANDLE;
    uint32_t              n_buffers   = 0;
    uint32_t              push_size   = 0;
};

class Context {
public:
    // Throws std::runtime_error if no Vulkan device with a compute queue.
    Context();
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    const std::string& device_name() const { return device_name_; }

    Buffer create_buffer(VkDeviceSize size, Mem mem = Mem::Shared);
    void   destroy_buffer(Buffer& b);

    // Fill a buffer (any Mem) with size bytes through a temporary staging
    // buffer; submits and waits.
    void upload(Buffer& dst, const void* data, VkDeviceSize size);

    // Memory type of b: index, property flags, heap and its size.
    std::string describe_memory(const Buffer& b) const;

    Pipeline create_pipeline(const uint32_t* spv, size_t spv_bytes,
                             uint32_t n_buffers, uint32_t push_size);
    void     destroy_pipeline(Pipeline& p);

    // Bind buffers, push constants, dispatch, submit and wait. The fills and
    // the copies in `before` run ahead of the dispatch, the copies in `after`
    // once it has finished; mapped destinations of `after` are ready for host
    // reads.
    void dispatch(Pipeline& p, const std::vector<Buffer*>& buffers,
                  const void* push, uint32_t gx, uint32_t gy, uint32_t gz,
                  const std::vector<Copy>& before = {},
                  const std::vector<Copy>& after = {},
                  const std::vector<Fill>& fills = {});

    // GPU timestamps of the last dispatch (enable_timestamps() first),
    // nanoseconds: copies before the dispatch, the dispatch, copies after.
    struct Times {
        bool   valid     = false;
        double before_ns = 0;
        double shader_ns = 0;
        double after_ns  = 0;
    };

    // Start recording timestamps around every dispatch. Returns false (and
    // records nothing) if the queue has no timestamp support.
    bool enable_timestamps();
    const Times& last_times() const { return last_times_; }

private:
    VkInstance       instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_     = VK_NULL_HANDLE;
    VkDevice         device_   = VK_NULL_HANDLE;
    VkQueue          queue_    = VK_NULL_HANDLE;
    uint32_t         queue_family_ = 0;
    VkCommandPool    cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer  cmd_      = VK_NULL_HANDLE;
    VkFence          fence_    = VK_NULL_HANDLE;
    std::string      device_name_;
    uint32_t         ts_valid_bits_ = 0;     // of queue_family_; 0: none
    float            ts_period_     = 0;     // ns per timestamp tick
    VkQueryPool      query_pool_    = VK_NULL_HANDLE;
    Times            last_times_;

    void cleanup();
    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    void submit_and_wait();
    void record_copy(const Copy& c);
    void memory_barrier(VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                        VkPipelineStageFlags dst_stage, VkAccessFlags dst_access);
    void invalidate(const Buffer& b);
    void read_times();
};

} // namespace gpu
} // namespace ffv1
} // namespace av1r

#endif // AV1R_FFV1_GPU
