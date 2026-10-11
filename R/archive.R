# Lossless archive mode: FFV1 v3 in MKV (own encoder, ffmpeg for verification)

#' Archive a 16-bit TIFF stack losslessly (FFV1 in MKV)
#'
#' Encodes a 16-bit grayscale TIFF stack to FFV1 version 3 (RFC 9043) in a
#' Matroska container. Compression is lossless: decoding the archive gives
#' back the original sample values bit for bit. Frames are processed in
#' chunks, so stacks larger than RAM can be archived.
#'
#' OME-XML found in the TIFF ImageDescription tag is stored as an MKV
#' attachment (\code{metadata.ome.xml}) and in the sidecar file.
#'
#' @param input  Path to a multi-page 16-bit grayscale TIFF.
#' @param output Path to the output \code{.mkv}. Defaults to \code{input}
#'   with the extension replaced by \code{.mkv}.
#' @param fps    Frame rate written to the container.
#' @param slices Integer vector \code{c(horizontal, vertical)}: slice raster.
#'   Slices are coded independently (unit of parallelism).
#' @param context_model \code{0} (small context set, default) or \code{1}
#'   (large context set; slower adaptation, can compress better on large
#'   frames).
#' @param verify If \code{TRUE}, decode the archive with FFmpeg and compare
#'   it bit-exactly with the input; stops with an error on any mismatch.
#'   Requires FFmpeg.
#' @param sidecar If \code{TRUE}, write \code{<output>.json} with codec
#'   parameters, versions, checksums and OME-XML.
#' @param verbose Print progress messages.
#' @param backend Encoder: \code{"auto"} (Vulkan GPU if available, else
#'   CPU), \code{"vulkan"} or \code{"cpu"}. Both produce byte-identical
#'   archives; the GPU path needs a Vulkan compute device and AV1R built
#'   with \code{glslc}.
#'
#' @return Invisibly, the path to the archive.
#'
#' @examples
#' \dontrun{
#' # Requires the 'tiff' package
#' archive_tiff("stack.tif", verify = TRUE)
#' }
#' @export
archive_tiff <- function(input, output = NULL, fps = 25, slices = c(2L, 2L),
                         context_model = 0L, verify = FALSE, sidecar = TRUE,
                         verbose = TRUE,
                         backend = c("auto", "vulkan", "cpu")) {
  if (!requireNamespace("tiff", quietly = TRUE))
    stop("Package 'tiff' is required for archive_tiff().\n",
         "  Install: install.packages(\"tiff\")")
  if (!file.exists(input)) stop("File not found: ", input)
  if (is.null(output))
    output <- sub("\\.[^.]*$", ".mkv", input)
  if (normalizePath(output, mustWork = FALSE) ==
      normalizePath(input, mustWork = FALSE))
    stop("output must differ from input")
  slices <- as.integer(slices)
  stopifnot(length(slices) == 2L, all(slices >= 1L),
            context_model %in% c(0L, 1L), is.numeric(fps), fps > 0)
  backend <- .ffv1_resolve_backend(match.arg(backend))

  info <- .tiff_stack_info(input)
  if (verbose)
    message(sprintf(
      "AV1R [archive]: %s -> %s  [FFV1 v3, %dx%d, %d frames, gray16, slices %dx%d, %s]",
      basename(input), basename(output), info$width, info$height,
      info$n_frames, slices[1], slices[2], .ffv1_backend_label(backend)))

  chunk <- .archive_chunk_frames(info$width, info$height)
  get_chunk <- function(idx) .tiff_read_frames(input, idx)

  ome <- info$description
  if (!is.null(ome) && !grepl("<OME", ome, fixed = TRUE)) ome <- NULL

  .ffv1_encode_stream(get_chunk, info$n_frames, output, fps = fps,
                      slices = slices, context_model = context_model,
                      chunk = chunk,
                      ome_xml = ome, backend = backend)

  verified <- FALSE
  if (verify) {
    if (verbose) message("AV1R [archive]: verifying with ffmpeg ...")
    .ffv1_verify_stream(output, info$width, info$height, info$n_frames,
                        get_chunk, chunk)
    verified <- TRUE
    if (verbose) message("AV1R [archive]: verified, bit-exact.")
  }

  if (sidecar)
    .archive_write_sidecar(output, input, info, fps, slices, context_model,
                           verified, ome, backend)

  if (verbose) {
    in_mb  <- file.info(input)$size  / 1024^2
    out_mb <- file.info(output)$size / 1024^2
    message(sprintf("AV1R [archive]: %.1f MB -> %.1f MB (%.2fx). done.",
                    in_mb, out_mb, in_mb / out_mb))
  }
  invisible(output)
}

