# AV1R

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
| 1 | **Vulkan** | `VK_KHR_VIDEO_ENCODE_AV1` — native GPU encode (coming soon in Mesa/RADV) |
| 2 | **VAAPI** | `av1_vaapi` via FFmpeg — works now on AMD/Intel GPUs |
| 3 | **CPU** | `libsvtav1` or `libaom-av1` via FFmpeg — always available |

## Quick Start

```r
library(AV1R)

# Convert microscopy recording
convert_to_av1("recording.mp4", "recording_av1.mp4")

# TIFF stack with custom settings
convert_to_av1("stack.tif", "stack.mp4", av1r_options(crf = 20))

# Check what backend will be used
detect_backend()
```

## Backends

| Backend | How | When used |
|---------|-----|-----------|
| CPU | FFmpeg binary (`libsvtav1` or `libaom-av1`) | Always available |
| GPU | Vulkan `VK_KHR_VIDEO_ENCODE_AV1` | When a compatible GPU is present |

Auto-detection order: GPU (Vulkan) → CPU (FFmpeg).

```r
# Check GPU availability
vulkan_available()
vulkan_devices()

# Force CPU
convert_to_av1("input.mp4", "output.mp4", av1r_options(backend = "cpu"))
```

## Supported Input Formats

| Format | Extension |
|--------|-----------|
| H.264 / MP4 | `.mp4`, `.mkv`, `.mov` |
| H.265 / HEVC | `.mp4`, `.mkv` |
| AVI / MJPEG | `.avi` |
| TIFF stack | `.tif`, `.tiff` |
| TIFF sequence | `frame%04d.tif` |

## System Requirements

- **FFmpeg >= 4.4** with `libsvtav1` or `libaom-av1` (required for CPU encoding)
- **libvulkan-dev** (optional, for GPU encoding on Linux)

```bash
# Ubuntu 22.04+
sudo apt install ffmpeg libvulkan-dev
```

## Options

```r
av1r_options(
  crf     = 28,    # quality: 0 (best) - 63 (worst)
  preset  = 8,     # speed: 0 (slow/best) - 13 (fast/worst)
  threads = 0,     # 0 = auto
  backend = "auto" # "auto", "cpu", or "vulkan"
)
```

## License

MIT

---

[GitHub](https://github.com/Zabis13/AV1R) · [Issues](https://github.com/Zabis13/AV1R/issues)
