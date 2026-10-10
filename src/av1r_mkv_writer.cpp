// Minimal Matroska (MKV) writer for a single FFV1 video track.

#include "av1r_mkv_writer.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace av1r {
namespace mkv {

namespace {

// Element IDs. EBML header: RFC 8794 11.2; Void: RFC 8794 11.3.2;
// Matroska: RFC 9559 section 5.
enum : uint32_t {
    ID_EBML               = 0x1A45DFA3,
    ID_EBMLVersion        = 0x4286,
    ID_EBMLReadVersion    = 0x42F7,
    ID_EBMLMaxIDLength    = 0x42F2,
    ID_EBMLMaxSizeLength  = 0x42F3,
    ID_DocType            = 0x4282,
    ID_DocTypeVersion     = 0x4287,
    ID_DocTypeReadVersion = 0x4285,
    ID_Void               = 0xEC,

    ID_Segment            = 0x18538067,
    ID_SeekHead           = 0x114D9B74,
    ID_Seek               = 0x4DBB,
    ID_SeekID             = 0x53AB,
    ID_SeekPosition       = 0x53AC,

    ID_Info               = 0x1549A966,
    ID_TimestampScale     = 0x2AD7B1,
    ID_Duration           = 0x4489,
    ID_MuxingApp          = 0x4D80,
    ID_WritingApp         = 0x5741,

    ID_Tracks             = 0x1654AE6B,
    ID_TrackEntry         = 0xAE,
    ID_TrackNumber        = 0xD7,
    ID_TrackUID           = 0x73C5,
    ID_TrackType          = 0x83,
    ID_FlagLacing         = 0x9C,
    ID_CodecID            = 0x86,
    ID_CodecPrivate       = 0x63A2,
    ID_DefaultDuration    = 0x23E383,
    ID_Video              = 0xE0,
    ID_PixelWidth         = 0xB0,
    ID_PixelHeight        = 0xBA,

    ID_Attachments        = 0x1941A469,
    ID_AttachedFile       = 0x61A7,
    ID_FileName           = 0x466E,
    ID_FileMediaType      = 0x4660,
    ID_FileData           = 0x465C,
    ID_FileUID            = 0x46AE,

    ID_Cluster            = 0x1F43B675,
    ID_Timestamp          = 0xE7,
    ID_SimpleBlock        = 0xA3,

    ID_Cues               = 0x1C53BB6B,
    ID_CuePoint           = 0xBB,
    ID_CueTime            = 0xB3,
    ID_CueTrackPositions  = 0xB7,
    ID_CueTrack           = 0xF7,
    ID_CueClusterPosition = 0xF1,
};

constexpr uint64_t TIMESTAMP_SCALE_NS = 1000000;  // 1 tick = 1 ms
constexpr int      TRACK_NUMBER       = 1;
constexpr uint64_t TRACK_UID          = 1;       // range: not 0
constexpr int64_t  CLUSTER_SPAN_TICKS = 1000;    // new Cluster every ~1 s
constexpr int      TRACK_TYPE_VIDEO   = 1;       // RFC 9559 Table 2

using Buf = std::vector<uint8_t>;

// Element ID: IDs already contain the VINT marker; write the significant
// octets big-endian.
void put_id(Buf& b, uint32_t id) {
    int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
    for (int i = n - 1; i >= 0; --i) b.push_back(uint8_t(id >> (8 * i)));
}

// Element Data Size as VINT (RFC 8794 4, 6.3). len = 0: minimal octets;
// value range for n octets is 0 .. 2^(7n) - 2 (all-ones = unknown size).
void put_size(Buf& b, uint64_t v, int len = 0) {
    if (len == 0) {
        len = 1;
        while (len < 8 && v > (uint64_t(1) << (7 * len)) - 2) ++len;
    }
    if (v > (uint64_t(1) << (7 * len)) - 2)
        throw std::length_error("MKV: element size does not fit VINT");
    const uint64_t vint = v | (uint64_t(1) << (7 * len));
    for (int i = len - 1; i >= 0; --i) b.push_back(uint8_t(vint >> (8 * i)));
}

void put_be(Buf& b, uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i)));
}

void put_uint(Buf& b, uint32_t id, uint64_t v) {
    int n = 1;
    while (n < 8 && (v >> (8 * n)) != 0) ++n;
    put_id(b, id);
    put_size(b, n);
    put_be(b, v, n);
}

// Unsigned integer stored on a fixed 8 octets (patched later).
// Returns offset of the payload within b.
size_t put_uint8_fixed(Buf& b, uint32_t id, uint64_t v) {
    put_id(b, id);
    put_size(b, 8);
    const size_t off = b.size();
    put_be(b, v, 8);
    return off;
}

