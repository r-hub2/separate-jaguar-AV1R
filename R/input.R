#' Read a TIFF stack and return basic metadata
#'
#' Does not load pixel data into R — only inspects the file.
#' Useful for checking frame count and dimensions before encoding.
#'
#' @param path Path to a multi-page TIFF file or printf-pattern
#'   (e.g. \code{"frame\%04d.tif"}).
#'
#' @return A list with elements: \code{path}, \code{n_frames},
#'   \code{width}, \code{height}, \code{size_mb}.
#'
#' @examples
#' \dontrun{
#' info <- read_tiff_stack("confocal_z_series.tif")
#' cat(info$n_frames, "frames,", info$size_mb, "MB\n")
#' }
#' @export
read_tiff_stack <- function(path) {
  if (!file.exists(path)) stop("File not found: ", path)

  size_mb <- file.info(path)$size / 1024^2

  # Use av package if available for quick metadata read
  if (requireNamespace("av", quietly = TRUE)) {
    info <- tryCatch(av::av_video_info(path), error = function(e) NULL)
    if (!is.null(info) && !is.null(info$video)) {
      v <- info$video
      return(list(
        path     = path,
        n_frames = as.integer(v$frames),
        width    = as.integer(v$width),
        height   = as.integer(v$height),
        size_mb  = round(size_mb, 2)
      ))
    }
  }

  # Fallback: return size only
  list(
    path     = path,
    n_frames = NA_integer_,
    width    = NA_integer_,
    height   = NA_integer_,
    size_mb  = round(size_mb, 2)
  )
}
