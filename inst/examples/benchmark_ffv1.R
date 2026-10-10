#!/usr/bin/env Rscript
# AV1R: FFV1 lossless archive benchmark
#   AV1R Vulkan vs AV1R CPU vs ffmpeg ffv1 (default threads and -threads 1)
# Usage: Rscript benchmark_ffv1.R
#
# TIFF stack: loaded into memory first (not timed); ffmpeg reads the same
# samples from a raw gray16le file (it cannot read multi-page TIFF).
# With verify = TRUE every archive is decoded by ffmpeg and compared
# bit-exactly with the source, chunk by chunk (not timed; failures printed).

library(AV1R)

# ── Settings ──────────────────────────────────────────────────────────────
real_tiff    <- "/mnt/Data2/DS_projects/AV_test1/mitosis.tif"
slices       <- c(2L, 2L)       # same settings for every method
context      <- 0L
verify       <- TRUE            # decode with ffmpeg, report mismatches only
reps         <- 1L              # repetitions per run; the fastest is kept
out_dir      <- "/tmp"            # archives: <dataset>_<method>.mkv

dir.create(out_dir, showWarnings = FALSE, recursive = TRUE)
ffmpeg <- Sys.which("ffmpeg")
if (!nzchar(ffmpeg)) stop("ffmpeg not found")

gpu_device <- AV1R:::.ffv1_gpu_device()
cat(sprintf("GPU: %s\n", if (is.null(gpu_device)) "not available" else gpu_device))
cat(sprintf("ffmpeg: %s\n\n",
            system2(ffmpeg, "-version", stdout = TRUE)[1L]))

# ── Helpers ───────────────────────────────────────────────────────────────
time_best <- function(fun) {
  best <- Inf
  for (r in seq_len(reps)) best <- min(best, system.time(fun())[["elapsed"]])
  best
}

# Frames per R -> C++ call, as archive_tiff
chunk_frames <- function(ds) AV1R:::.archive_chunk_frames(ds$width, ds$height)

# Sequential reader of n row-major gray16le frames from a connection
# opened by open_con(); returns get_chunk(idx) -> raw vector and a closer.
gray16_reader <- function(open_con, width, height) {
  con <- NULL
  frame_bytes <- 2 * width * height
  list(
    get = function(idx) {
      if (is.null(con)) con <<- open_con()
      readBin(con, "raw", n = length(idx) * frame_bytes)
    },
    close = function() if (!is.null(con)) { close(con); con <<- NULL })
}

ffmpeg_ffv1_args <- function(threads) {
  c("-c:v", "ffv1", "-level", "3", "-coder", "1",
    "-context", as.character(context),
    "-slices", as.character(prod(slices)),
    "-slicecrc", "1", "-g", "1",
    if (!is.null(threads)) c("-threads", as.character(threads)))
}

run_ffmpeg <- function(args) {
  status <- system2(ffmpeg, c("-v", "error", "-y", args))
  if (!identical(status, 0L)) stop("ffmpeg failed (status ", status, ")")
}

# Decode the archive and the source chunk by chunk, compare the bytes.
lossless <- function(out, ds) {
  arch <- gray16_reader(function() pipe(paste(
    shQuote(ffmpeg), "-v error -i", shQuote(out),
    "-map 0:v:0 -an -vsync passthrough -f rawvideo -pix_fmt gray16le pipe:1"),
    "rb"),
    ds$width, ds$height)
  src <- gray16_reader(ds$source_con, ds$width, ds$height)
  on.exit({ arch$close(); src$close() })
  chunk <- chunk_frames(ds)
  for (s in seq(1L, ds$n_frames, by = chunk)) {
    idx <- s:min(s + chunk - 1L, ds$n_frames)
    a <- arch$get(idx)
    if (length(a) == 0L || !identical(a, src$get(idx))) return(FALSE)
  }
  length(arch$get(1L)) == 0L   # no extra frames in the archive
}

