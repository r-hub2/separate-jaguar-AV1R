# Lossless archive mode for video input: FFV1 v3 in MKV, luma as gray16

#' Archive a video losslessly as 16-bit grayscale (FFV1 in MKV)
#'
#' Decodes a video with FFmpeg and encodes its luma (Y) channel, converted
#' to 16-bit grayscale, to FFV1 version 3 (RFC 9043) in a Matroska
#' container. Frames are decoded and encoded on the fly in chunks, so videos
#' larger than RAM can be archived.
#'
#' The archive is lossless with respect to the 16-bit grayscale frames that
#' FFmpeg produces from the source (\code{-pix_fmt gray16le}): colour
#' (chroma) is not stored, and 8-bit luma is scaled to 16 bits by FFmpeg.
#' Every decoded frame is stored exactly once, at the source's nominal frame
#' rate; variable frame rate timing is not preserved.
#'
#' Use it for lossless or uncompressed sources (raw or uncompressed AVI,
#' scientific camera recordings). FFV1 keeps every pixel of the decoded
#' frames, including the noise and artefacts of a lossy codec, so for lossy
#' sources (MPEG-4, H.264, H.265, ...) the archive is much larger than the
#' original; keep the original file as the master copy instead.
#'
#' @param input  Path to a video file FFmpeg can decode.
#' @param output Path to the output \code{.mkv}. Defaults to \code{input}
#'   with the extension replaced by \code{.mkv} (\code{.ffv1.mkv} if the
#'   input is already \code{.mkv}).
#' @param slices Integer vector \code{c(horizontal, vertical)}: slice raster.
#'   Slices are coded independently (unit of parallelism).
#' @param context_model \code{0} (small context set, default) or \code{1}
#'   (large context set).
#' @param audio If \code{TRUE}, copy the source's audio streams into the
#'   archive unchanged (FFmpeg remux after encoding; one extra pass over the
#'   file). Default \code{FALSE}: video only.
#' @param verify If \code{TRUE}, decode the archive and the source with
#'   FFmpeg and compare the grayscale frames bit-exactly; stops with an
#'   error on any mismatch.
#' @param sidecar If \code{TRUE}, write \code{<output>.json} with codec
#'   parameters, versions and checksums.
#' @param verbose Print progress messages.
#' @param backend Encoder: \code{"auto"} (Vulkan GPU if available, else
#'   CPU), \code{"vulkan"} or \code{"cpu"}. Both produce byte-identical
#'   video streams.
#'
#' @return Invisibly, the path to the archive.
#'
#' @examples
#' \dontrun{
#' archive_video("recording.avi", verify = TRUE)
#' archive_video("recording.avi", audio = TRUE)   # keep the sound track
#' }
#' @export
archive_video <- function(input, output = NULL, slices = c(2L, 2L),
                          context_model = 0L, audio = FALSE, verify = FALSE,
                          sidecar = TRUE, verbose = TRUE,
                          backend = c("auto", "vulkan", "cpu")) {
  ffmpeg <- check_ffmpeg()
  if (!file.exists(input)) stop("File not found: ", input)
  if (is.null(output)) {
    output <- sub("\\.[^.]*$", ".mkv", input)
    if (normalizePath(output, mustWork = FALSE) ==
        normalizePath(input, mustWork = FALSE))
      output <- sub("\\.[^.]*$", ".ffv1.mkv", input)
  }
  if (normalizePath(output, mustWork = FALSE) ==
      normalizePath(input, mustWork = FALSE))
    stop("output must differ from input")
  slices <- as.integer(slices)
  stopifnot(length(slices) == 2L, all(slices >= 1L),
            context_model %in% c(0L, 1L), is.logical(audio), length(audio) == 1L)
  backend <- .ffv1_resolve_backend(match.arg(backend))

  info <- .video_stream_info(input)
  if (verbose) {
    message(sprintf(
      "AV1R [archive]: %s -> %s  [FFV1 v3, %dx%d, %.3f fps, gray16 from %s, slices %dx%d, %s]",
      basename(input), basename(output), info$width, info$height, info$fps,
      info$pix_fmt, slices[1], slices[2], .ffv1_backend_label(backend)))
    if (!grepl("^gray", info$pix_fmt))
      message("AV1R [archive]: colour source, only luma (Y) is archived")
  }

  decode_cmd <- paste(shQuote(ffmpeg), "-v error -i", shQuote(input),
                      .ffv1_gray16_pipe_args)
  frame_bytes <- 2 * as.numeric(info$width) * info$height
  chunk <- max(1L, as.integer(256 * 1024^2 / frame_bytes))   # ~256 MB

  # Sequential reader of the decoder pipe; counts the frames it delivers.
  con <- NULL
  n_frames <- 0
  get_chunk <- function(idx) {
    if (is.null(con)) con <<- pipe(decode_cmd, open = "rb")
    x <- readBin(con, "raw", n = length(idx) * frame_bytes)
    n_frames <<- n_frames + length(x) %/% frame_bytes
    x
  }
  on.exit(if (!is.null(con)) close(con))

  with_audio <- audio && info$has_audio
  if (audio && !info$has_audio && verbose)
    message("AV1R [archive]: no audio stream in the source, video only")
  video_out <- if (with_audio) paste0(output, ".video.mkv") else output

  .ffv1_encode_stream(get_chunk, NA, video_out, fps = info$fps,
                      slices = slices, context_model = context_model,
                      chunk = chunk, backend = backend,
                      width = info$width, height = info$height)
  close(con)
  con <- NULL

  if (with_audio) {
    if (verbose) message("AV1R [archive]: adding audio (stream copy) ...")
    .ffv1_mux_audio(video_out, input, output)
  }

  verified <- FALSE
  if (verify) {
    if (verbose) message("AV1R [archive]: verifying with ffmpeg ...")
    n <- .ffv1_verify_pipes(
      paste(shQuote(ffmpeg), "-v error -i", shQuote(output), .ffv1_gray16_pipe_args),
      decode_cmd, info$width, info$height, chunk)
    if (n != n_frames)
      stop(sprintf("FFV1 verify: %.0f frames decoded, %.0f encoded", n, n_frames))
    verified <- TRUE
    if (verbose) message("AV1R [archive]: verified, bit-exact.")
  }

  if (sidecar)
    .archive_write_sidecar(
      output, input,
      list(width = info$width, height = info$height, n_frames = n_frames),
      info$fps, slices, context_model, verified, NULL, backend,
      extra = list(source_pix_fmt = info$pix_fmt,
                   gray16_conversion = "ffmpeg -pix_fmt gray16le (luma only)",
                   audio = if (with_audio) "copied" else "none"))

  if (verbose) {
    in_mb  <- file.info(input)$size  / 1024^2
    out_mb <- file.info(output)$size / 1024^2
    message(sprintf("AV1R [archive]: %.0f frames, %.1f MB -> %.1f MB. done.",
                    n_frames, in_mb, out_mb))
  }
  invisible(output)
}