# ----------------------------------------------------------------------------
# TIFF input
# ----------------------------------------------------------------------------

# Page count, size, sample format; errors unless 16-bit single channel.
.tiff_stack_info <- function(path) {
  meta <- .quiet_tiff(tiff::readTIFF(path, all = TRUE, payload = FALSE))
  n <- nrow(meta)
  if (n == 0L) stop("No images in TIFF: ", path)

  bad_bits <- meta$bits.per.sample != 16L
  bad_spp  <- meta$samples.per.pixel != 1L
  if (any(bad_bits) || any(bad_spp)) {
    i <- which(bad_bits | bad_spp)[1L]
    stop(sprintf(paste0(
      "archive_tiff() supports 16-bit grayscale only; page %d has ",
      "%d bits per sample, %d samples per pixel"),
      i, meta$bits.per.sample[i], meta$samples.per.pixel[i]))
  }
  if (length(unique(meta$width)) != 1L || length(unique(meta$length)) != 1L)
    stop("All TIFF pages must have the same size")

  desc <- if ("description" %in% names(meta)) meta$description[1L] else NULL
  if (!is.null(desc) && (is.na(desc) || !nzchar(desc))) desc <- NULL

  list(n_frames = n, width = meta$width[1L], height = meta$length[1L],
       description = desc)
}

# Read pages `idx` as integer array [height, width, length(idx)].
.tiff_read_frames <- function(path, idx) {
  pages <- .quiet_tiff(tiff::readTIFF(path, all = idx, as.is = TRUE))
  h <- nrow(pages[[1L]]); w <- ncol(pages[[1L]])
  out <- array(0L, dim = c(h, w, length(idx)))
  for (k in seq_along(pages)) {
    p <- pages[[k]]
    if (is.null(p) || !is.integer(p) || !identical(dim(p), c(h, w)))
      stop(sprintf("Unexpected TIFF page %d (not 16-bit integer data)", idx[k]))
    out[, , k] <- p
  }
  out
}

# Frames per chunk: about 256 MB of integer samples, at least 1.
.archive_chunk_frames <- function(width, height) {
  opt <- getOption("AV1R.archive_chunk_frames")
  if (!is.null(opt)) return(max(1L, as.integer(opt)))
  max(1L, as.integer(256 * 1024^2 / (4 * as.numeric(width) * height)))
}

# ----------------------------------------------------------------------------
# Encoder (C++ session: open -> write chunks -> close)
# ----------------------------------------------------------------------------

