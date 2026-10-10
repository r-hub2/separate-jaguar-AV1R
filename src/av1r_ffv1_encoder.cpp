// FFV1 version 3 encoder — CPU reference (RFC 9043)
//
// Written from RFC 9043. Points the RFC leaves open were checked against
// FFmpeg's decoder/encoder (ffv1dec.c, ffv1enc.c, ffv1_parse.c), which is
// the decoder used for verification:
//   * the "keyframe" bit is range coded by the coder of the first Slice, at
//     the start of the Frame, and is part of that Slice's bytes;
//   * slice_size counts the range-coded bytes of the Slice (header +
//     content), not the footer;
//   * slice_crc_parity covers the whole Slice including slice_size and
//     error_status;
//   * range-coded Slices end in Sentinel mode (symbol 0 with state 129);
//   * initial states (128) are reset for every Quantization Table, not
//     once per Quantization Table Set (RFC 4.1 wording).

#include "av1r_ffv1_encoder.h"
#include "av1r_ffv1_rangecoder.h"
#include "av1r_ffv1_tables.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace av1r {
namespace ffv1 {

namespace {

constexpr int VERSION        = 3;
constexpr int MICRO_VERSION  = 4;   // first stable variant, RFC 4.2.2
constexpr int COLORSPACE     = 0;   // YCbCr
constexpr int BITS           = 16;
constexpr int EC             = 1;   // CRC in Slices and ConfigurationRecord
constexpr int INTRA          = 1;   // keyframes only
constexpr int PICTURE_STRUCT = 3;   // progressive
// quant_table_set_index_count = 1 + (chroma_planes || version <= 3)
// + extra_plane = 2 for gray, version 3 (RFC 4.6.5).
constexpr int QUANT_INDEX_COUNT = 2;

int median3(int a, int b, int c) {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

// RFC 3.8, Figure 10, bits = 16.
int fold16(int d) {
    return ((d + 32768) & 0xFFFF) - 32768;
}

// RFC 3.3.1: samples are interpreted as signed 16-bit.
int32_t to_s16(uint16_t v) {
    return v >= 32768 ? int32_t(v) - 65536 : int32_t(v);
}

void put_be(std::vector<uint8_t>& out, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i)));
}

// Build a full 256-entry table from its first half, exactly as the decoder
// reconstructs it (RFC 4.1, QuantizationTable pseudocode), so encoder and
// decoder always use identical tables. Returns len_count.
int build_table(const int8_t* src, int scale, int16_t* dst) {
    int v = 0;
    for (int k = 0; k < 128;) {
        const int8_t first = src[k];
        while (k < 128 && src[k] == first) dst[k++] = int16_t(scale * v);
        ++v;
    }
    for (int k = 1; k < 128; ++k) dst[256 - k] = int16_t(-dst[k]);
    dst[128] = int16_t(-dst[127]);
    return v;
}

// Run lengths of the first half (RFC 4.1: stored as "len - 1").
std::vector<int> run_lengths(const int16_t* t) {
    std::vector<int> runs;
    int len = 1;
    for (int k = 1; k < 128; ++k) {
        if (t[k] != t[k - 1]) { runs.push_back(len); len = 1; }
        else ++len;
    }
    runs.push_back(len);
    return runs;
}

} // namespace

namespace {

// crc32_ieee one byte at a time: entry i is the bitwise CRC of i << 24.
struct Crc32Table {
    uint32_t t[256];
    Crc32Table() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int b = 0; b < 8; ++b)
                c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
            t[i] = c;
        }
    }
};

} // namespace

uint32_t crc32_ieee(const uint8_t* data, size_t size) {
    static const Crc32Table table;
    uint32_t crc = 0;
    for (size_t i = 0; i < size; ++i)
        crc = (crc << 8) ^ table.t[(crc >> 24) ^ data[i]];
    return crc;
}

