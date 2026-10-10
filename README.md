# AV1R

[![R-hub check on the R Consortium cluster](https://github.com/r-hub2/separate-jaguar-AV1R/actions/workflows/rhub-rc.yaml/badge.svg)](https://github.com/r-hub2/separate-jaguar-AV1R/actions/workflows/rhub-rc.yaml)

**AV1 video encoding for biological microscopy data.**

AV1R is an R package for biologists that converts legacy microscopy video formats (H.264/H.265, AVI/MJPEG, TIFF stacks) to the modern AV1 codec with minimal quality loss.

## Why AV1R?

- **Compress large TIFF stacks** from confocal microscopy, time-lapse, and EBImage workflows from hundreds of gigabytes to manageable sizes — without noticeable loss of information.
- **Re-encode MP4 files** exported from CellProfiler, ImageJ/Fiji, and microscope software with ~2x better compression at the same visual quality.
- **Standardise legacy recordings** — convert old AVI (MJPEG) and H.265 files to a single patent-free format suited for long-term archival.

## GPU acceleration

AV1R automatically selects the best available backend:

| Priority | Backend | How |
|----------|---------|-----|
| 1 | **VAAPI** | `av1_vaapi` via FFmpeg — best speed/quality (AMD/Intel) |
| 2 | **Vulkan** | `VK_KHR_VIDEO_ENCODE_AV1` — fastest, CQP only |
| 3 | **CPU (SVT-AV1)** | `libsvtav1` via FFmpeg |

### Tested hardware

| GPU | Driver | Status |
|-----|--------|--------|
| AMD Radeon RX 9070 (RDNA4) | Mesa RADV (GFX1201) | Working |

Vulkan AV1 encode headers are bundled in `src/vk_video/`, so no SDK upgrade is needed at build time. Builds with any Vulkan SDK >= 1.3.275. Runtime support depends on GPU driver.

### Benchmark (30 s clip, 1920x1080, CRF 28, AMD RX 9070)

| Backend | Time (s) | Size (MB) | Compression | SSIM | FPS | vs CPU |
|---------|----------|-----------|-------------|------|-----|--------|
| Vulkan  | 2.9      | 3.31      | 1.9x | 0.9339 | 245.9 | 4.0x |
| VAAPI   | 2.0      | 3.97      | 1.6x | 0.9609 | 363.3 | 5.9x |
| CPU (SVT-AV1) | 11.7 | 8.09  | 0.8x | 0.9645 | 61.6 | 1.0x |

Compression = input clip size / encoded size. SSIM >= 0.97 visually transparent.
vs CPU = CPU time / backend time (higher = faster).

Reproduce: `Rscript inst/examples/benchmark_backends.R`

## Quick Start

```r
library(AV1R)

# Convert microscopy recording
convert_to_av1("recording.mp4", "recording_av1.mp4")

# TIFF stack with custom settings
convert_to_av1("stack.tif", "stack.mp4", av1r_options(crf = 20))

# Batch convert entire experiment folder
convert_folder("experiment/", "compressed/")

# Lossless archive (FFV1 in MKV): 16-bit TIFF stack, or a video as gray16
archive_tiff("stack.tif")
archive_video("recording.avi", audio = TRUE)

# Check what backend will be used
detect_backend()
```

## GPU encoding

```r
# Check GPU availability
vulkan_available()
vulkan_devices()

# Force GPU backend
convert_to_av1("input.mp4", "output.mp4", av1r_options(backend = "vulkan"))

# Force CPU backend
convert_to_av1("input.mp4", "output.mp4", av1r_options(backend = "cpu"))
```

Each backend uses the quality control approach best suited to its rate control:

| Backend | Video input | TIFF/image input |
|---------|-------------|------------------|
| CPU (SVT-AV1) | CRF directly | CRF directly |
| Vulkan | CRF directly (CQP) | CRF directly (CQP) |
| VAAPI | VBR at 55% of input bitrate | CQP (CRF mapped to QP 0–255) |

Frames smaller than the hardware minimum coded extent are automatically
scaled up proportionally (no distortion). Audio from the original file
is preserved automatically.

### CRF equivalence across backends (TIFF, mitosis.tif, 510 frames)

CRF is a unified quality parameter (0 = best, 63 = worst). Since each backend
uses a different internal quantizer scale, AV1R maps CRF automatically.
The table below shows equivalent settings calibrated by file size:

| CRF | CPU (KB) | VAAPI QP | VAAPI (KB) | Vulkan CRF | Vulkan (KB) |
|----:|--------:|---------:|----------:|-----------:|----------:|
|   1 |  3815.5 |        8 |    3800.8 |          2 |    3369.2 |
|   3 |  2766.5 |       11 |    2822.4 |          2 |    3369.2 |
|   5 |  1983.7 |       14 |    1982.1 |          3 |    2064.3 |
|   8 |   924.4 |       24 |     952.6 |          5 |     955.0 |
|  10 |   578.7 |       31 |     597.1 |          7 |     548.9 |
|  15 |   264.4 |       50 |     278.7 |         11 |     260.5 |
|  20 |   162.1 |       75 |     156.5 |         15 |     163.8 |
|  30 |    84.4 |      110 |      82.0 |         30 |      61.9 |
|  40 |    53.2 |      150 |      53.3 |         40 |      46.9 |
|  50 |    38.8 |      190 |      38.8 |         50 |      37.6 |
|  63 |    25.4 |      255 |      29.5 |         63 |      30.5 |

Reproduce: `Rscript inst/examples/calibrate_vaapi_cqp.R`

**Note:** Vulkan uses CQP (constant quantizer) rate control — the only mode
currently working on RADV. CQP does not adapt to scene complexity, so Vulkan
produces slightly lower SSIM (~0.93) compared to CPU/VAAPI (~0.96). This is
a driver limitation; VBR/CRF support will improve quality when RADV implements
it.

## Supported Input Formats

| Format | Extension |
|--------|-----------|
| H.264 / MP4 | `.mp4`, `.mkv`, `.mov` |
| H.265 / HEVC | `.mp4`, `.mkv` |
| AVI / MJPEG | `.avi` |
| FLV | `.flv` |
| MPEG-1/2 | `.mpg`, `.mpeg` |
| WebM (VP8/VP9) | `.webm` |
| TIFF stack | `.tif`, `.tiff` |
| TIFF sequence | `frame%04d.tif` |

## System Requirements

- **FFmpeg >= 4.4** with `libsvtav1` (required for CPU encoding); `ffmpeg`
  and `ffprobe` are also used by `archive_video()` and for `verify = TRUE`
- **libvulkan-dev** (optional, for GPU encoding on Linux)
- **R package `tiff`** (optional, for `archive_tiff()`)
- **glslc** (optional, build time: GPU encoding for `archive_tiff()` and
  `archive_video()`)

```bash
# Ubuntu 22.04+
sudo apt install ffmpeg libvulkan-dev
```

`glslc` (shaderc) is needed only at install time, for the GPU path of the
FFV1 archive. Install it from your distribution if packaged
(`sudo apt install glslc` on recent Ubuntu/Debian) or from the
[Vulkan SDK](https://vulkan.lunarg.com/sdk/home); `configure` also looks in
`$GLSLC` and `$VULKAN_SDK/bin`. Without it the CPU encoder is used.

## TIFF stacks

TIFF stacks use near-lossless CRF 5 by default (`tiff_crf`). Output resolution can be controlled via `tiff_scale`.

> **Note for quantitative microscopy:** AV1 is a lossy codec even at CRF 1.
> If your workflow relies on pixel intensity measurements (fluorescence
> quantification, ratiometric imaging, colocalization analysis), lossy
> compression will alter the data. For such use cases, keep the original
> TIFF/PNG files for analysis and use AV1R only for visualization, sharing,
> and archival of preview copies. For lossless archiving use
> `archive_tiff()` (FFV1, see below).

```r
# 2x magnification (proportional, no distortion)
convert_to_av1("stack.tif", "stack.mp4", av1r_options(tiff_scale = 2))

# Fit into 1920x1080 bounding box (proportional)
convert_to_av1("stack.tif", "stack.mp4", av1r_options(tiff_scale = c(1920, 1080)))
```

See `inst/examples/tiff_scale.R` for more examples.

## Lossless archive (FFV1)

`archive_tiff()` stores 16-bit grayscale TIFF stacks losslessly as FFV1
version 3 ([RFC 9043](https://www.rfc-editor.org/rfc/rfc9043)) in Matroska.
Decoding gives back the original pixel values bit for bit, so the archive
can replace the TIFF for quantitative analysis. `archive_video()` does the
same for video files (see below).

```r
# stack.tif -> stack.mkv + stack.mkv.json
archive_tiff("stack.tif")

# Decode with FFmpeg and compare bit-exactly with the input
archive_tiff("stack.tif", "archive/stack.mkv", verify = TRUE)

# Force the encoder: "auto" (default, GPU if available), "vulkan" or "cpu"
archive_tiff("stack.tif", backend = "cpu")

# Slice raster and context model
archive_tiff("stack.tif", slices = c(4L, 4L), context_model = 0L)
```

### Video input

`archive_video()` decodes a video with FFmpeg and archives its luma (Y)
channel as 16-bit grayscale FFV1. Frames are decoded and encoded on the
fly in chunks, so long recordings need no more memory than one chunk.

**When to use it.** FFV1 is lossless: it keeps every pixel of the decoded
frames, including the noise and artefacts of a lossy codec. Use
`archive_video()` for lossless or uncompressed sources (raw or
uncompressed AVI, scientific camera recordings), where it saves space
without loss. For lossy sources (MPEG-4, H.264, H.265, ...) the archive
is much larger than the original — e.g. about 25x per frame for an MPEG-4
AVI — so keep the original file as the master copy instead.

```r
# recording.avi -> recording.mkv + recording.mkv.json (video only)
archive_video("recording.avi", verify = TRUE)

# Keep the sound track: audio streams are copied unchanged
archive_video("recording.avi", audio = TRUE)
```

- Lossless with respect to the frames FFmpeg decodes as `gray16le`:
  colour (chroma) is not stored, and 8-bit luma is scaled to 16 bits by
  FFmpeg. A colour source is reported in the messages.
- Every decoded frame is stored exactly once (`-vsync passthrough`), at
  the source's exact nominal frame rate (e.g. 24000/1001). Variable frame
  rate timing is not preserved.
- `audio = FALSE` (default) writes the video only. With `audio = TRUE` the
  audio streams are added after encoding by an FFmpeg remux (stream copy,
  one extra pass over the file); the FFV1 video stays byte-identical.
- `verify = TRUE` decodes the archive and the source again and compares
  the grayscale frames chunk by chunk.

### Compatibility with FFmpeg

AV1R has its own FFV1 encoder and Matroska writer: for `archive_tiff()`
FFmpeg is needed only for `verify = TRUE` and to decode the archive later.
The FFV1 bitstream is byte-identical to FFmpeg's encoder (checked packet by
packet in the tests, FFmpeg 6.1) for

```bash
ffmpeg -f rawvideo -pix_fmt gray16le -s WxH -i stack.raw \
       -c:v ffv1 -level 3 -coder 1 -context 0 -slices 4 -slicecrc 1 -g 1 stack.mkv
```

with `-slices` = `slices[1] * slices[2]` and `-context` = `context_model`.
The track is stored as FFmpeg stores FFV1 in Matroska (CodecID
`V_MS/VFW/FOURCC`), so archives play in FFmpeg-based tools, VLC,
GStreamer and mpv / Celluloid.
Every frame is a keyframe and every slice carries a CRC, so a damaged frame
does not affect the others. To read the samples back:

```bash
ffmpeg -i stack.mkv -map 0:v:0 -vsync passthrough -f rawvideo -pix_fmt gray16le stack.raw
```

### GPU encoding

With `backend = "vulkan"` (or `"auto"` when a GPU is found) slices are
encoded by Vulkan compute shaders. GPU and CPU produce byte-identical
archives. The GPU path is built when Vulkan headers and `glslc` are found
at install time; the device used is recorded in the sidecar
(`encoder_backend`).

- One GPU thread codes one slice of one frame (the range coder is serial
  within a slice); frames go to the GPU in batches of up to 256 (about
  1 GiB of buffers), so a batch keeps thousands of slice coders busy.
- A batch is coded as a series of short dispatches of about 8 ms each,
  a few slice rows at a time, so the desktop stays responsive while the
  GPU encodes.
- All buffers the shader uses are in device-local memory; frames are
  uploaded and results read back through staging buffers.

Set `AV1R_FFV1_PROFILE=1` to print the memory types of the GPU buffers and
the time per stage (conversion, upload, dispatch, gather, mux) to stderr
when an archive is closed. `inst/examples/profile_ffv1_shader.R` times the
shader alone for several slice rasters and batch sizes.

### Choosing slices and context model

Slices are coded independently: more slices give more parallelism but
worse compression, especially on small frames, because every slice starts
with untrained contexts. `context_model = 1` adapts slowly and pays off
only on large frames. Compression ratio on a small-frame stack (510 frames
171x196, `context_model = 0`):

| slices | ratio |
|--------|-------|
| 2x2    | 1.35  |
| 4x4    | 1.22  |
| 8x8    | 1.08  |

Compression is identical to FFmpeg's. Since the GPU gets its parallelism
from batches of frames, the default `c(2L, 2L)` is fast on the GPU too.

### Speed

`inst/examples/benchmark_ffv1.R`, slices 2x2, `context_model = 0`, AMD
RX 9070, FFmpeg 6.1.1, MB/s of 16-bit input, frames in memory.

| input | AV1R vulkan | AV1R cpu | ffmpeg `-threads 1` | ffmpeg |
|-------|-------------|----------|---------------------|--------|
| mitosis.tif, 171x196, 510 frames  | 67.3 | 20.0 | 28.3 | 80.1  |

FFmpeg encodes slices on all CPU cores; the AV1R CPU encoder uses one.

### Output files

- `<output>.mkv` — FFV1 video track (plus the copied audio streams with
  `archive_video(audio = TRUE)`); OME-XML from the TIFF ImageDescription is
  attached as `metadata.ome.xml`.
- `<output>.mkv.json` (`sidecar = TRUE`) — codec parameters, frame size
  and count, MD5 of the source and the archive, verification result and
  FFmpeg version, encoder backend, AV1R and R versions, OME-XML; for video
  also the source pixel format, the gray16 conversion and whether audio was
  copied.

### Limitations

- 16-bit grayscale only (RGB48 and 8-bit are planned); video is archived
  as luma only.
- `archive_tiff()` requires the `tiff` package; `archive_video()` requires
  `ffmpeg` and `ffprobe`. Both work in chunks, so inputs larger than RAM
  work.

## Options

```r
av1r_options(
  crf        = 28,    # quality: 0 (best) - 63 (worst)
  preset     = 8,     # speed: 0 (slow/best) - 13 (fast/worst), CPU only
  threads    = 0,     # 0 = auto, CPU only
  tiff_crf   = 5,     # CRF for TIFF input (near-lossless)
  tiff_scale = NULL,  # NULL, multiplier (e.g. 2), or c(width, height)
  verbose    = TRUE,  # informational messages
  backend    = "auto" # "auto", "cpu", "vaapi", or "vulkan"
)
```

## License

MIT

---

[GitHub](https://github.com/Zabis13/AV1R) · [Issues](https://github.com/Zabis13/AV1R/issues)
