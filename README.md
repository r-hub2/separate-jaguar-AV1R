# AV1R

AV1 video encoding for biological microscopy data.

Converts MP4/H.264, H.265/HEVC, AVI/MJPEG, and TIFF stacks to AV1 for significantly improved compression without quality loss.

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
