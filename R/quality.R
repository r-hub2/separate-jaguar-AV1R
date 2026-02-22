# QP targets for VAAPI CQP AV1 encode
# SSIM thresholds (approx equivalent to VMAF 95-97):
#   SSIM >= 0.97  -- visually transparent
#   SSIM >= 0.95  -- very good
#   SSIM <  0.93  -- noticeable loss

.SSIM_TARGET_HI <- 0.98   # above this: QP can be increased (save space)
.SSIM_TARGET_LO <- 0.97   # below this: QP must be decreased (improve quality)
.QP_DEFAULT     <- 23L    # starting point (visually transparent for AV1)
.QP_MIN         <- 10L
.QP_MAX         <- 40L
.QP_STEP        <-  2L
.PROBE_SECONDS  <- 30L    # seconds of video used for quality probing

# ============================================================================
# Internal: encode a short clip with given QP, return SSIM score
# ============================================================================
.probe_ssim <- function(input, qp, duration = .PROBE_SECONDS) {
  ffmpeg  <- Sys.which("ffmpeg")
  tmp_out <- tempfile(fileext = ".mp4")
  on.exit(unlink(tmp_out))

  # Encode probe clip
  ret <- suppressWarnings(system2(ffmpeg, c(
    "-y",
    "-vaapi_device", "/dev/dri/renderD128",
    "-i", input,
    "-vf", "format=nv12,hwupload",
    "-c:v", "av1_vaapi",
    "-rc_mode", "CQP",
    "-qp", as.character(qp),
    "-an", "-t", as.character(duration),
    tmp_out
  ), stdout = FALSE, stderr = FALSE))

  if (ret != 0L || !file.exists(tmp_out)) return(NA_real_)

  # Measure SSIM vs original
  lines <- suppressWarnings(system2(ffmpeg, c(
    "-i", tmp_out,
    "-i", input,
    "-lavfi", "ssim",
    "-t", as.character(duration),
    "-f", "null", "-"
  ), stdout = TRUE, stderr = TRUE))

  ssim_line <- grep("SSIM", lines, value = TRUE)
  if (length(ssim_line) == 0) return(NA_real_)

  m <- regmatches(ssim_line, regexpr("All:[0-9.]+", ssim_line))
  if (length(m) == 0) return(NA_real_)
  as.numeric(sub("All:", "", m))
}

# ============================================================================
# Internal: find optimal QP for target SSIM via binary search
# ============================================================================
.find_optimal_qp <- function(input) {
  message("AV1R: probing quality (this runs once per file)...")

  lo <- .QP_MIN
  hi <- .QP_MAX
  best_qp   <- .QP_DEFAULT
  best_ssim <- NA_real_

  # Start at QP=23, then binary search
  qp <- .QP_DEFAULT
  for (iter in seq_len(6L)) {
    ssim <- .probe_ssim(input, qp)
    if (is.na(ssim)) break

    message(sprintf("  QP=%d  SSIM=%.4f", qp, ssim))

    if (ssim >= .SSIM_TARGET_LO && ssim <= .SSIM_TARGET_HI) {
      best_qp   <- qp
      best_ssim <- ssim
      break
    }

    if (ssim > .SSIM_TARGET_HI) {
      # Quality too high (file too large) -> increase QP
      best_qp   <- qp
      best_ssim <- ssim
      lo  <- qp
      qp  <- min(qp + max(.QP_STEP, (hi - qp) %/% 2L), .QP_MAX)
    } else {
      # Quality too low -> decrease QP
      hi  <- qp
      qp  <- max(qp - max(.QP_STEP, (qp - lo) %/% 2L), .QP_MIN)
    }

    if (qp == best_qp) break
  }

  message(sprintf("AV1R: selected QP=%d (SSIM=%.4f)", best_qp,
                  if (is.na(best_ssim)) 0 else best_ssim))
  best_qp
}

#' Measure SSIM quality score between two video files
#'
#' Compares encoded video against the original using SSIM.
#' Values close to 1.0 indicate high similarity.
#'
#' @param original Path to original video file.
#' @param encoded  Path to encoded video file.
#' @param duration Seconds to compare. \code{NULL} = full video.
#'
#' @return Numeric SSIM score (0-1), or \code{NA} on failure.
#' @export
measure_ssim <- function(original, encoded, duration = NULL) {
  ffmpeg <- Sys.which("ffmpeg")
  if (nchar(ffmpeg) == 0) stop("ffmpeg not found")

  t_args <- if (!is.null(duration)) c("-t", as.character(duration)) else character(0)

  lines <- suppressWarnings(system2(ffmpeg, c(
    "-i", encoded,
    "-i", original,
    "-lavfi", "ssim",
    t_args,
    "-f", "null", "-"
  ), stdout = TRUE, stderr = TRUE))

  ssim_line <- grep("SSIM", lines, value = TRUE)
  if (length(ssim_line) == 0) return(NA_real_)
  m <- regmatches(ssim_line, regexpr("All:[0-9.]+", ssim_line))
  if (length(m) == 0) return(NA_real_)
  as.numeric(sub("All:", "", m))
}
