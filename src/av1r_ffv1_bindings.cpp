// R .Call() entry points for the FFV1 archive encoder (CPU reference and
// Vulkan GPU path) and their self-tests.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <R.h>
#include <Rinternals.h>

#include "av1r_ffv1_encoder.h"
#include "av1r_ffv1_rangecoder.h"
#include "av1r_mkv_writer.h"

#ifdef AV1R_FFV1_GPU
#include "av1r_ffv1_gpu.h"
#include "av1r_ffv1_gpu_encoder.h"
#include "av1r_ffv1_spv.h"
#endif

// R_CheckUserInterrupt() longjmps; run it under R_ToplevelExec so C++
// destructors are not skipped. Returns true if the user pressed Ctrl+C.
static void check_interrupt_cb(void*) { R_CheckUserInterrupt(); }
static bool user_interrupted() {
    return R_ToplevelExec(check_interrupt_cb, nullptr) == FALSE;
}

// Archive profiling to stderr: AV1R_FFV1_PROFILE set, non-empty, not "0".
static bool profile_enabled() {
    const char* v = std::getenv("AV1R_FFV1_PROFILE");
    return v && *v && std::strcmp(v, "0") != 0;
}

using prof_clock = std::chrono::steady_clock;
static double secs_since(prof_clock::time_point t0) {
    return std::chrono::duration<double>(prof_clock::now() - t0).count();
}

// Random range coder operations: bits with skewed probabilities and
// signed/unsigned symbols of varying magnitude. Uses R's RNG.
struct RacOp { int kind; int ctx; bool is_signed; int32_t value; };
static const int RAC_N_BIT_CTX = 8, RAC_N_SYM_CTX = 4;

static std::vector<RacOp> rac_random_ops(int n) {
    const int N_BIT_CTX = RAC_N_BIT_CTX, N_SYM_CTX = RAC_N_SYM_CTX;
    std::vector<RacOp> ops(n > 0 ? n : 0);

    GetRNGstate();
    for (auto& op : ops) {
        op.kind = unif_rand() < 0.5 ? 0 : 1;
        if (op.kind == 0) {
            // Skewed bits drive states to extremes -> small ranges, carries.
            op.ctx = int(unif_rand() * N_BIT_CTX);
            const double p1 = (op.ctx + 0.5) / N_BIT_CTX;
            op.value = (unif_rand() < p1 * p1 * p1) ? 1 : 0;
            op.is_signed = false;
        } else {
            op.ctx = int(unif_rand() * N_SYM_CTX);
            op.is_signed = unif_rand() < 0.5;
            const double u = unif_rand();
            int32_t mag;
            if (u < 0.3)       mag = 0;
            else if (u < 0.7)  mag = int32_t(unif_rand() * 16);
            else if (u < 0.95) mag = int32_t(unif_rand() * 65536);
            else               mag = int32_t(unif_rand() * (1 << 30));
            op.value = (op.is_signed && unif_rand() < 0.5) ? -mag : mag;
        }
    }
    PutRNGstate();
    return ops;
}

// Range coder self-test: encode n random operations (bits with skewed
// probabilities, signed/unsigned symbols of varying magnitude) with
// RangeEncoder, decode with the RFC-pseudocode RangeDecoder, compare.
// Uses R's RNG, so set.seed() makes it reproducible.
// Returns c(first_mismatch, n_bytes); first_mismatch = -1 if all match.
extern "C" SEXP R_av1r_ffv1_rac_selftest(SEXP r_n) {
    using namespace av1r::ffv1;
    const int n = Rf_asInteger(r_n);
    const int N_BIT_CTX = RAC_N_BIT_CTX, N_SYM_CTX = RAC_N_SYM_CTX;
    const std::vector<RacOp> ops = rac_random_ops(n);

    // Encode
    std::vector<uint8_t> buf;
    {
        uint8_t bit_st[N_BIT_CTX];
        uint8_t sym_st[N_SYM_CTX][CONTEXT_SIZE];
        std::fill(bit_st, bit_st + N_BIT_CTX, INITIAL_STATE);
        std::fill(&sym_st[0][0], &sym_st[0][0] + N_SYM_CTX * CONTEXT_SIZE,
                  INITIAL_STATE);
        RangeEncoder enc(buf);
        for (const auto& op : ops) {
            if (op.kind == 0) enc.put_bit(bit_st[op.ctx], op.value != 0);
            else enc.put_symbol(sym_st[op.ctx], op.value, op.is_signed);
        }
        enc.finish();
    }

    // Decode and compare
    int first_bad = -1;
    {
        uint8_t bit_st[N_BIT_CTX];
        uint8_t sym_st[N_SYM_CTX][CONTEXT_SIZE];
        std::fill(bit_st, bit_st + N_BIT_CTX, INITIAL_STATE);
        std::fill(&sym_st[0][0], &sym_st[0][0] + N_SYM_CTX * CONTEXT_SIZE,
                  INITIAL_STATE);
        RangeDecoder dec(buf.data(), buf.size());
        for (int i = 0; i < n; ++i) {
            const auto& op = ops[i];
            const int32_t got = op.kind == 0
                ? int32_t(dec.get_bit(bit_st[op.ctx]))
                : dec.get_symbol(sym_st[op.ctx], op.is_signed);
            if (got != op.value) { first_bad = i; break; }
        }
    }

    SEXP res = PROTECT(Rf_allocVector(INTSXP, 2));
    INTEGER(res)[0] = first_bad;
    INTEGER(res)[1] = int(buf.size());
    UNPROTECT(1);
    return res;
}

