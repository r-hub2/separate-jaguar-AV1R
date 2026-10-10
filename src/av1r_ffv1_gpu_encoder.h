// FFV1 v3 encoder on Vulkan compute (gray16). Produces Frames byte-identical
// to the CPU reference Encoder: the GPU range codes the Slices
// (ffv1_shaders/slice.comp), the CPU appends Slice Footers.
#pragma once

#ifdef AV1R_FFV1_GPU

#include <cstdint>
#include <string>
#include <vector>

#include "av1r_ffv1_encoder.h"
#include "av1r_ffv1_gpu.h"

namespace av1r {
namespace ffv1 {

class GpuEncoder {
public:
    // batch: frames per dispatch. bytes_per_sample: output capacity per
    // Slice (w * h * bytes_per_sample + margin); a Slice that does not fit
    // raises an error, it is never truncated.
    GpuEncoder(const Config& cfg, int batch, double bytes_per_sample);
    ~GpuEncoder();
    GpuEncoder(const GpuEncoder&) = delete;
    GpuEncoder& operator=(const GpuEncoder&) = delete;

    const std::string& device_name() const { return ctx_.device_name(); }
    std::vector<uint8_t> extradata() const { return ref_.extradata(); }
    int batch() const { return batch_; }

    // Encode n (1..batch) frames stored contiguously, row-major, width *
    // height samples each. Frame i is written to out[i] (replaced).
    void encode_frames(const uint16_t* pixels, int n,
                       std::vector<std::vector<uint8_t>>& out);

    // Wall time of encode_frames stages, seconds, summed over calls.
    struct Profile {
        double  upload   = 0;   // memcpy into the pixel buffer
        double  dispatch = 0;   // submit + wait for the fence
        double  gather   = 0;   // Slices from the output buffer + footers
        int64_t dispatches = 0; // batches
        int64_t parts      = 0; // submits (rows of the Slices per dispatch)
    };
    const Profile& profile() const { return prof_; }

    // One line per buffer: name, size, memory type (Context::describe_memory).
    std::string memory_report() const;

    // Shader benchmark: upload n frames once, one untimed warm-up dispatch,
    // then reps dispatches (with the pixel / output copies of encode_frames,
    // without gathering). coded_bytes: Slice bytes of the batch, footers
    // excluded.
    struct BenchRun {
        double wall_s    = 0;       // submit + wait
        bool   gpu_times = false;   // timestamps below are valid
        double before_ns = 0;       // pixel copy
        double shader_ns = 0;       // dispatch (all parts)
        double after_ns  = 0;       // output copies
    };
    std::vector<BenchRun> bench(const uint16_t* pixels, int n, int reps,
                                uint64_t& coded_bytes);

private:
    void release();
    void upload_frames(const uint16_t* pixels, int n);   // into b_pixels_up_
    // Code the n frames in b_pixels_up_ as a series of short dispatches
    // (row parts, see slice.comp); returns their GPU times summed (valid
    // only with timestamps enabled).
    gpu::Context::Times run_dispatch(int n);
    // Sum of the n * n_slices_ Slice lengths; throws if one overflowed.
    uint64_t checked_output_bytes(int n) const;

    Encoder      ref_;      // tables and geometry, same as the CPU path
    gpu::Context ctx_;
    gpu::Pipeline pipe_;
    gpu::Buffer  b_pixels_, b_slices_, b_trans_, b_quant_, b_states_,
                 b_out_, b_len_, b_coder_;
    // Host side: pixel upload staging, readback of out / len
    gpu::Buffer  b_pixels_up_, b_out_dl_, b_len_dl_;
    int          batch_;
    int          n_slices_;
    uint32_t     cap_words_;
    uint32_t     state_words_;
    std::vector<uint32_t> slice_w_, slice_h_;
    uint32_t     max_slice_h_ = 0;
    // Slice rows per dispatch part, adapted so that a part takes about
    // PART_TARGET_S on the GPU (the display is not blocked meanwhile).
    double       rows_per_part_ = 4;
    Profile      prof_;
};

} // namespace ffv1
} // namespace av1r

#endif // AV1R_FFV1_GPU
