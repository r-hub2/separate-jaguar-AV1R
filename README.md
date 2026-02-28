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
| 1 | **Vulkan** | `VK_KHR_VIDEO_ENCODE_AV1` — native GPU encode |
| 2 | **CPU** | `libsvtav1` or `libaom-av1` via FFmpeg |

### Tested hardware

| GPU | Driver | Status |
|-----|--------|--------|
| AMD Radeon RX 9070 (RDNA4) | Mesa RADV (GFX1201) | Working |

Vulkan AV1 encode headers are bundled in `src/vk_video/`, so no SDK upgrade is needed at build time. Builds with any Vulkan SDK >= 1.3.275. Runtime support depends on GPU driver.

## Quick Start

```r
library(AV1R)

# Convert microscopy recording
convert_to_av1("recording.mp4", "recording_av1.mp4")

# TIFF stack with custom settings
convert_to_av1("stack.tif", "stack.mp4", av1r_options(crf = 20))

# Batch convert entire experiment folder
convert_folder("experiment/", "compressed/")

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

GPU encoding uses CQP (constant quantizer) rate control. The `crf` parameter
maps directly to AV1 quantizer index (`qIndex = crf * 4`, range 0–255).
Choose CRF appropriate to your content: low values (1–10) for 16-bit
grayscale microscopy TIFF stacks, higher values (20–35) for conventional
video. Frames smaller than the hardware minimum coded extent are
automatically scaled up. Audio from the original file is preserved
automatically.

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
  preset  = 8,     # speed: 0 (slow/best) - 13 (fast/worst), CPU only
  threads = 0,     # 0 = auto, CPU only
  backend = "auto" # "auto", "cpu", or "vulkan"
)
```

## License

MIT

---

[GitHub](https://github.com/Zabis13/AV1R) · [Issues](https://github.com/Zabis13/AV1R/issues)