Encoder::Encoder(const Config& cfg) : cfg_(cfg) {
    if (cfg_.width <= 0 || cfg_.height <= 0)
        throw std::invalid_argument("FFV1: invalid frame size");
    if (cfg_.num_h_slices < 1 || cfg_.num_v_slices < 1 ||
        cfg_.num_h_slices > cfg_.width || cfg_.num_v_slices > cfg_.height)
        throw std::invalid_argument("FFV1: invalid slice count");
    if (cfg_.context_model != 0 && cfg_.context_model != 1)
        throw std::invalid_argument("FFV1: context_model must be 0 or 1");
    if (cfg_.coder_type != 1 && cfg_.coder_type != 2)
        throw std::invalid_argument("FFV1: coder_type must be 1 or 2");

    // Same table sets as FFmpeg uses for > 8 bit (ffv1enc.c):
    //   Set 0: Q9, 9*Q9, 81*Q9, 0, 0
    //   Set 1: Q9, 9*Q9, 81*Q5, 405*Q5, 2025*Q5
    // Scales follow RFC 4.1: scale *= 2 * len_count - 1.
    static const int8_t ZERO[256] = {};
    const int8_t* src[QUANT_SET_COUNT][CONTEXT_INPUTS] = {
        {QUANT9_10BIT, QUANT9_10BIT, QUANT9_10BIT, ZERO, ZERO},
        {QUANT9_10BIT, QUANT9_10BIT, QUANT5_10BIT, QUANT5_10BIT, QUANT5_10BIT},
    };
    for (int i = 0; i < QUANT_SET_COUNT; ++i) {
        int scale = 1;
        for (int j = 0; j < CONTEXT_INPUTS; ++j) {
            const int len_count = build_table(src[i][j], scale,
                                              quant_tables_[i][j]);
            scale *= 2 * len_count - 1;
        }
        context_count_[i] = (scale + 1) / 2;   // ceil(scale / 2)
        if (context_count_[i] > 32768)
            throw std::logic_error("FFV1: context_count > 32768");
    }
}

std::vector<uint8_t> Encoder::extradata() const {
    // ConfigurationRecord (RFC 4.3): Parameters, then CRC parity.
    std::vector<uint8_t> out;
    {
        RangeEncoder rc(out);
        uint8_t st[CONTEXT_SIZE];
        std::fill(st, st + CONTEXT_SIZE, INITIAL_STATE);

        // Parameters (RFC 4.2, Figure 28)
        rc.put_symbol(st, VERSION, false);
        rc.put_symbol(st, MICRO_VERSION, false);
        rc.put_symbol(st, cfg_.coder_type, false);
        if (cfg_.coder_type == 2) {
            // state_transition_delta in Parameters, coded with the default
            // table, as in FFmpeg ffv1enc.c write_extradata().
            const uint8_t* def = default_state_transition();
            for (int i = 1; i < 256; ++i)
                rc.put_symbol(st, int(VER2_STATE_TRANSITION[i]) - int(def[i]),
                              true);
        }
        rc.put_symbol(st, COLORSPACE, false);
        rc.put_symbol(st, BITS, false);
        rc.put_bit(st[0], false);                   // chroma_planes
        rc.put_symbol(st, 0, false);                // log2_h_chroma_subsample
        rc.put_symbol(st, 0, false);                // log2_v_chroma_subsample
        rc.put_bit(st[0], false);                   // extra_plane
        rc.put_symbol(st, cfg_.num_h_slices - 1, false);
        rc.put_symbol(st, cfg_.num_v_slices - 1, false);
        rc.put_symbol(st, QUANT_SET_COUNT, false);

        for (int i = 0; i < QUANT_SET_COUNT; ++i) {
            for (int j = 0; j < CONTEXT_INPUTS; ++j) {
                uint8_t qst[CONTEXT_SIZE];
                std::fill(qst, qst + CONTEXT_SIZE, INITIAL_STATE);
                for (int len : run_lengths(quant_tables_[i][j]))
                    rc.put_symbol(qst, len - 1, false);
            }
        }
        for (int i = 0; i < QUANT_SET_COUNT; ++i)
            rc.put_bit(st[0], false);               // states_coded = 0

        rc.put_symbol(st, EC, false);
        rc.put_symbol(st, INTRA, false);

        rc.finish();                                // Closed mode (RFC 3.8.1.1.1)
    }
    put_be(out, crc32_ieee(out.data(), out.size()), 4);
    return out;
}

const uint8_t* Encoder::state_transition() const {
    return cfg_.coder_type == 2 ? VER2_STATE_TRANSITION
                                : default_state_transition();
}

void Encoder::encode_frame(const uint16_t* pixels, std::vector<uint8_t>& out) {
    // Frame (RFC 4.4): Slices in raster order, slice i at
    // (i % num_h_slices, i / num_h_slices).
    const int n = cfg_.num_h_slices * cfg_.num_v_slices;
    for (int i = 0; i < n; ++i)
        encode_slice(pixels, i % cfg_.num_h_slices, i / cfg_.num_h_slices,
                     i == 0, out);
}