# ── Datasets ──────────────────────────────────────────────────────────────
# Each dataset: name, width, height, n_frames, av1r_chunk(), ffmpeg_input
# (input arguments), source_con() (gray16le source stream for verify).
datasets <- list()

if (!file.exists(real_tiff)) stop("TIFF not found: ", real_tiff)
info <- suppressWarnings(AV1R:::.tiff_stack_info(real_tiff))
tiff_frames <- suppressWarnings(
  AV1R:::.tiff_read_frames(real_tiff, seq_len(info$n_frames)))
tiff_raw <- file.path(out_dir, "input.raw")
local({
  con <- file(tiff_raw, "wb")
  on.exit(close(con))
  for (k in seq_len(info$n_frames))
    writeBin(as.integer(t(tiff_frames[, , k])), con, size = 2L,
             endian = "little")
})
datasets$tiff <- list(
  name = basename(real_tiff), width = info$width, height = info$height,
  n_frames = info$n_frames,
  av1r_chunk = function() list(
    get = function(idx) tiff_frames[, , idx, drop = FALSE], close = function() NULL),
  ffmpeg_input = c("-f", "rawvideo", "-pix_fmt", "gray16le",
                   "-s", sprintf("%dx%d", info$width, info$height),
                   "-r", "25", "-i", shQuote(tiff_raw)),
  source_con = function() file(tiff_raw, "rb"))

# ── Run ───────────────────────────────────────────────────────────────────
encoders <- c("ffmpeg", "ffmpeg -threads 1", "AV1R cpu",
              if (!is.null(gpu_device)) "AV1R vulkan")

av1r_encode <- function(ds, out, backend) {
  rd <- ds$av1r_chunk()
  on.exit(rd$close())
  AV1R:::.ffv1_encode_stream(rd$get, ds$n_frames, out, slices = slices,
                             context_model = context, chunk = chunk_frames(ds),
                             backend = backend, width = ds$width,
                             height = ds$height)
}

for (ds in datasets) {
  in_mb <- 2 * ds$width * ds$height * ds$n_frames / 1e6
  cat(sprintf("== %s: %dx%d, %d frames, %.1f MB, slices %dx%d, ctx %d ==\n",
              ds$name, ds$width, ds$height, ds$n_frames, in_mb, slices[1L],
              slices[2L], context))

  rows <- list()
  for (enc in encoders) {
    out <- file.path(out_dir, sprintf("%s_%s.mkv",
      tools::file_path_sans_ext(ds$name), gsub("[^a-z0-9]+", "_", tolower(enc))))
    unlink(out)
    t <- tryCatch(time_best(function() switch(enc,
      "AV1R vulkan"       = av1r_encode(ds, out, "vulkan"),
      "AV1R cpu"          = av1r_encode(ds, out, "cpu"),
      "ffmpeg"            = run_ffmpeg(c(ds$ffmpeg_input,
                                         ffmpeg_ffv1_args(NULL), shQuote(out))),
      "ffmpeg -threads 1" = run_ffmpeg(c(ds$ffmpeg_input,
                                         ffmpeg_ffv1_args(1L), shQuote(out))))),
      error = function(e) {
        cat(sprintf("  %s: ERROR %s\n", enc, conditionMessage(e)))
        NA_real_
      })
    if (verify && !is.na(t) && !lossless(out, ds))
      cat(sprintf("  %s: NOT LOSSLESS\n", enc))
    size <- if (!is.na(t)) file.size(out) / 1e6 else NA_real_
    rows[[length(rows) + 1L]] <- data.frame(
      method = enc, time_s = round(t, 2), MB_s = round(in_mb / t, 1),
      ratio = round(in_mb / size, 3), stringsAsFactors = FALSE)
  }
  print(do.call(rbind, rows), row.names = FALSE)
  cat("\n")
}
unlink(tiff_raw)
