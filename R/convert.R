#' Convert video to AV1
#'
#' Converts biological microscopy video files (MP4/H.264, H.265, AVI/MJPEG)
#' or TIFF stacks to AV1 format. Automatically selects the best available
#' backend: Vulkan GPU (\code{VK_KHR_VIDEO_ENCODE_AV1}) when a compatible
#' device is found, otherwise CPU via FFmpeg (\code{libsvtav1} or
#' \code{libaom-av1}).
#'
#' @param input  Path to input file. Supported: .mp4, .avi, .mkv, .mov,
#'   .tif/.tiff (multi-page), or printf pattern like \code{"frame\%04d.tif"}.
#' @param output Path to output file (.mp4 or .mkv).
#' @param options An \code{av1r_options} list. Defaults to \code{av1r_options()}.
#'   Use \code{backend = "cpu"} or \code{backend = "vulkan"} to force a backend.
#'
#' @return Invisibly returns 0L on success. Stops with an error on failure.
#'
#' @examples
#' \dontrun{
#' # Auto-detect backend (GPU if available, else CPU)
#' convert_to_av1("recording.mp4", "recording_av1.mp4")
#'
#' # Force CPU encoding
#' convert_to_av1("stack.tif", "stack.mp4", av1r_options(crf = 20, backend = "cpu"))
#'
#' # Force GPU encoding (requires Vulkan AV1 support)
#' convert_to_av1("recording.mp4", "out.mp4", av1r_options(backend = "vulkan"))
#' }
#' @export
convert_to_av1 <- function(input, output, options = av1r_options()) {
  if (!file.exists(input)) stop("Input file not found: ", input)
  check_ffmpeg()

  bk <- if (options$backend == "auto") detect_backend() else options$backend

  if (bk == "vulkan") {
    # GPU path: ffmpeg decode to NV12 pipe -> Vulkan AV1 encode -> IVF -> MP4
    message("AV1R [gpu/vulkan]: Vulkan AV1 encode")
    info <- .ffmpeg_video_info(input)
    .Call("R_av1r_vulkan_encode",
          input, output,
          info$width, info$height, info$fps, options$crf,
          PACKAGE = "AV1R")
    message("AV1R: done.")
    return(invisible(0L))
  }

  if (bk == "vaapi") {
    message("AV1R [gpu/vaapi]: VAAPI AV1 encode")
    ret <- .vaapi_encode_av1(input, output, options)
    message("AV1R: done.")
    return(invisible(ret))
  }

  # CPU path: ffmpeg binary
  .ffmpeg_encode_av1(input, output, options)
}

# Internal: call ffmpeg via system2()
.ffmpeg_encode_av1 <- function(input, output, options) {
  ffmpeg <- Sys.which("ffmpeg")

  is_tiff <- grepl("\\.tiff?$", input, ignore.case = TRUE)

  # For TIFF stacks ffmpeg reads via image2 demuxer
  input_args <- if (is_tiff) {
    c("-framerate", "25", "-i", input)
  } else {
    c("-i", input)
  }

  # Pick AV1 encoder: prefer libsvtav1, fallback to libaom-av1
  encoder <- .pick_av1_encoder()

  encode_args <- c(
    "-c:v", encoder,
    "-crf", as.character(options$crf),
    "-preset", as.character(options$preset)
  )

  if (options$threads > 0L) {
    encode_args <- c(encode_args, "-threads", as.character(options$threads))
  }

  args <- c(
    "-y",
    input_args,
    encode_args,
    "-an",   # no audio (microscopy video is silent)
    output
  )

  message(sprintf(
    "AV1R [cpu]: ffmpeg %s -> %s  [encoder=%s crf=%d preset=%d]",
    basename(input), basename(output), encoder, options$crf, options$preset
  ))

  ret <- system2(ffmpeg, args)

  if (ret != 0L) {
    stop("ffmpeg failed with exit code ", ret,
         "\nCommand: ffmpeg ", paste(args, collapse = " "))
  }

  message("AV1R: done.")
  invisible(ret)
}

