// FFV1 version 3 encoder — CPU reference (RFC 9043)
//
// First version: gray16 only (single luma plane, no RCT), range coder with
// the default state transition table (coder_type = 1), all frames keyframes
// (intra = 1), per-slice CRC (ec = 1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace av1r {
namespace ffv1 {

struct Config {
    int width          = 0;
    int height         = 0;
    int num_h_slices   = 2;   // slices are the unit of GPU parallelism
    int num_v_slices   = 2;
    int context_model  = 0;   // 0 = small (Quantization Table Set 0), 1 = large (Set 1)
    int coder_type     = 2;   // 1 = range coder, default state transition table;
                              // 2 = custom table (FFmpeg ver2_state), as ffmpeg -coder 1
};

class Encoder {
public:
    explicit Encoder(const Config& cfg);

    // ConfigurationRecord for MKV CodecPrivate (RFC 9043 4.3).
    std::vector<uint8_t> extradata() const;

    // Encode one keyframe. `pixels` is row-major, width * height values.
    // The encoded Frame is appended to `out`.
    void encode_frame(const uint16_t* pixels, std::vector<uint8_t>& out);

    // For the GPU encoder, which must code with identical tables.
    const Config& config() const { return cfg_; }
    const int16_t* quant_table(int set, int input) const {
        return quant_tables_[set][input];
    }
    int context_count(int set) const { return context_count_[set]; }
    // State transition table of the Slices (default or coder_type 2 table).
    const uint8_t* state_transition() const;

private:
    static constexpr int QUANT_SET_COUNT = 2;
    static constexpr int CONTEXT_INPUTS  = 5;

    void encode_slice(const uint16_t* pixels, int sx, int sy, bool first,
                      std::vector<uint8_t>& out);

    Config cfg_;
    // quant_tables[set][input][k], RFC 9043 3.4
    int16_t quant_tables_[QUANT_SET_COUNT][CONTEXT_INPUTS][256];
    int     context_count_[QUANT_SET_COUNT];
    // Context states of the slice being coded: [context][CONTEXT_SIZE].
    std::vector<uint8_t> states_;
    // Sample ring buffer for the slice being coded: 3 rows of width + 6.
    std::vector<int32_t> rows_;
};

// CRC-32, IEEE polynomial 0x04C11DB7, initial value 0, no pre/post
// inversion, MSB first (RFC 9043 4.9.3). Stored big-endian as parity, the
// whole block then has CRC remainder 0.
uint32_t crc32_ieee(const uint8_t* data, size_t size);

// Append the Slice Footer (RFC 4.9) to a Slice whose range-coded bytes start
// at out[start]: slice_size, error_status = 0, CRC parity over the Slice.
void append_slice_footer(std::vector<uint8_t>& out, size_t start);

} // namespace ffv1
} // namespace av1r