# get_chunk(idx) must return either an integer array [height, width,
# length(idx)] with values 0..65535, or a raw vector of length(idx)
# row-major gray16le frames (then width and height are required).
# n_frames = NA: unknown count, encode until get_chunk() returns nothing.
.ffv1_encode_stream <- function(get_chunk, n_frames, output, fps = 25,
                                slices = c(2L, 2L), context_model = 0L,
                                chunk = 64L, ome_xml = NULL,
                                backend = "cpu", width = NULL,
                                height = NULL) {
  first <- get_chunk(seq_len(if (is.na(n_frames)) chunk else min(chunk, n_frames)))
  if (length(first) == 0L) stop("no frames to encode")
  if (is.raw(first)) {
    if (is.null(width) || is.null(height))
      stop("width and height are required for raw gray16le chunks")
    w <- width; h <- height
  } else {
    h <- dim(first)[1L]; w <- dim(first)[2L]
  }

  att_name <- character(); att_mime <- character(); att_data <- raw()
  if (!is.null(ome_xml)) {
    att_name <- "metadata.ome.xml"
    att_mime <- "application/xml"
    att_data <- charToRaw(enc2utf8(ome_xml))
  }

  output <- normalizePath(output, mustWork = FALSE)
  ses <- .Call("R_av1r_ffv1_open", output, as.integer(w), as.integer(h),
               as.numeric(fps), as.integer(slices), as.integer(context_model),
               att_name, att_mime, att_data, as.character(backend),
               PACKAGE = "AV1R")
  done <- FALSE
  on.exit(if (!done) {
    .Call("R_av1r_ffv1_abort", ses, PACKAGE = "AV1R")
    unlink(output)
  })

  # n_frames = NA: read until get_chunk() returns an empty chunk.
  s <- 1L
  repeat {
    if (!is.na(n_frames) && s > n_frames) break
    idx <- s:(if (is.na(n_frames)) s + chunk - 1L else min(s + chunk - 1L, n_frames))
    fr <- if (s == 1L) first else get_chunk(idx)
    if (length(fr) == 0L) break
    .Call("R_av1r_ffv1_write", ses, if (is.raw(fr)) fr else .ffv1_as_frames(fr),
          PACKAGE = "AV1R")
    s <- s + chunk
  }
  .Call("R_av1r_ffv1_close", ses, PACKAGE = "AV1R")
  done <- TRUE
  invisible(output)
}

# [height, width, n] (or a matrix) -> integer array [height, width, n] as
# R_av1r_ffv1_write expects. No copy for integer input; the C++ side
# transposes and checks the 0..65535 range (NA included).
.ffv1_as_frames <- function(frames) {
  if (length(dim(frames)) == 2L) dim(frames) <- c(dim(frames), 1L)
  stopifnot(length(dim(frames)) == 3L, is.numeric(frames))
  if (!is.integer(frames)) {
    if (any(frames < 0 | frames > 65535, na.rm = TRUE))
      stop("frames must contain gray16 values in 0..65535 without NA")
    storage.mode(frames) <- "integer"
  }
  frames
}

# FFV1 GPU device name, or NULL if the GPU path is not built / no device.
# Probed once per R session (creating a Vulkan context is not free).
.ffv1_gpu_cache <- new.env(parent = emptyenv())

.ffv1_gpu_device <- function() {
  if (!exists("device", envir = .ffv1_gpu_cache, inherits = FALSE)) {
    dev <- tryCatch(.Call("R_av1r_ffv1_gpu_device", PACKAGE = "AV1R"),
                    error = function(e) NULL)
    assign("device", if (is.null(dev) || is.na(dev)) NULL else dev,
           envir = .ffv1_gpu_cache)
  }
  get("device", envir = .ffv1_gpu_cache, inherits = FALSE)
}

# "auto" -> "vulkan" if a GPU device is available, else "cpu".
.ffv1_resolve_backend <- function(backend) {
  if (backend == "cpu") return("cpu")
  dev <- .ffv1_gpu_device()
  if (!is.null(dev)) return("vulkan")
  if (backend == "vulkan")
    stop("backend = \"vulkan\": no FFV1 GPU path (AV1R built without ",
         "Vulkan/glslc, or no Vulkan compute device)")
  "cpu"
}

.ffv1_backend_label <- function(backend) {
  if (backend == "vulkan") paste0("vulkan: ", .ffv1_gpu_device()) else "cpu"
}

# Encode an in-memory array [height, width, n] (or a matrix) to FFV1/MKV.
.ffv1_encode <- function(frames, output, fps = 25, slices = c(2L, 2L),
                         context_model = 0L, backend = "cpu") {
  frames <- .ffv1_as_frames(frames)   # range is checked by the encoder
  n <- dim(frames)[3L]
  .ffv1_encode_stream(function(idx) frames[, , idx, drop = FALSE], n, output,
                      fps = fps, slices = slices,
                      context_model = context_model, chunk = n,
                      backend = backend)
}

