// FFV1 range coder — CPU reference (RFC 9043, section 3.8.1)

#include "av1r_ffv1_rangecoder.h"

#include <algorithm>
#include <cstdlib>

namespace av1r {
namespace ffv1 {

// RFC 9043 3.8.1.5, Figure 24 (extracted verbatim from the RFC text).
static const uint8_t DEFAULT_STATE_TRANSITION[256] = {
      0,   0,   0,   0,   0,   0,   0,   0,  20,  21,  22,  23,  24,  25,  26,  27,
     28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  37,  38,  39,  40,  41,  42,
     43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  56,  57,
     58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,
     74,  75,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,
     89,  90,  91,  92,  93,  94,  94,  95,  96,  97,  98,  99, 100, 101, 102, 103,
    104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 114, 115, 116, 117, 118,
    119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 133,
    134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
    150, 151, 152, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164,
    165, 166, 167, 168, 169, 170, 171, 171, 172, 173, 174, 175, 176, 177, 178, 179,
    180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 190, 191, 192, 194, 194,
    195, 196, 197, 198, 199, 200, 201, 202, 202, 204, 205, 206, 207, 208, 209, 209,
    210, 211, 212, 213, 215, 215, 216, 217, 218, 219, 220, 220, 222, 223, 224, 225,
    226, 227, 227, 229, 229, 230, 231, 232, 234, 234, 235, 236, 237, 238, 239, 240,
    241, 242, 243, 244, 245, 246, 247, 248, 248,   0,   0,   0,   0,   0,   0,   0,
};

const uint8_t* default_state_transition() {
    return DEFAULT_STATE_TRANSITION;
}

RangeEncoder::RangeEncoder(std::vector<uint8_t>& out)
    : out_(out), start_(out.size()) {
    set_state_transition(default_state_transition());
}

void RangeEncoder::set_state_transition(const uint8_t table[256]) {
    // RFC 9043 3.8.1.4, Figure 23: zero_state_i = 256 - one_state_(256-i)
    one_state_[0]  = table[0];
    zero_state_[0] = 0;
    for (int i = 1; i < 256; ++i) {
        one_state_[i]        = table[i];
        zero_state_[256 - i] = uint8_t(256 - table[i]);
    }
}

void RangeEncoder::renorm() {
    while (range_ < 0x100) {
        if (outstanding_byte_ < 0) {
            outstanding_byte_ = int(low_ >> 8);
        } else if (low_ <= 0xFF00) {
            // No carry possible: emit held byte and pending 0xFFs.
            out_.push_back(uint8_t(outstanding_byte_));
            out_.insert(out_.end(), outstanding_cnt_, 0xFF);
            outstanding_cnt_  = 0;
            outstanding_byte_ = int(low_ >> 8);
        } else if (low_ >= 0x10000) {
            // Carry: propagate into held byte, pending 0xFFs become 0x00.
            out_.push_back(uint8_t(outstanding_byte_ + 1));
            out_.insert(out_.end(), outstanding_cnt_, 0x00);
            outstanding_cnt_  = 0;
            outstanding_byte_ = int(low_ >> 8) - 0x100;
        } else {
            // Undecided (0xFF in the top byte): defer.
            ++outstanding_cnt_;
        }
        low_     = (low_ & 0xFF) << 8;
        range_ <<= 8;
    }
}

void RangeEncoder::put_bit(uint8_t& state, bool bit) {
    const uint32_t range1 = (range_ * state) >> 8;
    if (!bit) {
        range_ -= range1;
        state   = zero_state_[state];
    } else {
        low_   += range_ - range1;
        range_  = range1;
        state   = one_state_[state];
    }
    renorm();
}

void RangeEncoder::put_symbol(uint8_t* states, int32_t value, bool is_signed) {
    // RFC 9043 3.8.1.2: states[0] is_zero, [1..10] exponent,
    // [22..31] mantissa, [11..21] sign.
    if (value == 0) {
        put_bit(states[0], true);
        return;
    }
    const uint32_t a = uint32_t(std::abs(int64_t(value)));
    int e = 0;
    while ((a >> (e + 1)) != 0) ++e;   // e = floor(log2(a))

    put_bit(states[0], false);
    for (int i = 0; i < e; ++i)
        put_bit(states[1 + std::min(i, 9)], true);
    put_bit(states[1 + std::min(e, 9)], false);

    for (int i = e - 1; i >= 0; --i)
        put_bit(states[22 + std::min(i, 9)], (a >> i) & 1);

    if (is_signed)
        put_bit(states[11 + std::min(e, 10)], value < 0);
}

size_t RangeEncoder::finish() {
    // Flush low_ so the decoder's 16-bit window is fully determined.
    range_ = 0xFF;
    low_  += 0xFF;
    renorm();
    range_ = 0xFF;
    renorm();
    return bytes_written();
}

// ============================================================================
// RangeDecoder — RFC 9043 3.8.1.1 / 3.8.1.2 pseudocode, Closed mode
// ============================================================================

RangeDecoder::RangeDecoder(const uint8_t* data, size_t size)
    : data_(data), size_(size) {
    set_state_transition(default_state_transition());

    // Figure 18
    range_ = 0xFF00;
    end_   = false;
    low_   = get_byte() << 8;
    low_  |= get_byte();
    if (low_ >= range_) {
        low_ = range_;
        end_ = true;
    }
}

void RangeDecoder::set_state_transition(const uint8_t table[256]) {
    one_state_[0]  = table[0];
    zero_state_[0] = 0;
    for (int i = 1; i < 256; ++i) {
        one_state_[i]        = table[i];
        zero_state_[256 - i] = uint8_t(256 - table[i]);
    }
}

uint32_t RangeDecoder::get_byte() {
    // Closed mode: bytes beyond the length are read as 0.
    return pos_ < size_ ? data_[pos_++] : 0;
}

void RangeDecoder::refill() {
    // Figure 19
    if (range_ < 256) {
        range_ = range_ * 256;
        low_   = low_ * 256;
        if (!end_) {
            low_ += get_byte();
            if (pos_ >= size_) end_ = true;
        }
    }
}

bool RangeDecoder::get_bit(uint8_t& state) {
    // Figure 20
    const uint32_t rangeoff = (range_ * state) / 256;
    range_ -= rangeoff;
    if (low_ < range_) {
        state = zero_state_[state];
        refill();
        return false;
    }
    low_  -= range_;
    state  = one_state_[state];
    range_ = rangeoff;
    refill();
    return true;
}

int32_t RangeDecoder::get_symbol(uint8_t* states, bool is_signed) {
    // Figure 21
    if (get_bit(states[0]))
        return 0;

    int e = 0;
    while (get_bit(states[1 + std::min(e, 9)])) {
        if (++e > 31) return 0;   // corrupt stream guard (not in RFC)
    }

    int64_t a = 1;
    for (int i = e - 1; i >= 0; --i)
        a = a * 2 + get_bit(states[22 + std::min(i, 9)]);

    if (!is_signed)
        return int32_t(a);
    return get_bit(states[11 + std::min(e, 10)]) ? int32_t(-a) : int32_t(a);
}

} // namespace ffv1
} // namespace av1r