# Internal: VAAPI AV1 encode via ffmpeg av1_vaapi
.vaapi_encode_av1 <- function(input, output, options) {
  ffmpeg <- Sys.which("ffmpeg")
  if (nchar(ffmpeg) == 0) stop("ffmpeg not found")

  is_tiff <- grepl("\\.tiff?$", input, ignore.case = TRUE)
  input_args <- if (is_tiff) c("-framerate", "25", "-i", input) else c("-i", input)

  audio_args <- if (is_tiff) c("-an") else c("-c:a", "copy")

  # Rate control priority: explicit bitrate > auto-detect from input
  # Note: RADV (Mesa) does not implement CQP for AV1 — only VBR via -b:v works
  if (!is.null(options$bitrate)) {
    target_bps <- as.integer(options$bitrate) * 1000L
    rate_args  <- c("-b:v", as.character(target_bps))
    rate_label <- sprintf("bitrate=%dk (manual)", options$bitrate)
  } else {
    input_bitrate <- .ffmpeg_video_bitrate(input)
    if (!is.na(input_bitrate) && input_bitrate > 0) {
      # AV1 is ~45% more efficient than H.264 -> target 55% of input bitrate
      target_bps <- as.integer(input_bitrate * 0.55)
      rate_args  <- c("-b:v", as.character(target_bps))
      rate_label <- sprintf("bitrate=%dk (auto, 55%% of input)", target_bps %/% 1000L)
    } else {
      # Fallback for TIFF stacks: fixed 4 Mbps
      rate_args  <- c("-b:v", "4000000")
      rate_label <- "bitrate=4000k (default)"
    }
  }

  args <- c(
    "-y",
    "-vaapi_device", "/dev/dri/renderD128",
    input_args,
    "-vf", "format=nv12,hwupload",
    "-c:v", "av1_vaapi",
    rate_args,
    audio_args,
    output
  )

  message(sprintf(
    "AV1R [vaapi]: ffmpeg %s -> %s  [%s]",
    basename(input), basename(output), rate_label
  ))

  ret <- system2(ffmpeg, args)
  if (ret != 0L)
    stop("ffmpeg vaapi failed with exit code ", ret,
         "\nCommand: ffmpeg ", paste(args, collapse = " "))
  invisible(ret)
}

# Internal: get video stream bitrate in bps (NA if unavailable)
.ffmpeg_video_bitrate <- function(input) {
  ffprobe <- Sys.which("ffprobe")
  if (nchar(ffprobe) == 0) return(NA_integer_)

  lines <- tryCatch(
    suppressWarnings(
      system2(ffprobe,
              c("-v", "quiet", "-select_streams", "v:0",
                "-show_entries", "stream=bit_rate",
                "-of", "default=noprint_wrappers=1", input),
              stdout = TRUE, stderr = FALSE)
    ),
    error = function(e) character(0)
  )

  val <- sub("bit_rate=", "", grep("^bit_rate=", lines, value = TRUE))
  bps <- suppressWarnings(as.integer(val))
  if (length(bps) == 0 || is.na(bps) || bps <= 0) NA_integer_ else bps
}

# Internal: get video width/height/fps via ffprobe
.ffmpeg_video_info <- function(input) {
  ffprobe <- Sys.which("ffprobe")
  if (nchar(ffprobe) == 0) ffprobe <- Sys.which("ffmpeg")
  if (nchar(ffprobe) == 0) stop("ffprobe/ffmpeg not found")

  lines <- tryCatch(
    suppressWarnings(
      system2(ffprobe,
              c("-v", "quiet", "-select_streams", "v:0",
                "-show_entries", "stream=width,height,r_frame_rate",
                "-of", "default=noprint_wrappers=1", input),
              stdout = TRUE, stderr = FALSE)
    ),
    error = function(e) character(0)
  )

  width  <- as.integer(sub("width=",  "", grep("^width=",  lines, value = TRUE)))
  height <- as.integer(sub("height=", "", grep("^height=", lines, value = TRUE)))
  fps_str <- sub("r_frame_rate=", "", grep("^r_frame_rate=", lines, value = TRUE))

  fps <- if (length(fps_str) > 0 && grepl("/", fps_str)) {
    parts <- strsplit(fps_str, "/")[[1]]
    as.integer(round(as.numeric(parts[1]) / as.numeric(parts[2])))
  } else {
    25L
  }

  if (length(width) == 0 || length(height) == 0)
    stop("Could not read video dimensions from: ", input)

  list(width = width, height = height, fps = fps)
}

# Pick the best available AV1 encoder in the installed ffmpeg
.pick_av1_encoder <- function() {
  ffmpeg <- Sys.which("ffmpeg")
  encoders <- tryCatch(
    system2(ffmpeg, c("-encoders", "-v", "quiet"), stdout = TRUE, stderr = FALSE),
    error = function(e) character(0)
  )
  if (any(grepl("libsvtav1", encoders)))  return("libsvtav1")
  if (any(grepl("libaom-av1", encoders))) return("libaom-av1")
  stop("No AV1 encoder found in ffmpeg (need libsvtav1 or libaom-av1).\n",
       "  Install: sudo apt install ffmpeg  (Ubuntu 22.04+ includes libsvtav1)")
}
