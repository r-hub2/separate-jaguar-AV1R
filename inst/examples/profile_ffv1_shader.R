#!/usr/bin/env Rscript
# AV1R: FFV1 GPU shader profile (slice.comp only)
# Usage: Rscript profile_ffv1_shader.R
#
# Frames are uploaded once per run; each timed dispatch then runs the pixel
# copy, the shader and the output copies, without gathering, muxing or
# R-side preparation. With GPU timestamps (timer = "gpu") the shader and
# the copies are timed separately on the GPU; wall is submit + wait on the
# host, so overhead = wall - (shader + copies) is submit / sync cost.
# Without timestamp support only wall is measured (timer = "wall").
#
# Columns:
#   shader_ms   median shader time per dispatch (min in shader_min_ms)
#   copy_ms     median pixel + output copy time
#   wall_ms     median submit + wait time
#   MB_s        input bytes (2 per sample) / shader time
#   ns_sample   shader time / samples in the batch
#   bits_sample coded bits per sample (Slice footers excluded)
#   ns_bit      shader time * coders / coded bits, coders = batch * slices:
#               time per coded bit of one Slice coder if all run at once
#   note        why a run was skipped

library(AV1R)

# ── Settings ──────────────────────────────────────────────────────────────
real_tiff    <- "/mnt/Data2/DS_projects/AV_test1/mitosis.tif"
slice_grid   <- list(c(2L, 2L), c(4L, 4L), c(8L, 8L))
context_sets <- c(0L, 1L)
batches      <- c(16L, 32L, 64L, 128L)
reps         <- 1L        # timed dispatches per run (after one warm-up)
max_mib      <- 2048      # skip runs whose buffers are estimated above this

gpu_device <- AV1R:::.ffv1_gpu_device()
if (is.null(gpu_device) || is.na(gpu_device))
  stop("FFV1 GPU path not available")
cat(sprintf("GPU: %s\n\n", gpu_device))

# ── Datasets: integer arrays [width, height, n] (row-major frames) ────────
datasets <- list()

if (!file.exists(real_tiff)) stop("TIFF not found: ", real_tiff)
info <- suppressWarnings(AV1R:::.tiff_stack_info(real_tiff))
n_real <- min(info$n_frames, max(batches, na.rm = TRUE))
datasets$real <- list(
  name   = basename(real_tiff),
  frames = aperm(suppressWarnings(
    AV1R:::.tiff_read_frames(real_tiff, seq_len(n_real))), c(2L, 1L, 3L)))

# ── Run ───────────────────────────────────────────────────────────────────
med <- function(x) if (length(x)) stats::median(x) else NA_real_

rows <- list()
for (ds in datasets) {
  d <- dim(ds$frames)
  cat(sprintf("== %s: %dx%d, %d distinct frames ==\n",
              ds$name, d[1L], d[2L], d[3L]))
  for (sl in slice_grid) for (cm in context_sets) for (b in batches) {
    r <- .Call("R_av1r_ffv1_gpu_bench", ds$frames, as.integer(sl),
               as.integer(cm), as.integer(b), reps, max_mib, PACKAGE = "AV1R")
    if (is.null(r)) stop("FFV1 GPU path not built")
    batch_lbl <- if (is.na(b)) sprintf("auto(%d)", r$batch) else
      as.character(r$batch)
    ok <- is.na(r$skipped)

    shader <- if (ok && r$timer == "gpu") r$shader_ns / 1e6 else NA_real_
    copies <- if (ok && r$timer == "gpu")
      (r$before_ns + r$after_ns) / 1e6 else NA_real_
    wall   <- if (ok) r$wall_s * 1e3 else NA_real_
    # Without timestamps the shader rate falls back to wall time.
    t_ns   <- if (ok) (if (r$timer == "gpu") med(shader) else med(wall)) * 1e6
              else NA_real_
    bits   <- r$coded_bytes * 8

    rows[[length(rows) + 1L]] <- data.frame(
      dataset       = ds$name,
      slices        = sprintf("%dx%d", sl[1L], sl[2L]),
      ctx           = cm,
      batch         = batch_lbl,
      timer         = if (ok) r$timer else NA_character_,
      shader_ms     = med(shader),
      shader_min_ms = if (all(is.na(shader))) NA_real_ else min(shader),
      copy_ms       = med(copies),
      wall_ms       = med(wall),
      overhead_ms   = med(wall - shader - copies),
      MB_s          = 2 * r$samples / t_ns * 1e3,
      ns_sample     = t_ns / r$samples,
      bits_sample   = bits / r$samples,
      ns_bit        = t_ns * r$batch * r$n_slices / bits,
      note          = if (ok) "" else r$skipped,
      stringsAsFactors = FALSE)

    if (ok)
      cat(sprintf(
        "  slices %dx%d ctx %d batch %-9s %s  shader %8.2f ms  copy %6.2f ms  wall %8.2f ms  %7.1f MB/s  %6.2f ns/bit\n",
        sl[1L], sl[2L], cm, batch_lbl, r$timer, med(shader), med(copies),
        med(wall), 2 * r$samples / t_ns * 1e3,
        t_ns * r$batch * r$n_slices / bits))
    else
      cat(sprintf("  slices %dx%d ctx %d batch %-9s skipped: %s\n",
                  sl[1L], sl[2L], cm, batch_lbl, r$skipped))
  }
  cat("\n")
}

res <- do.call(rbind, rows)
num <- vapply(res, is.numeric, logical(1L))
res[num] <- lapply(res[num], round, 2L)
print(res, row.names = FALSE)