void Encoder::encode_slice(const uint16_t* pixels, int sx, int sy, bool first,
                           std::vector<uint8_t>& out) {
    // Slice geometry (RFC 4.7.3, 4.7.4, 4.8.2, 4.8.3)
    const int x0 = int(int64_t(sx) * cfg_.width / cfg_.num_h_slices);
    const int x1 = int(int64_t(sx + 1) * cfg_.width / cfg_.num_h_slices);
    const int y0 = int(int64_t(sy) * cfg_.height / cfg_.num_v_slices);
    const int y1 = int(int64_t(sy + 1) * cfg_.height / cfg_.num_v_slices);
    const int w = x1 - x0;
    const int h = y1 - y0;

    const int qset = cfg_.context_model;
    const int16_t (*q)[256] = quant_tables_[qset];

    const size_t start = out.size();
    {
        RangeEncoder rc(out);

        if (first) {
            uint8_t keystate = INITIAL_STATE;       // Frame: keyframe, own state
            rc.put_bit(keystate, true);
        }
        // The keyframe bit uses the default table; the Slice itself uses
        // the coder_type 2 table (FFmpeg ffv1enc.c encode_frame,
        // ffv1.c ff_ffv1_init_slice_state).
        rc.set_state_transition(state_transition());

        // Slice Header (RFC 4.6), own states
        uint8_t hst[CONTEXT_SIZE];
        std::fill(hst, hst + CONTEXT_SIZE, INITIAL_STATE);
        rc.put_symbol(hst, sx, false);
        rc.put_symbol(hst, sy, false);
        rc.put_symbol(hst, 0, false);               // slice_width - 1
        rc.put_symbol(hst, 0, false);               // slice_height - 1
        for (int i = 0; i < QUANT_INDEX_COUNT; ++i)
            rc.put_symbol(hst, qset, false);        // quant_table_set_index
        rc.put_symbol(hst, PICTURE_STRUCT, false);
        // SAR unknown: 0/1 as FFmpeg writes it; FFmpeg's decoder rejects
        // 0/0 ("ignoring invalid SAR", av_image_check_sar: den <= 0).
        rc.put_symbol(hst, 0, false);               // sar_num
        rc.put_symbol(hst, 1, false);               // sar_den

        // Keyframe: all context states to initial value (RFC 3.8.1.3)
        states_.assign(size_t(context_count_[qset]) * CONTEXT_SIZE,
                       INITIAL_STATE);

        // Slice Content (RFC 4.7): one plane, lines top to bottom.
        // Ring of 3 rows with 3 border samples on each side; border rules
        // RFC 3.1 (two rows above and the outer left column are 0).
        const int stride = w + 6;
        rows_.assign(size_t(3) * stride, 0);
        int32_t* cur   = rows_.data() + 3;
        int32_t* prev  = cur + stride;
        int32_t* prev2 = prev + stride;

        for (int y = 0; y < h; ++y) {
            if (y > 0) {
                int32_t* t = prev2;
                prev2 = prev;
                prev  = cur;
                cur   = t;
            }
            const uint16_t* src = pixels + size_t(y0 + y) * cfg_.width + x0;
            for (int x = 0; x < w; ++x) cur[x] = to_s16(src[x]);
            cur[-1] = prev[0];        // left border: leftmost column, one row up
            cur[-2] = 0;              // outer left column
            prev[w] = prev[w - 1];    // right border: rightmost column

            for (int x = 0; x < w; ++x) {
                const int l  = cur[x - 1];
                const int L  = cur[x - 2];
                const int t  = prev[x];
                const int tl = prev[x - 1];
                const int tr = prev[x + 1];
                const int T  = prev2[x];

                // Context (RFC 3.5)
                int ctx = q[0][(l - tl) & 255] + q[1][(tl - t) & 255] +
                          q[2][(t - tr) & 255] + q[3][(L - l) & 255] +
                          q[4][(T - t) & 255];

                // Median predictor (RFC 3.3.1) and difference (RFC 3.8)
                int diff = cur[x] - median3(l, t, l + t - tl);
                if (ctx < 0) {
                    ctx  = -ctx;
                    diff = -diff;
                }
                rc.put_symbol(&states_[size_t(ctx) * CONTEXT_SIZE],
                              fold16(diff), true);
            }
        }

        // Sentinel mode termination (RFC 3.8.1.1.1)
        uint8_t sentinel = 129;
        rc.put_bit(sentinel, false);
        rc.finish();
    }

    append_slice_footer(out, start);
}

void append_slice_footer(std::vector<uint8_t>& out, size_t start) {
    // Slice Footer (RFC 4.9)
    const size_t slice_size = out.size() - start;
    if (slice_size >= (size_t(1) << 24))
        throw std::runtime_error("FFV1: slice larger than 16 MiB, use more slices");
    put_be(out, uint32_t(slice_size), 3);           // slice_size
    out.push_back(0);                                // error_status
    put_be(out, crc32_ieee(out.data() + start, out.size() - start), 4);
}

} // namespace ffv1
} // namespace av1r