# ----------------------------------------------------------------------------
# Verification (ffmpeg decode -> gray16le pipe -> compare)
# ----------------------------------------------------------------------------

# ffmpeg output options: first video stream as row-major gray16le frames on
# stdout. -an: no audio interleaved into the raw stream. -vsync passthrough:
# every decoded frame exactly once (rawvideo has no timestamps; otherwise
# ffmpeg duplicates or drops frames for a constant rate, e.g. a duplicated
# first frame when the stream starts at pts 1).
.ffv1_gray16_pipe_args <-
  "-map 0:v:0 -an -vsync passthrough -f rawvideo -pix_fmt gray16le pipe:1"

# Compare two gray16le pipes (shell commands) chunk by chunk, bit-exactly.
# Errors on any mismatch or different frame count; returns the frame count.
.ffv1_verify_pipes <- function(cmd_archive, cmd_source, width, height, chunk) {
  frame_bytes <- 2 * as.numeric(width) * height
  a <- pipe(cmd_archive, open = "rb")
  on.exit(close(a))
  b <- pipe(cmd_source, open = "rb")
  on.exit(close(b), add = TRUE)
  n <- 0
  repeat {
    x <- readBin(a, "raw", n = chunk * frame_bytes)
    y <- readBin(b, "raw", n = chunk * frame_bytes)
    if (length(x) != length(y))
      stop(sprintf("FFV1 verify: archive and source differ in length after frame %.0f",
                   n + min(length(x), length(y)) %/% frame_bytes))
    if (length(x) == 0L) break
    if (!identical(x, y)) {
      i <- which(x != y)[1L] - 1
      stop(sprintf("FFV1 verify: mismatch at frame %.0f", n + i %/% frame_bytes + 1))
    }
    n <- n + length(x) %/% frame_bytes
  }
  n
}

# Decode `path` with ffmpeg chunk by chunk and compare bit-exactly with
# get_chunk(idx) ([height, width, length(idx)]). Errors on any mismatch.
.ffv1_verify_stream <- function(path, width, height, n_frames, get_chunk,
                                chunk) {
  ffmpeg <- check_ffmpeg()
  npix <- as.numeric(width) * height

  cmd <- paste(shQuote(ffmpeg), "-v error -i", shQuote(path),
               .ffv1_gray16_pipe_args)
  con <- pipe(cmd, open = "rb")
  on.exit(close(con))

  for (s in seq(1L, n_frames, by = chunk)) {
    idx <- s:min(s + chunk - 1L, n_frames)
    want <- length(idx) * npix
    decoded <- readBin(con, what = "integer", n = want, size = 2L,
                       signed = FALSE, endian = "little")
    if (length(decoded) != want)
      stop(sprintf("FFV1 verify: decoded %.0f samples for frames %d-%d, expected %.0f",
                   length(decoded), idx[1L], idx[length(idx)], want))
    expected <- as.integer(aperm(get_chunk(idx), c(2L, 1L, 3L)))
    bad <- which(decoded != expected)
    if (length(bad) > 0L) {
      i <- bad[1L] - 1
      stop(sprintf(
        "FFV1 verify: %d mismatching samples; first at frame %d, row %d, col %d",
        length(bad), idx[1L] + i %/% npix, (i %% npix) %/% width + 1,
        i %% width + 1))
    }
  }
  extra <- readBin(con, what = "raw", n = 1L)
  if (length(extra) > 0L)
    stop("FFV1 verify: archive contains more frames than the input")
  invisible(TRUE)
}

