## Tests for .tiff_to_frame_sequence

test_that(".tiff_to_frame_sequence errors without tiff", {
  skip_if(requireNamespace("tiff", quietly = TRUE),
          "tiff is installed, cannot test missing-package path")
  tmp <- tempfile(fileext = ".tif")
  file.create(tmp)
  on.exit(unlink(tmp))
  expect_error(
    AV1R:::.tiff_to_frame_sequence(tmp, tempfile()),
    "tiff"
  )
})

test_that(".tiff_to_frame_sequence extracts 16-bit frames from TIFF", {
  skip_if_not_installed("tiff")

  tmpdir <- tempfile("tiff_test_")
  tiff_path <- tempfile(fileext = ".tif")
  on.exit({
    unlink(tmpdir, recursive = TRUE)
    unlink(tiff_path)
  })

  # 3-frame 16-bit grayscale stack with distinct values per frame
  set.seed(1)
  pages <- lapply(1:3, function(k)
    matrix(sample.int(65536L, 8L * 6L, replace = TRUE) - 1L, 8L, 6L) / 65535)
  tiff::writeTIFF(pages, tiff_path, bits.per.sample = 16L)

  result <- suppressMessages(
    AV1R:::.tiff_to_frame_sequence(tiff_path, tmpdir)
  )

  expect_true(grepl("%06d\\.tif$", result))
  frames <- sort(list.files(tmpdir, pattern = "\\.tif$", full.names = TRUE))
  expect_equal(length(frames), 3L)
  for (k in 1:3) {
    meta <- tiff::readTIFF(frames[k], all = TRUE, payload = FALSE)
    expect_equal(meta$bits.per.sample, 16L)
    expect_equal(tiff::readTIFF(frames[k], as.is = TRUE),
                 round(pages[[k]] * 65535), ignore_attr = TRUE)
  }
})

test_that(".convert_tiff_sequence handles skip_existing", {
  skip_if_not(nchar(Sys.which("ffmpeg")) > 0, "ffmpeg not installed")

  tmp_in  <- tempfile("tiff_in_")
  tmp_out <- tempfile("tiff_out_")
  dir.create(tmp_in)
  dir.create(tmp_out)
  on.exit({
    unlink(tmp_in,  recursive = TRUE)
    unlink(tmp_out, recursive = TRUE)
  })

  # Create fake TIFF files
  for (i in 1:3) {
    file.create(file.path(tmp_in, sprintf("frame%03d.tif", i)))
  }

  # Pre-create output file
  dir_name <- basename(normalizePath(tmp_in))
  out_file <- file.path(tmp_out, paste0(dir_name, ".mp4"))
  file.create(out_file)

  opts <- av1r_options(backend = "cpu")
  tiff_files <- list.files(tmp_in, full.names = TRUE)

  result <- suppressMessages(
    AV1R:::.convert_tiff_sequence(tiff_files, tmp_in, tmp_out, opts, skip_existing = TRUE)
  )
  expect_equal(result$status, "skipped")
})
