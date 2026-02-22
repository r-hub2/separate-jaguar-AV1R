#' Convert all videos in a folder to AV1
#'
#' Finds all supported video files in \code{input_dir} and converts them to
#' AV1. Output files are written to \code{output_dir} with the same base name
#' and \code{.mp4} extension.
#'
#' @param input_dir  Path to folder with input files.
#' @param output_dir Path to folder for output files. Created if it does not
#'   exist. Defaults to \code{input_dir}.
#' @param options    An \code{av1r_options} list. Defaults to
#'   \code{av1r_options()}.
#' @param ext        Character vector of input extensions to process.
#'   Default: \code{c("mp4","avi","mkv","mov","tif","tiff")}.
#' @param skip_existing If \code{TRUE} (default), skip files where the output
#'   already exists.
#'
#' @return Invisibly returns a data.frame with columns \code{input},
#'   \code{output}, \code{status} ("ok", "skipped", or "error"),
#'   and \code{message}.
#'
#' @examples
#' \dontrun{
#' # Convert everything in a folder, auto-detect backend
#' convert_folder("~/data/microscopy")
#'
#' # GPU encode to a separate output folder
#' convert_folder("~/data/raw", "~/data/av1",
#'                av1r_options(backend = "vaapi"))
#' }
#' @export
convert_folder <- function(input_dir,
                           output_dir    = input_dir,
                           options       = av1r_options(),
                           ext           = c("mp4", "avi", "mkv", "mov", "tif", "tiff"),
                           skip_existing = TRUE) {
  if (!dir.exists(input_dir))
    stop("Input directory not found: ", input_dir)

  if (!dir.exists(output_dir))
    dir.create(output_dir, recursive = TRUE)

  # Find all matching files
  pattern <- paste0("\\.(", paste(ext, collapse = "|"), ")$")
  files   <- list.files(input_dir, pattern = pattern,
                        ignore.case = TRUE, full.names = TRUE)

  if (length(files) == 0) {
    message("AV1R: no supported files found in ", input_dir)
    return(invisible(data.frame(input=character(), output=character(),
                                status=character(), message=character())))
  }

  bk <- if (options$backend == "auto") detect_backend() else options$backend
  message(sprintf("AV1R batch: %d file(s), backend=%s", length(files), bk))

  results <- vector("list", length(files))

  for (i in seq_along(files)) {
    inp  <- files[[i]]
    base <- sub("\\.[^.]+$", "", basename(inp))
    out  <- file.path(output_dir, paste0(base, "_av1.mp4"))

    if (skip_existing && file.exists(out)) {
      message(sprintf("[%d/%d] skip  %s (exists)", i, length(files), basename(inp)))
      results[[i]] <- list(input=inp, output=out, status="skipped", message="output exists")
      next
    }

    message(sprintf("[%d/%d] %s -> %s", i, length(files), basename(inp), basename(out)))
    status  <- "ok"
    msg     <- ""

    tryCatch(
      convert_to_av1(inp, out, options),
      error = function(e) {
        status  <<- "error"
        msg     <<- conditionMessage(e)
        message("  ERROR: ", msg)
      }
    )

    results[[i]] <- list(input=inp, output=out, status=status, message=msg)
  }

  df <- do.call(rbind, lapply(results, as.data.frame, stringsAsFactors = FALSE))

  n_ok  <- sum(df$status == "ok")
  n_err <- sum(df$status == "error")
  n_skp <- sum(df$status == "skipped")
  message(sprintf("\nAV1R batch done: %d ok, %d skipped, %d errors", n_ok, n_skp, n_err))

  invisible(df)
}
