#' AV1R encoding options
#'
#' @param crf     Constant Rate Factor: 0 (lossless) to 63 (worst). Default 28.
#'   Lower = better quality, larger file. Used as fallback when \code{bitrate}
#'   is \code{NULL} and input bitrate cannot be detected.
#' @param preset  Encoding speed preset: 0 (slowest/best) to 13 (fastest).
#'   Default 8 (good balance for microscopy batch jobs).
#' @param threads Number of CPU threads. 0 = auto-detect.
#' @param bitrate Target video bitrate in kbps (e.g. \code{3000} for 3 Mbps).
#'   \code{NULL} (default) = auto-detect from input (55\% of source bitrate
#'   for VAAPI, CRF for CPU).
#' @param backend \code{"auto"} (best GPU if available, else CPU),
#'   \code{"vulkan"} (Vulkan AV1), \code{"vaapi"} (VAAPI AV1, AMD/Intel),
#'   or \code{"cpu"}.
#'
#' @return A named list of encoding parameters.
#'
#' @examples
#' # Default options (auto-detect backend and bitrate)
#' av1r_options()
#'
#' # High-quality lossless-ish for publication figures
#' av1r_options(crf = 15, preset = 4)
#'
#' # Explicit bitrate (GPU VAAPI)
#' av1r_options(bitrate = 2000, backend = "vaapi")
#'
#' # Fast batch conversion of large TIFF stacks
#' av1r_options(crf = 32, preset = 12, threads = 16)
#'
#' @export
av1r_options <- function(crf     = 28L,
                          preset  = 8L,
                          threads = 0L,
                          bitrate = NULL,
                          backend = "auto") {
  backend <- match.arg(backend, c("auto", "vulkan", "vaapi", "cpu"))
  stopifnot(is.numeric(crf),    crf    >= 0, crf    <= 63)
  stopifnot(is.numeric(preset), preset >= 0, preset <= 13)
  stopifnot(is.numeric(threads), threads >= 0)
  if (!is.null(bitrate)) stopifnot(is.numeric(bitrate), bitrate > 0)

  structure(
    list(crf     = as.integer(crf),
         preset  = as.integer(preset),
         threads = as.integer(threads),
         bitrate = if (is.null(bitrate)) NULL else as.integer(bitrate),
         backend = backend),
    class = "av1r_options"
  )
}

#' @export
print.av1r_options <- function(x, ...) {
  btr <- if (is.null(x$bitrate)) "auto" else paste0(x$bitrate, "k")
  cat(sprintf(
    "AV1R options: crf=%d  preset=%d  threads=%s  bitrate=%s  backend=%s\n",
    x$crf, x$preset,
    if (x$threads == 0L) "auto" else as.character(x$threads),
    btr,
    x$backend
  ))
  invisible(x)
}
