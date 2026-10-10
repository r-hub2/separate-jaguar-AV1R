// Minimal Matroska (MKV) writer for a single FFV1 video track.
//
// Layout: EBML header, Segment { SeekHead, Info, Tracks, Attachments,
// Cluster..., Cues }. Element IDs and encoding rules: RFC 8794 (EBML),
// RFC 9559 (Matroska); FFV1 mapping: RFC 9043 4.3.3.4.
//
// Output is deterministic (fixed UIDs, no dates), so identical input gives
// a byte-identical file.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace av1r {
namespace mkv {

struct TrackInfo {
    int width  = 0;
    int height = 0;
    double fps = 25.0;
    std::string codec_id = "V_FFV1";
    std::vector<uint8_t> codec_private;   // as CodecID requires
};

// CodecPrivate for CodecID "V_MS/VFW/FOURCC": a 40-byte little-endian
// BITMAPINFOHEADER (biSize = 40 + extradata size, biPlanes 1, biSizeImage =
// width * height * bit_count / 8, other fields 0) followed by the codec
// extradata. This is how FFmpeg's Matroska muxer stores FFV1; players that
// lack the native "V_FFV1" mapping (mpv, Celluloid) accept it.
std::vector<uint8_t> vfw_codec_private(const char fourcc[4], int width,
                                       int height, uint16_t bit_count,
                                       const std::vector<uint8_t>& extradata);

struct Attachment {
    std::string name;       // e.g. "metadata.ome.xml"
    std::string mime;       // e.g. "application/xml"
    std::vector<uint8_t> data;
};

class Writer {
public:
    Writer() = default;
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void open(const std::string& path, const TrackInfo& track);
    void add_attachment(const Attachment& att);   // before first frame
    void write_frame(const uint8_t* data, size_t size, bool keyframe);
    void close();

private:
    struct CuePoint { int64_t time; int64_t cluster_pos; };

    void write_header();
    void start_cluster(int64_t ts);
    void finish_cluster();
    void write_cues();

    void    write_bytes(const std::vector<uint8_t>& b);
    void    write_bytes(const uint8_t* p, size_t n);
    int64_t tell();
    void    seek(int64_t pos);

    FILE* f_ = nullptr;
    std::string path_;
    TrackInfo track_;
    std::vector<Attachment> attachments_;
    bool header_written_ = false;
    int64_t frame_index_ = 0;

    // File positions (absolute) for patching at close().
    int64_t segment_size_pos_ = -1;   // 8-byte size of Segment
    int64_t segment_data_pos_ = -1;   // start of Segment data
    int64_t duration_pos_     = -1;   // 8-byte float payload of Duration
    // SeekPosition payload offsets (absolute) and the targets they point to.
    int64_t seek_info_pos_ = -1, seek_tracks_pos_ = -1;
    int64_t seek_attach_pos_ = -1, seek_cues_pos_ = -1;
    int64_t info_pos_ = -1, tracks_pos_ = -1, attach_pos_ = -1, cues_pos_ = -1;
    // Cues Seek element (absolute position, total length) for Void fallback.
    int64_t cues_seek_pos_ = -1;
    size_t  cues_seek_len_ = 0;

    // Current cluster.
    int64_t cluster_pos_      = -1;   // absolute position of Cluster ID
    int64_t cluster_size_pos_ = -1;   // 8-byte size of Cluster
    int64_t cluster_ts_       = 0;

    std::vector<CuePoint> cues_;
};

} // namespace mkv
} // namespace av1r
