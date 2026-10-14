// FFV1 range coder (RFC 9043, section 3.8.1)
//
// CPU reference implementation. The Vulkan compute shaders must produce a
// byte-identical bitstream, so this code is the ground truth for the GPU path.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace av1r {
namespace ffv1 {

// Number of adaptive states per context (one byte each), RFC 9043 3.8.1.2.
constexpr int CONTEXT_SIZE = 32;

// Initial value of every context state.
constexpr uint8_t INITIAL_STATE = 128;

// Default state transition table (coder_type = 1), RFC 9043 3.8.1.5.
const uint8_t* default_state_transition();

class RangeEncoder {
public:
    // Appends encoded bytes to `out`.
    explicit RangeEncoder(std::vector<uint8_t>& out);

    // Encode one binary decision with an adaptive 8-bit state (RFC 9043 3.8.1.1).
    void put_bit(uint8_t& state, bool bit);

    // Encode an integer with a context of CONTEXT_SIZE states (RFC 9043 3.8.1.2).
    void put_symbol(uint8_t* states, int32_t value, bool is_signed);

    // Use a custom state transition table (coder_type = 2), RFC 9043 3.8.1.4.
    void set_state_transition(const uint8_t table[256]);

    // Terminate the coder and flush pending bytes.
    // Returns number of bytes written since construction.
    size_t finish();

    // Bytes written so far (pending carry bytes not included).
    size_t bytes_written() const { return out_.size() - start_; }

private:
    void renorm();

    std::vector<uint8_t>& out_;
    size_t   start_;
    uint32_t low_              = 0;
    uint32_t range_            = 0xFF00;
    int      outstanding_byte_ = -1;   // byte held back for carry propagation
    int      outstanding_cnt_  = 0;    // number of 0xFF bytes after it
    uint8_t  one_state_[256];
    uint8_t  zero_state_[256];
};

// Range decoder written strictly from the RFC 9043 pseudocode
// (Figures 18-21), Closed mode: bytes beyond `size` are read as 0.
// Used to verify RangeEncoder independently of FFmpeg.
class RangeDecoder {
public:
    RangeDecoder(const uint8_t* data, size_t size);

    bool    get_bit(uint8_t& state);                         // Figure 20
    int32_t get_symbol(uint8_t* states, bool is_signed);     // Figure 21

    void set_state_transition(const uint8_t table[256]);

private:
    uint32_t get_byte();
    void     refill();                                       // Figure 19

    const uint8_t* data_;
    size_t   size_;
    size_t   pos_   = 0;
    uint32_t low_   = 0;
    uint32_t range_ = 0xFF00;
    bool     end_   = false;
    uint8_t  one_state_[256];
    uint8_t  zero_state_[256];
};

} // namespace ffv1
} // namespace av1r