# Decode `path` and compare with an in-memory array [height, width, n].
.ffv1_verify <- function(path, frames) {
  if (length(dim(frames)) == 2L) dim(frames) <- c(dim(frames), 1L)
  d <- dim(frames)
  .ffv1_verify_stream(path, d[2L], d[1L], d[3L],
                      function(idx) frames[, , idx, drop = FALSE], d[3L])
}

# ----------------------------------------------------------------------------
# Sidecar JSON
# ----------------------------------------------------------------------------

.archive_write_sidecar <- function(output, input, info, fps, slices,
                                   context_model, verified, ome_xml,
                                   backend = "cpu", extra = list()) {
  ffmpeg_version <- NULL
  if (verified) {
    v <- tryCatch(suppressWarnings(system2(check_ffmpeg(), "-version",
                                           stdout = TRUE, stderr = FALSE)),
                  error = function(e) character())
    if (length(v)) ffmpeg_version <- v[1L]
  }
  fields <- list(
    format             = "FFV1",
    ffv1_version       = 3L,
    ffv1_micro_version = 4L,
    coder_type         = 2L,   # custom state transition table (ffmpeg -coder 1)
    context_model      = as.integer(context_model),
    slices_h           = slices[1L],
    slices_v           = slices[2L],
    ec                 = 1L,
    colorspace         = "gray",
    bits_per_sample    = 16L,
    container          = "matroska",
    width              = info$width,
    height             = info$height,
    frames             = info$n_frames,
    fps                = fps,
    source_file        = basename(input),
    source_bytes       = file.info(input)$size,
    source_md5         = unname(tools::md5sum(input)),
    archive_file       = basename(output),
    archive_bytes      = file.info(output)$size,
    archive_md5        = unname(tools::md5sum(output)),
    verified_bit_exact = verified,
    verify_decoder     = ffmpeg_version,
    encoder_backend    = .ffv1_backend_label(backend),
    av1r_version       = as.character(utils::packageVersion("AV1R")),
    r_version          = R.version.string,
    created_utc        = format(Sys.time(), "%Y-%m-%dT%H:%M:%SZ", tz = "UTC"),
    ome_xml_attachment = if (!is.null(ome_xml)) "metadata.ome.xml" else NULL,
    ome_xml            = ome_xml
  )
  fields <- c(fields, extra)
  writeLines(.json_object(fields), paste0(output, ".json"), useBytes = TRUE)
}

# Minimal JSON writer for a flat named list of scalars (string, number,
# logical, NULL).
.json_object <- function(x) {
  vals <- vapply(x, .json_scalar, "")
  keys <- vapply(names(x), .json_string, "")
  paste0("{\n", paste0("  ", keys, ": ", vals, collapse = ",\n"), "\n}")
}

.json_scalar <- function(v) {
  if (is.null(v) || (length(v) == 1L && is.na(v))) return("null")
  stopifnot(length(v) == 1L)
  if (is.logical(v)) return(if (v) "true" else "false")
  if (is.numeric(v)) return(format(v, scientific = FALSE, digits = 15))
  .json_string(as.character(v))
}

.json_string <- function(s) {
  s <- enc2utf8(s)
  s <- gsub("\\", "\\\\", s, fixed = TRUE)
  s <- gsub("\"", "\\\"", s, fixed = TRUE)
  s <- gsub("\n", "\\n", s, fixed = TRUE)
  s <- gsub("\r", "\\r", s, fixed = TRUE)
  s <- gsub("\t", "\\t", s, fixed = TRUE)
  # Remaining control characters U+0000..U+001F (rare; slow path only then)
  if (!grepl("[[:cntrl:]]", s)) return(paste0("\"", s, "\""))
  chars <- strsplit(s, "", useBytes = FALSE)[[1L]]
  ctrl <- vapply(chars, function(ch) {
    cp <- utf8ToInt(ch)
    length(cp) == 1L && cp < 32L
  }, TRUE)
  if (any(ctrl))
    chars[ctrl] <- sprintf("\\u%04x", vapply(chars[ctrl], utf8ToInt, 1L))
  paste0("\"", paste(chars, collapse = ""), "\"")
}
