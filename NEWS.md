# AV1R 0.1.2

## Minimum coded extent handling

* GPU encoder now queries `minCodedExtent` from the Vulkan driver before
  opening the ffmpeg decode pipe. Input frames smaller than the hardware
  minimum (e.g. 170×196 vs driver minimum 320×128) are automatically
  scaled up to the minimum supported resolution.
* Vulkan context is initialized before the ffmpeg pipe so that the
  corrected dimensions are used for both decoding and encoding.
* Fixed segfault in `av1r_vulkan_query_min_extent()`: the
  `VkVideoEncodeAV1CapabilitiesKHR` structure was missing from the pNext
  chain passed to `vkGetPhysicalDeviceVideoCapabilitiesKHR`, causing
  RADV to write to an unmapped address.

## CRF / QIndex mapping

* CRF-to-QIndex mapping remains linear (`qIndex = crf * 4`, range 0–255).
  Users should choose CRF appropriate to their content: low CRF (1–10) for
  16-bit grayscale microscopy TIFF stacks, higher CRF (20–35) for
  conventional video recordings.

# AV1R 0.1.1

## Vulkan AV1 GPU Encoding — fully working

* **GPU encoding now works end-to-end** on AMD RDNA4 (RX 9070, RADV GFX1201)
  via Vulkan `VK_KHR_VIDEO_ENCODE_AV1`.
* Two-queue architecture: transfer queue (buffer→image upload) + encode queue
  (video encoding) with semaphore synchronization. Fixes `VK_ERROR_DEVICE_LOST`
  on GPUs where the encode queue has no TRANSFER capability.
* Correct NV12 upload: Y plane and UV plane copied as separate
  `vkCmdCopyBufferToImage` calls with proper `bufferRowLength` for each plane.
* Sequence Header OBU automatically prepended via
  `vkGetEncodedVideoSessionParametersKHR` — output plays in all players.
* Rate control: DISABLED mode (CQP) with `constantQIndex` derived from CRF
  (`qIndex = crf * 4`). VBR/CBR modes are reported as supported by RADV but
  have no effect; CQP is the only working mode.
* Audio track from the original file is preserved during IVF→MP4 muxing.
* Dynamic loading of all Vulkan Video KHR extension functions via
  `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` for compatibility with
  SDK < 1.3.290.

## Multi-page TIFF support

* Multi-page TIFF stacks are now extracted to a temporary PNG sequence via
  the `magick` package before encoding. Both GPU and CPU paths support this.
* `magick` added to `Suggests` (required only for multi-page TIFF input).
* Image sequences (printf patterns like `frame%06d.png`) are passed to ffmpeg
  with `-framerate` for correct decoding.
* Tested on 510-frame 16-bit grayscale confocal microscopy TIFF stack
  (32.7 MB → 3.8 MB at CRF 1, 8.6x compression with quality preservation).

## CRAN Fixes

* Removed redundant "A tool for" from Description.
* Single-quoted software and API names in Title and Description per CRAN policy.
* Replaced `\dontrun{}` with `\donttest{}` in all examples.
* Examples now write to `tempdir()` instead of user's home filespace.

## Bundled Headers

* Bundled Khronos `VK_KHR_video_encode_av1` headers (`src/vk_video/`) — package
  no longer requires Vulkan SDK >= 1.3.290 at build time; builds against any
  Vulkan SDK >= 1.3.275 with the bundled polyfill.
* `configure` now detects bundled AV1 encode headers and enables GPU path
  automatically.

## Initial Release

* CPU encoding via FFmpeg (`libsvtav1` / `libaom-av1`).
* Automatic backend selection: GPU (Vulkan) → CPU (FFmpeg).
* Supports H.264, H.265, AVI/MJPEG, TIFF stacks, and TIFF sequences as input.
* `convert_to_av1()` for single-file and batch conversion.
* `detect_backend()`, `vulkan_available()`, `vulkan_devices()` for diagnostics.