uint64_t double_bits(double d) {
    uint64_t u;
    std::memcpy(&u, &d, sizeof u);
    return u;
}

// 8-octet float; returns offset of the payload within b.
size_t put_float(Buf& b, uint32_t id, double v) {
    put_id(b, id);
    put_size(b, 8);
    const size_t off = b.size();
    put_be(b, double_bits(v), 8);
    return off;
}

void put_string(Buf& b, uint32_t id, const std::string& s) {
    put_id(b, id);
    put_size(b, s.size());
    b.insert(b.end(), s.begin(), s.end());
}

void put_binary(Buf& b, uint32_t id, const uint8_t* p, size_t n) {
    put_id(b, id);
    put_size(b, n);
    b.insert(b.end(), p, p + n);
}

// Master element: ID + size + already-built payload.
// Returns offset of the payload within b.
size_t put_master(Buf& b, uint32_t id, const Buf& payload) {
    put_id(b, id);
    put_size(b, payload.size());
    const size_t off = b.size();
    b.insert(b.end(), payload.begin(), payload.end());
    return off;
}

Buf be_bytes(uint64_t v, int n) {
    Buf b;
    put_be(b, v, n);
    return b;
}

} // namespace

std::vector<uint8_t> vfw_codec_private(const char fourcc[4], int width,
                                       int height, uint16_t bit_count,
                                       const std::vector<uint8_t>& extradata) {
    std::vector<uint8_t> b;
    auto le32 = [&b](uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
    };
    auto le16 = [&b](uint16_t v) {
        b.push_back(uint8_t(v));
        b.push_back(uint8_t(v >> 8));
    };
    le32(uint32_t(40 + extradata.size()));                     // biSize
    le32(uint32_t(width));                                     // biWidth
    le32(uint32_t(height));                                    // biHeight
    le16(1);                                                   // biPlanes
    le16(bit_count);                                           // biBitCount
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(fourcc[i]));  // biCompression
    le32(uint32_t(uint64_t(width) * height * bit_count / 8));  // biSizeImage
    le32(0);                                                   // biXPelsPerMeter
    le32(0);                                                   // biYPelsPerMeter
    le32(0);                                                   // biClrUsed
    le32(0);                                                   // biClrImportant
    b.insert(b.end(), extradata.begin(), extradata.end());
    return b;
}

// ============================================================================
// File I/O
// ============================================================================

void Writer::write_bytes(const uint8_t* p, size_t n) {
    if (n && fwrite(p, 1, n, f_) != n)
        throw std::runtime_error("MKV writer: write failed");
}

void Writer::write_bytes(const Buf& b) { write_bytes(b.data(), b.size()); }

int64_t Writer::tell() {
#ifdef _WIN32
    return _ftelli64(f_);
#else
    return ftello(f_);
#endif
}

void Writer::seek(int64_t pos) {
#ifdef _WIN32
    const int r = _fseeki64(f_, pos, SEEK_SET);
#else
    const int r = fseeko(f_, off_t(pos), SEEK_SET);
#endif
    if (r != 0) throw std::runtime_error("MKV writer: seek failed");
}

// ============================================================================
// Public API
// ============================================================================

Writer::~Writer() {
    if (f_) fclose(f_);
}

void Writer::open(const std::string& path, const TrackInfo& track) {
    if (f_) throw std::logic_error("MKV writer: already open");
    if (track.width <= 0 || track.height <= 0 || !(track.fps > 0))
        throw std::invalid_argument("MKV writer: invalid track parameters");
    f_ = fopen(path.c_str(), "wb");
    if (!f_) throw std::runtime_error("MKV writer: cannot open " + path);
    track_ = track;
    path_  = path;
    // Header is written lazily so attachments can be added after open().
}

void Writer::add_attachment(const Attachment& att) {
    if (header_written_)
        throw std::logic_error("MKV writer: attachments must precede frames");
    attachments_.push_back(att);
}

void Writer::write_frame(const uint8_t* data, size_t size, bool keyframe) {
    if (!f_) throw std::logic_error("MKV writer: not open");
    if (!header_written_) write_header();

    const int64_t ts =
        std::llround(double(frame_index_) * 1000.0 / track_.fps);

    if (cluster_pos_ < 0 || ts - cluster_ts_ >= CLUSTER_SPAN_TICKS) {
        finish_cluster();
        start_cluster(ts);
        if (keyframe)
            cues_.push_back({ts, cluster_pos_ - segment_data_pos_});
    }

    // SimpleBlock (RFC 9559 10.2): TrackNumber VINT, int16 relative
    // timestamp, flags (KEY = 0x80, no lacing), frame data.
    Buf b;
    put_id(b, ID_SimpleBlock);
    put_size(b, 4 + uint64_t(size));
    put_size(b, TRACK_NUMBER);
    const int16_t rel = int16_t(ts - cluster_ts_);
    put_be(b, uint16_t(rel), 2);
    b.push_back(keyframe ? 0x80 : 0x00);
    write_bytes(b);
    write_bytes(data, size);

    ++frame_index_;
}