// MKV writer self-test: writes `n_frames` dummy frames (frame i is
// `frame_size` bytes of value i & 0xFF) as V_FFV1 without CodecPrivate,
// plus an optional attachment. Container structure is checked by ffprobe.
extern "C" SEXP R_av1r_mkv_selftest(SEXP r_path, SEXP r_width, SEXP r_height,
                                    SEXP r_fps, SEXP r_n_frames,
                                    SEXP r_frame_size, SEXP r_att_name,
                                    SEXP r_att_data) {
    const std::string path = CHAR(STRING_ELT(r_path, 0));
    const int n_frames   = Rf_asInteger(r_n_frames);
    const int frame_size = Rf_asInteger(r_frame_size);

    av1r::mkv::TrackInfo track;
    track.width  = Rf_asInteger(r_width);
    track.height = Rf_asInteger(r_height);
    track.fps    = Rf_asReal(r_fps);

    av1r::mkv::Attachment att;
    const bool has_att = Rf_length(r_att_name) > 0;
    if (has_att) {
        att.name = CHAR(STRING_ELT(r_att_name, 0));
        att.mime = "application/xml";
        att.data.assign(RAW(r_att_data), RAW(r_att_data) + Rf_length(r_att_data));
    }

    std::string err;
    try {
        av1r::mkv::Writer mkv;
        mkv.open(path, track);
        if (has_att) mkv.add_attachment(att);
        std::vector<uint8_t> frame(frame_size > 0 ? frame_size : 0);
        for (int i = 0; i < n_frames; ++i) {
            std::fill(frame.begin(), frame.end(), uint8_t(i & 0xFF));
            mkv.write_frame(frame.data(), frame.size(), true);
        }
        mkv.close();
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (!err.empty())
        Rf_error("%s", err.c_str());
    return Rf_ScalarInteger(n_frames);
}

// ============================================================================
// FFV1 archive session: open -> write (chunks of frames) -> close.
// Frames are streamed, so stacks larger than RAM can be encoded.
// ============================================================================

namespace {

struct ArchiveSession {
    av1r::ffv1::Encoder   enc;
#ifdef AV1R_FFV1_GPU
    std::unique_ptr<av1r::ffv1::GpuEncoder> gpu;   // null: CPU encoder
    std::vector<std::vector<uint8_t>> packets;
#endif
    av1r::mkv::Writer     mkv;
    std::vector<uint16_t> frame;    // CPU: one frame; GPU: one batch
    std::vector<uint8_t>  packet;
    int width;
    int height;
    int64_t frames_written = 0;

    // AV1R_FFV1_PROFILE: stage wall times (s), printed by R_av1r_ffv1_close
    bool   profile   = false;
    double t_convert = 0;   // int -> uint16
    double t_encode  = 0;   // CPU Encoder (GPU stages: GpuEncoder::profile)
    double t_mux     = 0;   // mkv write_frame

    explicit ArchiveSession(const av1r::ffv1::Config& cfg)
        : enc(cfg), width(cfg.width), height(cfg.height) {}
};

#ifdef AV1R_FFV1_GPU
// Output capacity per sample for GPU Slices (overflow is an error).
constexpr double GPU_BYTES_PER_SAMPLE = 4.0;

// Frames per GPU dispatch: about 1 GiB of buffers, 1..256 frames. Large
// batches keep many Slice coders busy at once (a coder is serial).
// Pixels and output exist twice (device buffer + host staging copy), the
// states once (device only); 512 bytes per Slice cover the output margin.
double gpu_bytes_per_frame(const av1r::ffv1::Encoder& enc) {
    const av1r::ffv1::Config& cfg = enc.config();
    const double px = double(cfg.width) * cfg.height;
    const double n_slices = double(cfg.num_h_slices) * cfg.num_v_slices;
    return 2.0 * px * (2.0 + GPU_BYTES_PER_SAMPLE) +
           n_slices * (2.0 * 512.0 + 32.0 * enc.context_count(cfg.context_model));
}

int gpu_auto_batch(const av1r::ffv1::Encoder& enc) {
    const double b = std::floor(1024.0 * 1024 * 1024 / gpu_bytes_per_frame(enc));
    return int(std::max(1.0, std::min(256.0, b)));
}
#endif

void session_finalizer(SEXP ptr) {
    // Abandoned session: Writer destructor closes the (incomplete) file.
    delete static_cast<ArchiveSession*>(R_ExternalPtrAddr(ptr));
    R_ClearExternalPtr(ptr);
}

ArchiveSession* get_session(SEXP ptr) {
    if (TYPEOF(ptr) != EXTPTRSXP || !R_ExternalPtrAddr(ptr))
        Rf_error("FFV1 archive session is closed or invalid");
    return static_cast<ArchiveSession*>(R_ExternalPtrAddr(ptr));
}

} // namespace

// output: path to .mkv; width, height; fps; slices = c(h, v);
// context_model: 0 small / 1 large;
// att_name, att_mime: character(0) or length 1; att_data: raw vector;
// backend: "cpu" or "vulkan" (FFV1 GPU path, errors if unavailable).
extern "C" SEXP R_av1r_ffv1_open(SEXP r_output, SEXP r_width, SEXP r_height,
                                 SEXP r_fps, SEXP r_slices,
                                 SEXP r_context_model, SEXP r_att_name,
                                 SEXP r_att_mime, SEXP r_att_data,
                                 SEXP r_backend) {
    const std::string output = CHAR(STRING_ELT(r_output, 0));
    const std::string backend = CHAR(STRING_ELT(r_backend, 0));
    if (backend != "cpu" && backend != "vulkan")
        Rf_error("backend must be \"cpu\" or \"vulkan\"");
#ifndef AV1R_FFV1_GPU
    if (backend == "vulkan")
        Rf_error("AV1R was built without the FFV1 GPU path (needs Vulkan and glslc)");
#endif

    av1r::ffv1::Config cfg;
    cfg.width         = Rf_asInteger(r_width);
    cfg.height        = Rf_asInteger(r_height);
    cfg.num_h_slices  = INTEGER(r_slices)[0];
    cfg.num_v_slices  = INTEGER(r_slices)[1];
    cfg.context_model = Rf_asInteger(r_context_model);
    const double fps  = Rf_asReal(r_fps);

    const bool has_att = Rf_length(r_att_name) > 0;
    av1r::mkv::Attachment att;
    if (has_att) {
        att.name = CHAR(STRING_ELT(r_att_name, 0));
        att.mime = CHAR(STRING_ELT(r_att_mime, 0));
        att.data.assign(RAW(r_att_data), RAW(r_att_data) + Rf_length(r_att_data));
    }

    ArchiveSession* s = nullptr;
    std::string err;
    try {
        s = new ArchiveSession(cfg);
        s->profile = profile_enabled();
#ifdef AV1R_FFV1_GPU
        if (backend == "vulkan") {
            s->gpu.reset(new av1r::ffv1::GpuEncoder(
                cfg, gpu_auto_batch(s->enc), GPU_BYTES_PER_SAMPLE));
            if (s->profile)
                REprintf("AV1R FFV1 profile: %s, %dx%d, slices %dx%d, ctx %d, "
                         "batch %d\n%s",
                         s->gpu->device_name().c_str(), cfg.width, cfg.height,
                         cfg.num_h_slices, cfg.num_v_slices, cfg.context_model,
                         s->gpu->batch(), s->gpu->memory_report().c_str());
        }
#endif
        av1r::mkv::TrackInfo track;
        track.width         = cfg.width;
        track.height        = cfg.height;
        track.fps           = fps;
        // FFV1 as FFmpeg stores it in Matroska (V_MS/VFW/FOURCC, biBitCount
        // 24 as FFmpeg writes): readable by FFmpeg, VLC, GStreamer and mpv,
        // which has no "V_FFV1" mapping.
        track.codec_id      = "V_MS/VFW/FOURCC";
        track.codec_private = av1r::mkv::vfw_codec_private(
            "FFV1", cfg.width, cfg.height, 24, s->enc.extradata());
        s->mkv.open(output, track);
        if (has_att) s->mkv.add_attachment(att);
        size_t frames_buf = 1;
#ifdef AV1R_FFV1_GPU
        if (s->gpu) frames_buf = size_t(s->gpu->batch());
#endif
        s->frame.resize(size_t(cfg.width) * cfg.height * frames_buf);
    } catch (const std::exception& e) {
        err = e.what();
        delete s;
        s = nullptr;
    }
    if (!err.empty())
        Rf_error("%s", err.c_str());

    SEXP ptr = PROTECT(R_MakeExternalPtr(s, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(ptr, session_finalizer, TRUE);
    UNPROTECT(1);
    return ptr;
}

// R frames [height, width, n] (column-major) -> row-major uint16 frames,
// n frames from src to dst. Throws on NA or values outside 0..65535;
// first_frame is only used in the message (0-based index in the call).
static void convert_frames(const int* src, int height, int width, int n,
                           uint16_t* dst, int first_frame) {
    const size_t npix = size_t(width) * height;
    const int TILE = 64;   // transpose in tiles: sequential reads and writes
    for (int f = 0; f < n; ++f) {
        const int* s = src + npix * size_t(f);
        uint16_t*  d = dst + npix * size_t(f);
        for (int x0 = 0; x0 < width; x0 += TILE)
            for (int y0 = 0; y0 < height; y0 += TILE) {
                const int x1 = std::min(x0 + TILE, width);
                const int y1 = std::min(y0 + TILE, height);
                for (int x = x0; x < x1; ++x)
                    for (int y = y0; y < y1; ++y) {
                        const int v = s[size_t(x) * height + y];
                        // NA_INTEGER is INT_MIN, caught by v < 0
                        if (v < 0 || v > 65535)
                            throw std::invalid_argument(
                                "frames must contain gray16 values in 0..65535 "
                                "without NA (frame " +
                                std::to_string(first_frame + f + 1) + ")");
                        d[size_t(y) * width + x] = uint16_t(v);
                    }
            }
    }
}

// Row-major gray16le bytes (as ffmpeg -pix_fmt gray16le writes them) ->
// uint16 frames; n frames of npix samples.
static void convert_gray16le(const uint8_t* src, size_t npix, int n,
                             uint16_t* dst) {
    const size_t count = npix * size_t(n);
    for (size_t j = 0; j < count; ++j)
        dst[j] = uint16_t(src[2 * j] | (src[2 * j + 1] << 8));
}

// frames: either an integer array dim c(height, width, k) as R holds
// images, values 0..65535 (transposed and checked here), or a raw vector
// of k row-major gray16le frames (k * width * height * 2 bytes).
extern "C" SEXP R_av1r_ffv1_write(SEXP r_ptr, SEXP r_frames) {
    ArchiveSession* s = get_session(r_ptr);
    const size_t npix = size_t(s->width) * s->height;
    const bool raw = TYPEOF(r_frames) == RAWSXP;
    int k = 0;
    if (raw) {
        const double len = double(XLENGTH(r_frames));
        if (std::fmod(len, 2.0 * double(npix)) != 0)
            Rf_error("raw frames: %.0f bytes is not a multiple of the frame "
                     "size %.0f (gray16le %dx%d)", len, 2.0 * double(npix),
                     s->width, s->height);
        k = int(len / (2.0 * double(npix)));
    } else {
        SEXP dim = Rf_getAttrib(r_frames, R_DimSymbol);
        if (TYPEOF(r_frames) != INTSXP || Rf_length(dim) != 3)
            Rf_error("frames must be an integer array [height, width, n] "
                     "or a raw vector of gray16le frames");
        if (INTEGER(dim)[1] != s->width || INTEGER(dim)[0] != s->height)
            Rf_error("frame size %dx%d does not match session %dx%d",
                     INTEGER(dim)[1], INTEGER(dim)[0], s->width, s->height);
        k = INTEGER(dim)[2];
    }

    // Frames i0 .. i0 + m - 1 of this call into dst (row-major uint16).
    auto fill = [&](int i0, int m, uint16_t* dst) {
        if (raw)
            convert_gray16le(RAW(r_frames) + 2 * npix * size_t(i0), npix, m, dst);
        else
            convert_frames(INTEGER(r_frames) + npix * size_t(i0), s->height,
                           s->width, m, dst, i0);
    };

    std::string err;
    try {
#ifdef AV1R_FFV1_GPU
        if (s->gpu) {
            const int batch = s->gpu->batch();
            for (int i0 = 0; i0 < k; i0 += batch) {
                const int m = std::min(batch, k - i0);
                auto t0 = prof_clock::now();
                fill(i0, m, s->frame.data());
                s->t_convert += secs_since(t0);
                s->gpu->encode_frames(s->frame.data(), m, s->packets);
                t0 = prof_clock::now();
                for (int f = 0; f < m; ++f) {
                    s->mkv.write_frame(s->packets[f].data(),
                                       s->packets[f].size(), true);
                    ++s->frames_written;
                }
                s->t_mux += secs_since(t0);
                if (user_interrupted())
                    throw std::runtime_error("interrupted by user");
            }
        } else
#endif
        for (int i = 0; i < k; ++i) {
            auto t0 = prof_clock::now();
            fill(i, 1, s->frame.data());
            s->t_convert += secs_since(t0);
            s->packet.clear();
            t0 = prof_clock::now();
            s->enc.encode_frame(s->frame.data(), s->packet);
            s->t_encode += secs_since(t0);
            t0 = prof_clock::now();
            s->mkv.write_frame(s->packet.data(), s->packet.size(), true);
            s->t_mux += secs_since(t0);
            ++s->frames_written;
            if (user_interrupted())
                throw std::runtime_error("interrupted by user");
        }
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (!err.empty())
        Rf_error("%s", err.c_str());
    return Rf_ScalarReal(double(s->frames_written));
}

// Finalize the file (Cues, sizes, Duration). Returns frames written.
extern "C" SEXP R_av1r_ffv1_close(SEXP r_ptr) {
    ArchiveSession* s = get_session(r_ptr);
    const double n = double(s->frames_written);
    std::string err;
    try {
        s->mkv.close();
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (s->profile) {
        REprintf("AV1R FFV1 profile: %.0f frames\n", n);
        REprintf("  convert  %8.3f s\n", s->t_convert);
#ifdef AV1R_FFV1_GPU
        if (s->gpu) {
            const av1r::ffv1::GpuEncoder::Profile& p = s->gpu->profile();
            REprintf("  upload   %8.3f s\n", p.upload);
            REprintf("  dispatch %8.3f s  (%lld dispatches, %lld parts)\n", p.dispatch,
                     static_cast<long long>(p.dispatches),
                     static_cast<long long>(p.parts));
            REprintf("  gather   %8.3f s\n", p.gather);
        } else
#endif
        REprintf("  encode   %8.3f s\n", s->t_encode);
        REprintf("  mux      %8.3f s\n", s->t_mux);
    }
    delete s;
    R_ClearExternalPtr(r_ptr);
    if (!err.empty())
        Rf_error("%s", err.c_str());
    return Rf_ScalarReal(n);
}

// Abandon the session without finalizing (file is left incomplete; the R
// side deletes it).
extern "C" SEXP R_av1r_ffv1_abort(SEXP r_ptr) {
    if (TYPEOF(r_ptr) == EXTPTRSXP && R_ExternalPtrAddr(r_ptr)) {
        delete static_cast<ArchiveSession*>(R_ExternalPtrAddr(r_ptr));
        R_ClearExternalPtr(r_ptr);
    }
    return R_NilValue;
}

// GPU path self-test: run the selftest shader on n random words, compare
// with dst[i] = src[i] * 2654435761 + i computed on the CPU.
// Returns NULL if built without the FFV1 GPU path, otherwise
// list(device = <name>, first_mismatch = <1-based index or -1>).
// Errors if the GPU path is built but no usable Vulkan device exists.
extern "C" SEXP R_av1r_ffv1_gpu_selftest(SEXP r_n) {
#ifdef AV1R_FFV1_GPU
    using namespace av1r::ffv1::gpu;
    const int n = Rf_asInteger(r_n);
    if (n <= 0) Rf_error("n must be positive");

    std::vector<uint32_t> src(n);
    GetRNGstate();
    for (auto& v : src) v = uint32_t(unif_rand() * 4294967296.0);
    PutRNGstate();

    std::string err, device;
    int first_mismatch = -1;
    try {
        Context ctx;
        device = ctx.device_name();
        Buffer b_src = ctx.create_buffer(sizeof(uint32_t) * n);
        Buffer b_dst = ctx.create_buffer(sizeof(uint32_t) * n);
        Pipeline p = ctx.create_pipeline(av1r_ffv1_spv_selftest,
                                         sizeof(av1r_ffv1_spv_selftest),
                                         2, sizeof(uint32_t));
        std::memcpy(b_src.mapped, src.data(), sizeof(uint32_t) * n);
        std::memset(b_dst.mapped, 0, sizeof(uint32_t) * n);
        const uint32_t push = uint32_t(n);
        ctx.dispatch(p, {&b_src, &b_dst}, &push, (uint32_t(n) + 63) / 64, 1, 1);

        const uint32_t* dst = static_cast<const uint32_t*>(b_dst.mapped);
        for (int i = 0; i < n; ++i)
            if (dst[i] != src[i] * 2654435761u + uint32_t(i)) {
                first_mismatch = i + 1;
                break;
            }
        ctx.destroy_pipeline(p);
        ctx.destroy_buffer(b_dst);
        ctx.destroy_buffer(b_src);
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (!err.empty())
        Rf_error("%s", err.c_str());

    SEXP res = PROTECT(Rf_allocVector(VECSXP, 2));
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, 2));
    SET_VECTOR_ELT(res, 0, Rf_mkString(device.c_str()));
    SET_VECTOR_ELT(res, 1, Rf_ScalarInteger(first_mismatch));
    SET_STRING_ELT(nms, 0, Rf_mkChar("device"));
    SET_STRING_ELT(nms, 1, Rf_mkChar("first_mismatch"));
    Rf_setAttrib(res, R_NamesSymbol, nms);
    UNPROTECT(2);
    return res;
#else
    (void)r_n;
    return R_NilValue;
#endif
}

// GPU range coder test: n_ops random operations (as in rac_selftest) split
// into n_streams streams of random length; workgroup k codes stream k with
// rangecoder.glsl, the CPU codes it with RangeEncoder; bytes are compared.
// custom_table = TRUE uses a random state transition table (coder_type 2
// style) instead of the default one.
// Returns NULL if built without the FFV1 GPU path, otherwise
// list(device, first_mismatch_stream (1-based or -1), n_bytes).
extern "C" SEXP R_av1r_ffv1_gpu_rac_selftest(SEXP r_streams, SEXP r_ops,
                                              SEXP r_custom) {
#ifdef AV1R_FFV1_GPU
    using namespace av1r::ffv1;
    using namespace av1r::ffv1::gpu;
    const int n_streams = Rf_asInteger(r_streams);
    const int n_ops     = Rf_asInteger(r_ops);
    const bool custom   = Rf_asLogical(r_custom) == TRUE;
    if (n_streams <= 0 || n_ops < 0) Rf_error("invalid n_streams / n_ops");

    const std::vector<RacOp> ops = rac_random_ops(n_ops);

    // Stream boundaries (random cut points) and transition table.
    std::vector<uint32_t> cuts(n_streams + 1, 0);
    uint8_t table[256];
    std::memcpy(table, default_state_transition(), 256);
    GetRNGstate();
    for (int k = 1; k < n_streams; ++k)
        cuts[k] = uint32_t(unif_rand() * (n_ops + 1));
    if (custom)
        for (int i = 1; i < 256; ++i) table[i] = uint8_t(1 + unif_rand() * 255);
    PutRNGstate();
    cuts[n_streams] = uint32_t(n_ops);
    std::sort(cuts.begin(), cuts.end());

    // CPU reference.
    std::vector<std::vector<uint8_t>> ref(n_streams);
    size_t max_len = 0, total = 0;
    for (int k = 0; k < n_streams; ++k) {
        uint8_t bit_st[RAC_N_BIT_CTX];
        uint8_t sym_st[RAC_N_SYM_CTX][CONTEXT_SIZE];
        std::fill(bit_st, bit_st + RAC_N_BIT_CTX, INITIAL_STATE);
        std::fill(&sym_st[0][0], &sym_st[0][0] + RAC_N_SYM_CTX * CONTEXT_SIZE,
                  INITIAL_STATE);
        RangeEncoder enc(ref[k]);
        enc.set_state_transition(table);
        for (uint32_t i = cuts[k]; i < cuts[k + 1]; ++i) {
            const RacOp& op = ops[i];
            if (op.kind == 0) enc.put_bit(bit_st[op.ctx], op.value != 0);
            else enc.put_symbol(sym_st[op.ctx], op.value, op.is_signed);
        }
        enc.finish();
        max_len = std::max(max_len, ref[k].size());
        total += ref[k].size();
    }

    // GPU input: ops as {kind, ctx, is_signed, value}, streams as {first, n},
    // transition tables as one_state[256], zero_state[256] (Figure 23).
    std::vector<uint32_t> g_ops(4 * std::max(n_ops, 1), 0);
    for (int i = 0; i < n_ops; ++i) {
        g_ops[4 * i]     = uint32_t(ops[i].kind);
        g_ops[4 * i + 1] = uint32_t(ops[i].ctx);
        g_ops[4 * i + 2] = ops[i].is_signed ? 1u : 0u;
        g_ops[4 * i + 3] = uint32_t(ops[i].value);
    }
    std::vector<uint32_t> g_streams(2 * n_streams);
    for (int k = 0; k < n_streams; ++k) {
        g_streams[2 * k]     = cuts[k];
        g_streams[2 * k + 1] = cuts[k + 1] - cuts[k];
    }
    std::vector<uint32_t> g_trans(512, 0);
    g_trans[0] = table[0];
    for (int i = 1; i < 256; ++i) {
        g_trans[i]             = table[i];
        g_trans[256 + 256 - i] = uint32_t(uint8_t(256 - table[i]));
    }
    // Capacity with headroom so an encoder bug shows as a length mismatch,
    // not as truncated output.
    const uint32_t cap_words = uint32_t(max_len / 4 + 16);

    std::string err, device;
    int first_bad = -1;
    try {
        Context ctx;
        device = ctx.device_name();
        Buffer b_ops = ctx.create_buffer(g_ops.size() * 4);
        Buffer b_str = ctx.create_buffer(g_streams.size() * 4);
        Buffer b_tr  = ctx.create_buffer(g_trans.size() * 4);
        Buffer b_out = ctx.create_buffer(VkDeviceSize(cap_words) * 4 * n_streams);
        Buffer b_len = ctx.create_buffer(VkDeviceSize(n_streams) * 4);
        std::memcpy(b_ops.mapped, g_ops.data(), g_ops.size() * 4);
        std::memcpy(b_str.mapped, g_streams.data(), g_streams.size() * 4);
        std::memcpy(b_tr.mapped, g_trans.data(), g_trans.size() * 4);
        Pipeline p = ctx.create_pipeline(av1r_ffv1_spv_rac_test,
                                         sizeof(av1r_ffv1_spv_rac_test),
                                         5, sizeof(uint32_t));
        ctx.dispatch(p, {&b_ops, &b_str, &b_tr, &b_out, &b_len}, &cap_words,
                     uint32_t(n_streams), 1, 1);

        const uint32_t* len = static_cast<const uint32_t*>(b_len.mapped);
        const uint8_t*  out = static_cast<const uint8_t*>(b_out.mapped);
        for (int k = 0; k < n_streams && first_bad < 0; ++k) {
            const uint8_t* got = out + size_t(k) * cap_words * 4;
            if (len[k] != ref[k].size() ||
                std::memcmp(got, ref[k].data(), ref[k].size()) != 0)
                first_bad = k + 1;
        }
        ctx.destroy_pipeline(p);
        ctx.destroy_buffer(b_len);
        ctx.destroy_buffer(b_out);
        ctx.destroy_buffer(b_tr);
        ctx.destroy_buffer(b_str);
        ctx.destroy_buffer(b_ops);
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (!err.empty())
        Rf_error("%s", err.c_str());

    SEXP res = PROTECT(Rf_allocVector(VECSXP, 3));
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, 3));
    SET_VECTOR_ELT(res, 0, Rf_mkString(device.c_str()));
    SET_VECTOR_ELT(res, 1, Rf_ScalarInteger(first_bad));
    SET_VECTOR_ELT(res, 2, Rf_ScalarReal(double(total)));
    SET_STRING_ELT(nms, 0, Rf_mkChar("device"));
    SET_STRING_ELT(nms, 1, Rf_mkChar("first_mismatch_stream"));
    SET_STRING_ELT(nms, 2, Rf_mkChar("n_bytes"));
    Rf_setAttrib(res, R_NamesSymbol, nms);
    UNPROTECT(2);
    return res;
#else
    (void)r_streams; (void)r_ops; (void)r_custom;
    return R_NilValue;
#endif
}

// GPU Slice encoder test: encode frames ([width, height, n] integer array,
// 0..65535) with the CPU Encoder and with GpuEncoder, compare whole Frames.
// Returns NULL if built without the FFV1 GPU path, otherwise
// list(device, first_mismatch_frame (1-based or -1), first_mismatch_byte
// (1-based or -1), n_bytes). Errors if a Slice overflows its GPU buffer.
extern "C" SEXP R_av1r_ffv1_gpu_slice_selftest(SEXP r_frames, SEXP r_nh,
                                                SEXP r_nv, SEXP r_ctx_model,
                                                SEXP r_batch, SEXP r_bps) {
#ifdef AV1R_FFV1_GPU
    using namespace av1r::ffv1;
    SEXP dim = Rf_getAttrib(r_frames, R_DimSymbol);
    if (TYPEOF(r_frames) != INTSXP || Rf_length(dim) != 3)
        Rf_error("frames must be an integer array [width, height, n]");
    Config cfg;
    cfg.width         = INTEGER(dim)[0];
    cfg.height        = INTEGER(dim)[1];
    cfg.num_h_slices  = Rf_asInteger(r_nh);
    cfg.num_v_slices  = Rf_asInteger(r_nv);
    cfg.context_model = Rf_asInteger(r_ctx_model);
    const int n       = INTEGER(dim)[2];
    const int batch   = Rf_asInteger(r_batch);
    const double bps  = Rf_asReal(r_bps);

    const size_t frame_px = size_t(cfg.width) * cfg.height;
    std::vector<uint16_t> px(frame_px * n);
    const int* src = INTEGER(r_frames);
    for (size_t i = 0; i < px.size(); ++i) {
        if (src[i] == NA_INTEGER || src[i] < 0 || src[i] > 65535)
            Rf_error("sample values must be in 0..65535");
        px[i] = uint16_t(src[i]);
    }

    std::string err, device;
    int bad_frame = -1;
    double bad_byte = -1, total = 0;
    try {
        Encoder cpu(cfg);
        GpuEncoder gpu(cfg, batch, bps);
        device = gpu.device_name();
        std::vector<std::vector<uint8_t>> got;
        for (int f0 = 0; f0 < n && bad_frame < 0; f0 += batch) {
            const int m = std::min(batch, n - f0);
            gpu.encode_frames(px.data() + frame_px * f0, m, got);
            for (int k = 0; k < m; ++k) {
                std::vector<uint8_t> ref;
                cpu.encode_frame(px.data() + frame_px * (f0 + k), ref);
                total += double(ref.size());
                const std::vector<uint8_t>& g = got[k];
                const size_t common = std::min(ref.size(), g.size());
                size_t i = 0;
                while (i < common && ref[i] == g[i]) ++i;
                if (i < common || ref.size() != g.size()) {
                    bad_frame = f0 + k + 1;
                    bad_byte  = double(i + 1);
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (!err.empty())
        Rf_error("%s", err.c_str());

    SEXP res = PROTECT(Rf_allocVector(VECSXP, 4));
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, 4));
    SET_VECTOR_ELT(res, 0, Rf_mkString(device.c_str()));
    SET_VECTOR_ELT(res, 1, Rf_ScalarInteger(bad_frame));
    SET_VECTOR_ELT(res, 2, Rf_ScalarReal(bad_byte));
    SET_VECTOR_ELT(res, 3, Rf_ScalarReal(total));
    SET_STRING_ELT(nms, 0, Rf_mkChar("device"));
    SET_STRING_ELT(nms, 1, Rf_mkChar("first_mismatch_frame"));
    SET_STRING_ELT(nms, 2, Rf_mkChar("first_mismatch_byte"));
    SET_STRING_ELT(nms, 3, Rf_mkChar("n_bytes"));
    Rf_setAttrib(res, R_NamesSymbol, nms);
    UNPROTECT(2);
    return res;
#else
    (void)r_frames; (void)r_nh; (void)r_nv; (void)r_ctx_model;
    (void)r_batch; (void)r_bps;
    return R_NilValue;
#endif
}

// GPU shader benchmark (inst/examples/profile_ffv1_shader.R).
// frames: integer array [width, height, k] (row-major frames, values
// 0..65535); the batch takes frames 1..k cyclically. slices = c(h, v);
// batch: NA for the archive default (gpu_auto_batch); reps: timed
// dispatches; max_mib: estimated buffer size above which the run is skipped.
// Returns NULL if built without the FFV1 GPU path, otherwise a list; a run
// that is skipped or fails has `skipped` set to the reason.
extern "C" SEXP R_av1r_ffv1_gpu_bench(SEXP r_frames, SEXP r_slices,
                                      SEXP r_ctx_model, SEXP r_batch,
                                      SEXP r_reps, SEXP r_max_mib) {
#ifdef AV1R_FFV1_GPU
    using namespace av1r::ffv1;
    SEXP dim = Rf_getAttrib(r_frames, R_DimSymbol);
    if (TYPEOF(r_frames) != INTSXP || Rf_length(dim) != 3)
        Rf_error("frames must be an integer array [width, height, n]");
    if (Rf_length(r_slices) != 2) Rf_error("slices must be c(h, v)");
    Config cfg;
    cfg.width         = INTEGER(dim)[0];
    cfg.height        = INTEGER(dim)[1];
    cfg.num_h_slices  = INTEGER(r_slices)[0];
    cfg.num_v_slices  = INTEGER(r_slices)[1];
    cfg.context_model = Rf_asInteger(r_ctx_model);
    const int k       = INTEGER(dim)[2];
    const int reps    = Rf_asInteger(r_reps);
    const double max_mib = Rf_asReal(r_max_mib);
    if (k < 1 || reps < 1) Rf_error("need at least one frame and one rep");

    std::string err, skipped, device;
    int batch = Rf_asInteger(r_batch);
    double est_mib = NA_REAL;
    uint64_t coded = 0;
    std::vector<GpuEncoder::BenchRun> runs;
    try {
        Encoder enc(cfg);
        if (batch == NA_INTEGER) batch = gpu_auto_batch(enc);
        if (batch < 1) throw std::invalid_argument("batch must be >= 1");
        est_mib = gpu_bytes_per_frame(enc) * batch / (1024.0 * 1024.0);
        if (est_mib > max_mib) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "needs ~%.0f MiB > max_mib %.0f",
                          est_mib, max_mib);
            skipped = msg;
        } else {
            const size_t frame_px = size_t(cfg.width) * cfg.height;
            std::vector<uint16_t> px(frame_px * batch);
            const int* src = INTEGER(r_frames);
            for (int f = 0; f < batch; ++f) {
                const int* p = src + frame_px * size_t(f % k);
                for (size_t j = 0; j < frame_px; ++j) {
                    if (p[j] == NA_INTEGER || p[j] < 0 || p[j] > 65535)
                        throw std::invalid_argument(
                            "sample values must be in 0..65535");
                    px[frame_px * f + j] = uint16_t(p[j]);
                }
            }
            GpuEncoder gpu(cfg, batch, GPU_BYTES_PER_SAMPLE);
            device = gpu.device_name();
            runs = gpu.bench(px.data(), batch, reps, coded);
        }
    } catch (const std::exception& e) {
        skipped = e.what();
    }

    const int n = int(runs.size());
    const bool gpu_times = n > 0 && runs[0].gpu_times;
    SEXP wall   = PROTECT(Rf_allocVector(REALSXP, n));
    SEXP before = PROTECT(Rf_allocVector(REALSXP, n));
    SEXP shader = PROTECT(Rf_allocVector(REALSXP, n));
    SEXP after  = PROTECT(Rf_allocVector(REALSXP, n));
    for (int i = 0; i < n; ++i) {
        REAL(wall)[i]   = runs[i].wall_s;
        REAL(before)[i] = gpu_times ? runs[i].before_ns : NA_REAL;
        REAL(shader)[i] = gpu_times ? runs[i].shader_ns : NA_REAL;
        REAL(after)[i]  = gpu_times ? runs[i].after_ns  : NA_REAL;
    }

    const char* names[] = {"device", "batch", "n_slices", "samples",
                           "coded_bytes", "est_mib", "timer", "wall_s",
                           "before_ns", "shader_ns", "after_ns", "skipped"};
    const int nn = int(sizeof(names) / sizeof(names[0]));
    SEXP res = PROTECT(Rf_allocVector(VECSXP, nn));
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, nn));
    SET_VECTOR_ELT(res, 0, device.empty() ? Rf_ScalarString(NA_STRING)
                                          : Rf_mkString(device.c_str()));
    SET_VECTOR_ELT(res, 1, Rf_ScalarInteger(batch));
    SET_VECTOR_ELT(res, 2, Rf_ScalarInteger(cfg.num_h_slices * cfg.num_v_slices));
    SET_VECTOR_ELT(res, 3, Rf_ScalarReal(double(cfg.width) * cfg.height * batch));
    SET_VECTOR_ELT(res, 4, Rf_ScalarReal(n > 0 ? double(coded) : NA_REAL));
    SET_VECTOR_ELT(res, 5, Rf_ScalarReal(est_mib));
    SET_VECTOR_ELT(res, 6, Rf_mkString(gpu_times ? "gpu" : "wall"));
    SET_VECTOR_ELT(res, 7, wall);
    SET_VECTOR_ELT(res, 8, before);
    SET_VECTOR_ELT(res, 9, shader);
    SET_VECTOR_ELT(res, 10, after);
    SET_VECTOR_ELT(res, 11, skipped.empty() ? Rf_ScalarString(NA_STRING)
                                            : Rf_mkString(skipped.c_str()));
    for (int i = 0; i < nn; ++i) SET_STRING_ELT(nms, i, Rf_mkChar(names[i]));
    Rf_setAttrib(res, R_NamesSymbol, nms);
    UNPROTECT(6);
    return res;
#else
    (void)r_frames; (void)r_slices; (void)r_ctx_model; (void)r_batch;
    (void)r_reps; (void)r_max_mib;
    return R_NilValue;
#endif
}

// FFV1 GPU device: NULL if built without the FFV1 GPU path, NA if no usable
// Vulkan compute device, otherwise the device name.
extern "C" SEXP R_av1r_ffv1_gpu_device() {
#ifdef AV1R_FFV1_GPU
    std::string name;
    bool ok = true;
    try {
        av1r::ffv1::gpu::Context ctx;
        name = ctx.device_name();
    } catch (const std::exception&) {
        ok = false;
    }
    return ok ? Rf_mkString(name.c_str()) : Rf_ScalarString(NA_STRING);
#else
    return R_NilValue;
#endif
}