# Width, height, exact frame rate, pixel format of the first video stream;
# whether the file has an audio stream.
.video_stream_info <- function(input) {
  ffprobe <- Sys.which("ffprobe")
  if (!nzchar(ffprobe)) stop("ffprobe not found (it comes with ffmpeg)")
  probe <- function(args)
    suppressWarnings(system2(ffprobe, c("-v", "error", args, shQuote(input)),
                             stdout = TRUE, stderr = FALSE))
  v <- probe(c("-select_streams", "v:0", "-show_entries",
               "stream=width,height,r_frame_rate,pix_fmt",
               "-of", "default=noprint_wrappers=1"))
  field <- function(k) sub(paste0("^", k, "="), "",
                           grep(paste0("^", k, "="), v, value = TRUE))[1L]
  width  <- as.integer(field("width"))
  height <- as.integer(field("height"))
  if (is.na(width) || is.na(height))
    stop("No video stream found in: ", input)
  rate <- as.numeric(strsplit(field("r_frame_rate"), "/", fixed = TRUE)[[1L]])
  fps <- if (length(rate) == 2L && all(is.finite(rate)) && rate[2L] > 0)
    rate[1L] / rate[2L] else NA_real_
  if (!is.finite(fps) || fps <= 0) stop("Unknown frame rate: ", input)
  a <- probe(c("-select_streams", "a", "-show_entries", "stream=index",
               "-of", "csv=p=0"))
  list(width = width, height = height, fps = fps, pix_fmt = field("pix_fmt"),
       has_audio = length(a) > 0L)
}

# Remux: video from `video` (FFV1 bitstream unchanged), all audio streams
# of `source` (stream copy) -> `output`. Removes `video` on success.
.ffv1_mux_audio <- function(video, source, output) {
  stderr_file <- tempfile("av1r_mux_")
  on.exit(unlink(stderr_file), add = TRUE)
  status <- system2(check_ffmpeg(),
                    c("-v", "error", "-y", "-i", shQuote(video),
                      "-i", shQuote(source), "-map", "0:v:0", "-map", "1:a",
                      "-c", "copy", shQuote(output)),
                    stderr = stderr_file)
  if (!identical(status, 0L)) {
    unlink(output)
    stop("ffmpeg audio mux failed (status ", status, "):\n",
         paste(readLines(stderr_file, warn = FALSE), collapse = "\n"),
         "\nThe video-only archive is kept: ", video)
  }
  unlink(video)
  invisible(output)
}
