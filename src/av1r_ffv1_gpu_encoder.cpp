// FFV1 v3 encoder on Vulkan compute (gray16).

#ifdef AV1R_FFV1_GPU

#include "av1r_ffv1_gpu_encoder.h"
#include "av1r_ffv1_rangecoder.h"
#include "av1r_ffv1_spv.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace av1r {
namespace ffv1 {

namespace {
// State bytes per coder (Slice) before the contexts (see slice.comp).
constexpr uint32_t STATE_HEADER_BYTES = 64;
// Fixed output margin per Slice: keyframe bit, Slice Header, termination.
constexpr uint32_t SLICE_MARGIN_BYTES = 256;
// GPU time per dispatch part: short enough that the display (compositor)
// keeps running while the GPU encodes.
constexpr double PART_TARGET_S = 0.008;
// Range coder registers saved per coder between parts (CODER_WORDS).
constexpr uint32_t CODER_WORDS = 8;
} // namespace

GpuEncoder::GpuEncoder(const Config& cfg, int batch, double bytes_per_sample)
    : ref_(cfg), batch_(batch) {
    if (batch < 1) throw std::invalid_argument("FFV1 GPU: batch must be >= 1");
    if (!(bytes_per_sample > 0))
        throw std::invalid_argument("FFV1 GPU: bytes_per_sample must be > 0");

    const uint64_t frame_px = uint64_t(cfg.width) * cfg.height;
    if (frame_px * batch >= (uint64_t(1) << 32))
        throw std::invalid_argument("FFV1 GPU: batch too large for 32-bit indexing");

    // Slice geometry, RFC 4.7.3, 4.7.4, 4.8.2, 4.8.3 (as Encoder::encode_slice)
    n_slices_ = cfg.num_h_slices * cfg.num_v_slices;
    std::vector<uint32_t> slices(size_t(8) * n_slices_, 0);
    uint64_t max_px = 0;
    for (int i = 0; i < n_slices_; ++i) {
        const int sx = i % cfg.num_h_slices, sy = i / cfg.num_h_slices;
        const int x0 = int(int64_t(sx) * cfg.width / cfg.num_h_slices);
        const int x1 = int(int64_t(sx + 1) * cfg.width / cfg.num_h_slices);
        const int y0 = int(int64_t(sy) * cfg.height / cfg.num_v_slices);
        const int y1 = int(int64_t(sy + 1) * cfg.height / cfg.num_v_slices);
        uint32_t* s = &slices[size_t(8) * i];
        s[0] = uint32_t(sx); s[1] = uint32_t(sy);
        s[2] = uint32_t(x0); s[3] = uint32_t(y0);
        s[4] = uint32_t(x1 - x0); s[5] = uint32_t(y1 - y0);
        slice_w_.push_back(s[4]);
        slice_h_.push_back(s[5]);
        max_slice_h_ = std::max(max_slice_h_, s[5]);
        max_px = std::max(max_px, uint64_t(s[4]) * s[5]);
    }
    const double cap_bytes = double(max_px) * bytes_per_sample + SLICE_MARGIN_BYTES;
    if (cap_bytes >= double(1u << 24) + SLICE_MARGIN_BYTES)
        throw std::invalid_argument(
            "FFV1 GPU: slice output buffer exceeds 16 MiB, use more slices");
    cap_words_ = uint32_t(cap_bytes / 4) + 1;

    const int qset = cfg.context_model;
    state_words_ = (STATE_HEADER_BYTES +
                    uint32_t(ref_.context_count(qset)) * CONTEXT_SIZE) / 4;

    // Transition tables of the Slices (RFC 3.8.1.4, Figure 23): default or
    // coder_type 2 table. The keyframe bit is coded with the default table
    // on the CPU path; its output does not depend on the table (only the
    // discarded next state does), so slice.comp codes it with this one.
    const uint8_t* table = ref_.state_transition();
    std::vector<uint32_t> trans(512, 0);
    trans[0] = table[0];
    for (int i = 1; i < 256; ++i) {
        trans[i]             = table[i];
        trans[256 + 256 - i] = uint32_t(uint8_t(256 - table[i]));
    }
    std::vector<int32_t> quant(5 * 256);
    for (int j = 0; j < 5; ++j)
        for (int k = 0; k < 256; ++k)
            quant[size_t(256) * j + k] = ref_.quant_table(qset, j)[k];

    const size_t n_wg = size_t(batch) * n_slices_;
    try {
        // Everything the shader touches is device-local; the host writes
        // pixels and reads out / len through staging buffers.
        using gpu::Mem;
        const VkDeviceSize pixels_bytes = (frame_px * batch + 1) / 2 * 4;
        const VkDeviceSize out_bytes = VkDeviceSize(cap_words_) * 4 * n_wg;
        const VkDeviceSize len_bytes = VkDeviceSize(4) * n_wg;
        b_pixels_    = ctx_.create_buffer(pixels_bytes, Mem::Device);
        b_slices_    = ctx_.create_buffer(slices.size() * 4, Mem::Device);
        b_trans_     = ctx_.create_buffer(trans.size() * 4, Mem::Device);
        b_quant_     = ctx_.create_buffer(quant.size() * 4, Mem::Device);
        b_states_    = ctx_.create_buffer(VkDeviceSize(state_words_) * 4 * n_wg,
                                          Mem::Device);
        b_out_       = ctx_.create_buffer(out_bytes, Mem::Device);
        b_len_       = ctx_.create_buffer(len_bytes, Mem::Device);
        b_pixels_up_ = ctx_.create_buffer(pixels_bytes, Mem::Shared);
        b_out_dl_    = ctx_.create_buffer(out_bytes, Mem::Download);
        b_len_dl_    = ctx_.create_buffer(len_bytes, Mem::Download);
        b_coder_     = ctx_.create_buffer(VkDeviceSize(CODER_WORDS) * 4 * n_wg,
                                          Mem::Device);
        ctx_.upload(b_slices_, slices.data(), slices.size() * 4);
        ctx_.upload(b_trans_, trans.data(), trans.size() * 4);
        ctx_.upload(b_quant_, quant.data(), quant.size() * 4);
        pipe_ = ctx_.create_pipeline(av1r_ffv1_spv_slice,
                                     sizeof(av1r_ffv1_spv_slice), 8,
                                     9 * sizeof(uint32_t));
    } catch (...) {
        release();   // ctx_ itself is destroyed during unwinding
        throw;
    }
}

GpuEncoder::~GpuEncoder() { release(); }

void GpuEncoder::release() {
    ctx_.destroy_pipeline(pipe_);
    for (gpu::Buffer* b : {&b_pixels_, &b_slices_, &b_trans_, &b_quant_,
                           &b_states_, &b_out_, &b_len_, &b_pixels_up_,
                           &b_out_dl_, &b_len_dl_, &b_coder_})
        if (b->buffer != VK_NULL_HANDLE) ctx_.destroy_buffer(*b);
}

namespace {
using prof_clock = std::chrono::steady_clock;
double secs_since(prof_clock::time_point t0) {
    return std::chrono::duration<double>(prof_clock::now() - t0).count();
}
} // namespace

void GpuEncoder::upload_frames(const uint16_t* pixels, int n) {
    if (n < 1 || n > batch_)
        throw std::invalid_argument("FFV1 GPU: frame count outside 1..batch");
    const Config& cfg = ref_.config();
    const auto t0 = prof_clock::now();
    std::memcpy(b_pixels_up_.mapped, pixels,
                size_t(cfg.width) * cfg.height * n * sizeof(uint16_t));
    prof_.upload += secs_since(t0);
}

gpu::Context::Times GpuEncoder::run_dispatch(int n) {
    const Config& cfg = ref_.config();
    const VkDeviceSize pixel_bytes =
        VkDeviceSize(cfg.width) * cfg.height * n * sizeof(uint16_t);

    // Copy only the frames of this call: n Slices per frame, frames first.
    const VkDeviceSize n_wg = VkDeviceSize(n) * n_slices_;
    const std::vector<gpu::Copy> before = {
        {&b_pixels_up_, &b_pixels_, (pixel_bytes + 3) / 4 * 4}};
    const std::vector<gpu::Copy> after = {
        {&b_out_, &b_out_dl_, VkDeviceSize(cap_words_) * 4 * n_wg},
        {&b_len_, &b_len_dl_, 4 * n_wg}};
    // Keyframe: all context states to 128 (RFC 3.8.1.3), before part 1.
    const std::vector<gpu::Fill> fills = {
        {&b_states_, VkDeviceSize(state_words_) * 4 * n_wg, 0x80808080u}};
    const std::vector<gpu::Copy> none;
    const std::vector<gpu::Fill> no_fill;

    gpu::Context::Times sum;
    sum.valid = true;
    const auto t_all = prof_clock::now();
    for (uint32_t row = 0; row < max_slice_h_;) {
        const uint32_t rows = std::max<uint32_t>(
            1, std::min<uint32_t>(uint32_t(rows_per_part_), max_slice_h_ - row));
        const uint32_t row_end = row + rows;
        const bool first = row == 0, last = row_end >= max_slice_h_;

        const uint32_t push[9] = {
            uint32_t(cfg.width), uint32_t(cfg.height), uint32_t(n_slices_),
            cap_words_, state_words_, uint32_t(cfg.context_model), uint32_t(n_wg),
            row, row_end};
        const auto t0 = prof_clock::now();
        ctx_.dispatch(pipe_, {&b_pixels_, &b_slices_, &b_trans_, &b_quant_,
                              &b_states_, &b_out_, &b_len_, &b_coder_},
                      push, uint32_t((n_wg + 31) / 32), 1, 1,
                      first ? before : none, last ? after : none,
                      first ? fills : no_fill);
        const double wall = secs_since(t0);
        ++prof_.parts;

        const gpu::Context::Times& t = ctx_.last_times();
        sum.valid = sum.valid && t.valid;
        sum.before_ns += t.before_ns;
        sum.shader_ns += t.shader_ns;
        sum.after_ns  += t.after_ns;

        // Adapt the part size to PART_TARGET_S (shader time when measured,
        // else submit + wait), at most halving or doubling per step.
        if (!last) {
            const double took = t.valid ? t.shader_ns * 1e-9 : wall;
            if (took > 0) {
                const double f = std::min(2.0, std::max(0.5, PART_TARGET_S / took));
                rows_per_part_ = std::min(double(max_slice_h_),
                                          std::max(1.0, rows_per_part_ * f));
            }
        }
        row = row_end;
    }
    prof_.dispatch += secs_since(t_all);
    ++prof_.dispatches;
    return sum;
}

uint64_t GpuEncoder::checked_output_bytes(int n) const {
    const uint32_t* len = static_cast<const uint32_t*>(b_len_dl_.mapped);
    uint64_t total = 0;
    for (int f = 0; f < n; ++f)
        for (int s = 0; s < n_slices_; ++s) {
            const size_t wg = size_t(f) * n_slices_ + s;
            if (len[wg] > cap_words_ * 4u)
                throw std::runtime_error(
                    "FFV1 GPU: slice " + std::to_string(s) + " of frame " +
                    std::to_string(f) + " needs " + std::to_string(len[wg]) +
                    " bytes, output buffer holds " +
                    std::to_string(cap_words_ * 4u) +
                    " (raise bytes_per_sample)");
            total += len[wg];
        }
    return total;
}

void GpuEncoder::encode_frames(const uint16_t* pixels, int n,
                               std::vector<std::vector<uint8_t>>& out) {
    upload_frames(pixels, n);
    run_dispatch(n);

    const auto t0 = prof_clock::now();
    checked_output_bytes(n);
    const uint32_t* len = static_cast<const uint32_t*>(b_len_dl_.mapped);
    const uint8_t*  data = static_cast<const uint8_t*>(b_out_dl_.mapped);
    out.resize(n);
    for (int f = 0; f < n; ++f) {
        std::vector<uint8_t>& frame = out[f];
        frame.clear();
        for (int s = 0; s < n_slices_; ++s) {
            const size_t wg = size_t(f) * n_slices_ + s;
            const size_t start = frame.size();
            const uint8_t* src = data + wg * cap_words_ * 4;
            frame.insert(frame.end(), src, src + len[wg]);
            append_slice_footer(frame, start);
        }
    }
    prof_.gather += secs_since(t0);
}

std::vector<GpuEncoder::BenchRun> GpuEncoder::bench(const uint16_t* pixels,
                                                    int n, int reps,
                                                    uint64_t& coded_bytes) {
    if (reps < 1) throw std::invalid_argument("FFV1 GPU: reps must be >= 1");
    const bool gpu_times = ctx_.enable_timestamps();
    upload_frames(pixels, n);
    run_dispatch(n);   // warm-up, not timed

    std::vector<BenchRun> runs(reps);
    for (BenchRun& r : runs) {
        const auto t0 = prof_clock::now();
        const gpu::Context::Times t = run_dispatch(n);
        r.wall_s = secs_since(t0);
        if (gpu_times) {
            r.gpu_times = t.valid;
            r.before_ns = t.before_ns;
            r.shader_ns = t.shader_ns;
            r.after_ns  = t.after_ns;
        }
    }
    coded_bytes = checked_output_bytes(n);
    return runs;
}

std::string GpuEncoder::memory_report() const {
    const struct { const char* name; const gpu::Buffer* b; } bufs[] = {
        {"pixels", &b_pixels_}, {"slices", &b_slices_}, {"trans", &b_trans_},
        {"quant", &b_quant_},   {"states", &b_states_}, {"out", &b_out_},
        {"len", &b_len_},       {"pix_up", &b_pixels_up_},
        {"out_dl", &b_out_dl_}, {"len_dl", &b_len_dl_}, {"coder", &b_coder_}};
    std::string r;
    for (const auto& x : bufs) {
        char head[64];
        std::snprintf(head, sizeof(head), "  %-7s %10.2f MiB  ", x.name,
                      double(x.b->size) / (1024.0 * 1024.0));
        r += head + ctx_.describe_memory(*x.b) + "\n";
    }
    return r;
}

} // namespace ffv1
} // namespace av1r

#endif // AV1R_FFV1_GPU
