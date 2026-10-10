test_that(".archive_chunk_frames: ~256 MB of integers, at least 1", {
  withr::local_options(AV1R.archive_chunk_frames = NULL)
  expect_equal(AV1R:::.archive_chunk_frames(1024L, 1024L), 64L)
  expect_equal(AV1R:::.archive_chunk_frames(1e5, 1e5), 1L)
})

test_that(".archive_chunk_frames: option overrides, clamped to 1", {
  withr::local_options(AV1R.archive_chunk_frames = 7)
  expect_identical(AV1R:::.archive_chunk_frames(1024L, 1024L), 7L)
  withr::local_options(AV1R.archive_chunk_frames = 0)
  expect_identical(AV1R:::.archive_chunk_frames(1024L, 1024L), 1L)
})

test_that(".ffv1_as_frames: matrix becomes a 1-frame array", {
  m <- matrix(1:6, 2, 3)
  a <- AV1R:::.ffv1_as_frames(m)
  expect_identical(dim(a), c(2L, 3L, 1L))
  expect_identical(as.vector(a), 1:6)
})

test_that(".ffv1_as_frames: integer input is returned unchanged", {
  a <- array(0:23, dim = c(2L, 3L, 4L))
  expect_identical(AV1R:::.ffv1_as_frames(a), a)
})

test_that(".ffv1_as_frames: double input is converted to integer", {
  a <- array(c(0, 1, 65535, 300), dim = c(2L, 2L, 1L))
  r <- AV1R:::.ffv1_as_frames(a)
  expect_type(r, "integer")
  expect_equal(as.vector(r), c(0L, 1L, 65535L, 300L))
})

test_that(".ffv1_as_frames: rejects out-of-range and non-numeric input", {
  expect_error(AV1R:::.ffv1_as_frames(matrix(c(-1, 0), 1, 2)), "0..65535")
  expect_error(AV1R:::.ffv1_as_frames(matrix(c(0, 65536), 1, 2)), "0..65535")
  expect_error(AV1R:::.ffv1_as_frames(matrix("a", 1, 1)))
  expect_error(AV1R:::.ffv1_as_frames(1:4))
})

test_that(".ffv1_resolve_backend / .ffv1_backend_label: cpu", {
  expect_identical(AV1R:::.ffv1_resolve_backend("cpu"), "cpu")
  expect_identical(AV1R:::.ffv1_backend_label("cpu"), "cpu")
})

test_that(".archive_write_sidecar writes expected JSON fields", {
  input  <- tempfile(fileext = ".tif")
  output <- tempfile(fileext = ".mkv")
  on.exit(unlink(c(input, output, paste0(output, ".json"))))
  writeBin(as.raw(1:100), input)
  writeBin(as.raw(1:10), output)

  info <- list(width = 64L, height = 48L, n_frames = 3L)
  AV1R:::.archive_write_sidecar(output, input, info, fps = 25,
                                slices = c(2L, 2L), context_model = 0L,
                                verified = FALSE,
                                ome_xml = "<a b=\"1\"/>",
                                extra = list(note = "x"))

  json <- paste(readLines(paste0(output, ".json")), collapse = "\n")
  has <- function(s) expect_true(grepl(s, json, fixed = TRUE), info = s)
  has("\"format\": \"FFV1\"")
  has("\"width\": 64")
  has("\"height\": 48")
  has("\"frames\": 3")
  has("\"source_bytes\": 100")
  has("\"archive_bytes\": 10")
  has(sprintf("\"source_md5\": \"%s\"", unname(tools::md5sum(input))))
  has(sprintf("\"source_file\": \"%s\"", basename(input)))
  has("\"verified_bit_exact\": false")
  has("\"verify_decoder\": null")
  has("\"encoder_backend\": \"cpu\"")
  has("\"ome_xml_attachment\": \"metadata.ome.xml\"")
  has("\"ome_xml\": \"<a b=\\\"1\\\"/>\"")
  has("\"note\": \"x\"")
})

test_that(".archive_write_sidecar: no OME-XML gives null fields", {
  input  <- tempfile(fileext = ".tif")
  output <- tempfile(fileext = ".mkv")
  on.exit(unlink(c(input, output, paste0(output, ".json"))))
  writeBin(raw(4), input)
  writeBin(raw(4), output)

  AV1R:::.archive_write_sidecar(output, input,
                                list(width = 1L, height = 1L, n_frames = 1L),
                                fps = 25, slices = c(1L, 1L),
                                context_model = 0L, verified = FALSE,
                                ome_xml = NULL)
  json <- paste(readLines(paste0(output, ".json")), collapse = "\n")
  expect_true(grepl("\"ome_xml_attachment\": null", json, fixed = TRUE))
  expect_true(grepl("\"ome_xml\": null", json, fixed = TRUE))
})