void Writer::close() {
    if (!f_) return;
    if (frame_index_ == 0) {
        // A Matroska file without any Cluster is valid per RFC 9559, but
        // FFmpeg's demuxer cannot open it; an empty archive is an error.
        fclose(f_);
        f_ = nullptr;
        std::remove(path_.c_str());
        throw std::runtime_error("MKV writer: no frames written");
    }
    finish_cluster();
    write_cues();

    const int64_t end = tell();

    // Segment size
    seek(segment_size_pos_);
    Buf s;
    put_size(s, uint64_t(end - segment_data_pos_), 8);
    write_bytes(s);

    // Duration (ticks)
    seek(duration_pos_);
    write_bytes(be_bytes(
        double_bits(double(frame_index_) * 1000.0 / track_.fps), 8));

    // SeekHead positions, relative to Segment data
    auto patch = [&](int64_t at, int64_t target) {
        if (at < 0) return;
        seek(at);
        write_bytes(be_bytes(uint64_t(target - segment_data_pos_), 8));
    };
    patch(seek_info_pos_,   info_pos_);
    patch(seek_tracks_pos_, tracks_pos_);
    patch(seek_attach_pos_, attach_pos_);
    if (cues_pos_ >= 0) {
        patch(seek_cues_pos_, cues_pos_);
    } else {
        // No Cues (first frame of every Cluster was not a keyframe):
        // overwrite the Cues Seek with Void
        // (RFC 8794 11.3.2), keeping the SeekHead size unchanged.
        seek(cues_seek_pos_);
        Buf v;
        put_id(v, ID_Void);
        put_size(v, cues_seek_len_ - 2, 1);
        v.resize(cues_seek_len_, 0);
        write_bytes(v);
    }

    const int rc = fclose(f_);
    f_ = nullptr;
    if (rc != 0) throw std::runtime_error("MKV writer: close failed");
}

// ============================================================================
// Header: EBML header, Segment start, SeekHead, Info, Tracks, Attachments
// ============================================================================

