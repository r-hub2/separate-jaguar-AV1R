// FFV1 range coder (RFC 9043 3.8.1) for compute shaders.
// Line-by-line port of RangeEncoder (av1r_ffv1_rangecoder.cpp); the output
// must be byte-identical. One invocation codes one stream.
//
// The including shader must declare:
//   uint rc_out[]     - output words (storage buffer), bytes packed little-endian
//   uint rc_one[256], rc_zero[256] - state transition tables (shared),
//     or the accessors RC_ONE(s) / RC_ZERO(s)
// and either
//   uint rc_states[]  - context states, one per element (shared or buffer)
// or the accessors RC_STATE_GET(i) / RC_STATE_SET(i, v) for another layout.

#ifndef RC_STATE_GET
#define RC_STATE_GET(i)    rc_states[i]
#define RC_STATE_SET(i, v) rc_states[i] = (v)
#endif

// Transition tables; RC_ONE(s) / RC_ZERO(s) may be defined for a packed layout.
#ifndef RC_ONE
#define RC_ONE(s)  rc_one[s]
#define RC_ZERO(s) rc_zero[s]
#endif

uint rc_low;
uint rc_range;
int  rc_outstanding_byte;
int  rc_outstanding_cnt;
uint rc_pos;        // bytes emitted
uint rc_word;       // pending output word
uint rc_base;       // first word of this stream in rc_out
uint rc_cap;        // capacity in words; bytes beyond it are counted, not stored

void rc_init(uint base_word, uint cap_words) {
    rc_low = 0u;
    rc_range = 0xFF00u;
    rc_outstanding_byte = -1;
    rc_outstanding_cnt = 0;
    rc_pos = 0u;
    rc_word = 0u;
    rc_base = base_word;
    rc_cap = cap_words;
}

void rc_emit(uint b) {
    rc_word |= (b & 0xFFu) << (8u * (rc_pos & 3u));
    rc_pos++;
    if ((rc_pos & 3u) == 0u) {
        uint w = (rc_pos >> 2) - 1u;
        if (w < rc_cap) rc_out[rc_base + w] = rc_word;
        rc_word = 0u;
    }
}

void rc_renorm() {
    while (rc_range < 0x100u) {
        if (rc_outstanding_byte < 0) {
            rc_outstanding_byte = int(rc_low >> 8);
        } else if (rc_low <= 0xFF00u) {
            rc_emit(uint(rc_outstanding_byte));
            for (int i = 0; i < rc_outstanding_cnt; ++i) rc_emit(0xFFu);
            rc_outstanding_cnt = 0;
            rc_outstanding_byte = int(rc_low >> 8);
        } else if (rc_low >= 0x10000u) {
            rc_emit(uint(rc_outstanding_byte + 1));
            for (int i = 0; i < rc_outstanding_cnt; ++i) rc_emit(0x00u);
            rc_outstanding_cnt = 0;
            rc_outstanding_byte = int(rc_low >> 8) - 0x100;
        } else {
            rc_outstanding_cnt++;
        }
        rc_low = (rc_low & 0xFFu) << 8;
        rc_range <<= 8;
    }
}

void rc_put_bit(uint idx, bool bit) {
    uint state = RC_STATE_GET(idx);
    uint range1 = (rc_range * state) >> 8;
    if (!bit) {
        rc_range -= range1;
        RC_STATE_SET(idx, RC_ZERO(state));
    } else {
        rc_low += rc_range - range1;
        rc_range = range1;
        RC_STATE_SET(idx, RC_ONE(state));
    }
    rc_renorm();
}

// states: rc_states[base .. base + 31], RFC 9043 3.8.1.2
void rc_put_symbol(uint base, int value, bool is_signed) {
    if (value == 0) {
        rc_put_bit(base, true);
        return;
    }
    uint a = uint(abs(value));
    int e = findMSB(a);

    rc_put_bit(base, false);
    for (int i = 0; i < e; ++i)
        rc_put_bit(base + 1u + uint(min(i, 9)), true);
    rc_put_bit(base + 1u + uint(min(e, 9)), false);

    for (int i = e - 1; i >= 0; --i)
        rc_put_bit(base + 22u + uint(min(i, 9)), ((a >> uint(i)) & 1u) != 0u);

    if (is_signed)
        rc_put_bit(base + 11u + uint(min(e, 10)), value < 0);
}

// Returns the number of bytes in the stream (may exceed rc_cap * 4).
uint rc_finish() {
    rc_range = 0xFFu;
    rc_low += 0xFFu;
    rc_renorm();
    rc_range = 0xFFu;
    rc_renorm();
    // Flush the partial last word.
    if ((rc_pos & 3u) != 0u) {
        uint w = rc_pos >> 2;
        if (w < rc_cap) rc_out[rc_base + w] = rc_word;
    }
    return rc_pos;
}