void Writer::write_header() {
    header_written_ = true;

    // EBML header (RFC 8794 11.2). SimpleBlock needs DocTypeVersion >= 2
    // (RFC 9559 section 9).
    Buf ebml;
    put_uint(ebml, ID_EBMLVersion, 1);
    put_uint(ebml, ID_EBMLReadVersion, 1);
    put_uint(ebml, ID_EBMLMaxIDLength, 4);
    put_uint(ebml, ID_EBMLMaxSizeLength, 8);
    put_string(ebml, ID_DocType, "matroska");
    put_uint(ebml, ID_DocTypeVersion, 4);
    put_uint(ebml, ID_DocTypeReadVersion, 2);
    Buf head;
    put_master(head, ID_EBML, ebml);

    // Segment with 8-octet size placeholder.
    put_id(head, ID_Segment);
    const size_t seg_size_off = head.size();
    put_size(head, 0, 8);
    const size_t seg_data_off = head.size();

    // SeekHead: one Seek per top-level element, SeekPosition fixed 8 octets.
    // Cues go last: if the file ends up without Cues, that Seek is replaced
    // by a Void element of the same size in close().
    Buf seekhead;
    std::vector<std::pair<uint32_t, int64_t*>> targets = {
        {ID_Info, &seek_info_pos_}, {ID_Tracks, &seek_tracks_pos_}};
    if (!attachments_.empty())
        targets.push_back({ID_Attachments, &seek_attach_pos_});
    targets.push_back({ID_Cues, &seek_cues_pos_});

    std::vector<std::pair<size_t, int64_t*>> seek_offs;   // in seekhead
    size_t cues_seek_off = 0;                             // in seekhead
    for (auto& t : targets) {
        Buf id_bytes;
        put_id(id_bytes, t.first);
        Buf seek;
        put_binary(seek, ID_SeekID, id_bytes.data(), id_bytes.size());
        const size_t pos_off = put_uint8_fixed(seek, ID_SeekPosition, 0);
        cues_seek_off = seekhead.size();
        const size_t payload_off = put_master(seekhead, ID_Seek, seek);
        seek_offs.push_back({payload_off + pos_off, t.second});
    }
    cues_seek_len_ = seekhead.size() - cues_seek_off;
    const size_t seekhead_payload = put_master(head, ID_SeekHead, seekhead);
    for (auto& s : seek_offs)
        *s.second = int64_t(seekhead_payload + s.first);
    cues_seek_pos_ = int64_t(seekhead_payload + cues_seek_off);

    // Info
    Buf info;
    put_uint(info, ID_TimestampScale, TIMESTAMP_SCALE_NS);
    const size_t dur_off = put_float(info, ID_Duration, 0.0);
    put_string(info, ID_MuxingApp, "AV1R");
    put_string(info, ID_WritingApp, "AV1R");
    const size_t info_off = head.size();
    const size_t info_payload = put_master(head, ID_Info, info);

    // Tracks
    Buf video;
    put_uint(video, ID_PixelWidth,  uint64_t(track_.width));
    put_uint(video, ID_PixelHeight, uint64_t(track_.height));
    Buf entry;
    put_uint(entry, ID_TrackNumber, TRACK_NUMBER);
    put_uint(entry, ID_TrackUID, TRACK_UID);
    put_uint(entry, ID_TrackType, TRACK_TYPE_VIDEO);
    put_uint(entry, ID_FlagLacing, 0);
    put_string(entry, ID_CodecID, track_.codec_id);
    if (!track_.codec_private.empty())
        put_binary(entry, ID_CodecPrivate, track_.codec_private.data(),
                   track_.codec_private.size());
    put_uint(entry, ID_DefaultDuration,
             uint64_t(std::llround(1e9 / track_.fps)));
    put_master(entry, ID_Video, video);
    Buf tracks;
    put_master(tracks, ID_TrackEntry, entry);
    const size_t tracks_off = head.size();
    put_master(head, ID_Tracks, tracks);

    // Attachments
    size_t attach_off = 0;
    if (!attachments_.empty()) {
        Buf atts;
        uint64_t uid = 1;
        for (const auto& a : attachments_) {
            Buf af;
            put_string(af, ID_FileName, a.name);
            put_string(af, ID_FileMediaType, a.mime);
            put_binary(af, ID_FileData, a.data.data(), a.data.size());
            put_uint(af, ID_FileUID, uid++);
            put_master(atts, ID_AttachedFile, af);
        }
        attach_off = head.size();
        put_master(head, ID_Attachments, atts);
    }

    const int64_t base = tell();
    write_bytes(head);

    segment_size_pos_ = base + int64_t(seg_size_off);
    segment_data_pos_ = base + int64_t(seg_data_off);
    duration_pos_     = base + int64_t(info_payload + dur_off);
    info_pos_         = base + int64_t(info_off);
    tracks_pos_       = base + int64_t(tracks_off);
    if (!attachments_.empty())
        attach_pos_   = base + int64_t(attach_off);
    for (auto& s : seek_offs)
        *s.second += base;
    cues_seek_pos_ += base;
}

// ============================================================================
// Clusters and Cues
// ============================================================================

void Writer::start_cluster(int64_t ts) {
    cluster_pos_ = tell();
    cluster_ts_  = ts;
    Buf b;
    put_id(b, ID_Cluster);
    cluster_size_pos_ = cluster_pos_ + int64_t(b.size());
    put_size(b, 0, 8);                       // patched in finish_cluster()
    put_uint(b, ID_Timestamp, uint64_t(ts));
    write_bytes(b);
}

void Writer::finish_cluster() {
    if (cluster_pos_ < 0) return;
    const int64_t end = tell();
    const int64_t data_start = cluster_size_pos_ + 8;
    seek(cluster_size_pos_);
    Buf s;
    put_size(s, uint64_t(end - data_start), 8);
    write_bytes(s);
    seek(end);
    cluster_pos_ = -1;
}

void Writer::write_cues() {
    if (cues_.empty()) return;
    Buf cues;
    for (const auto& c : cues_) {
        Buf tp;
        put_uint(tp, ID_CueTrack, TRACK_NUMBER);
        put_uint(tp, ID_CueClusterPosition, uint64_t(c.cluster_pos));
        Buf cp;
        put_uint(cp, ID_CueTime, uint64_t(c.time));
        put_master(cp, ID_CueTrackPositions, tp);
        put_master(cues, ID_CuePoint, cp);
    }
    cues_pos_ = tell();
    Buf b;
    put_master(b, ID_Cues, cues);
    write_bytes(b);
}

} // namespace mkv
} // namespace av1r
