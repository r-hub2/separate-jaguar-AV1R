has_ffmpeg <- function() nchar(Sys.which("ffmpeg")) > 0

# Write a 16-bit grayscale TIFF stack; returns the values as read back by
# tiff::readTIFF(as.is = TRUE) as an array [height, width, n].
write_tiff16 <- function(path, frames) {
  pages <- lapply(seq_len(dim(frames)[3L]),
                  function(k) frames[, , k] / 65535)
  tiff::writeTIFF(pages, path, bits.per.sample = 16L)
  back <- tiff::readTIFF(path, all = TRUE, as.is = TRUE)
  array(unlist(back), dim = dim(frames))
}

random_stack <- function(h, w, n, seed) {
  set.seed(seed)
  array(sample.int(65536L, h * w * n, replace = TRUE) - 1L, dim = c(h, w, n))
}

test_that("archive_tiff: lossless round-trip with verify and sidecar", {
  skip_if_not_installed("tiff")
  skip_if_not(has_ffmpeg(), "ffmpeg not installed")
  dir <- tempfile(); dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE))
  tif <- file.path(dir, "stack.tif")
  frames <- write_tiff16(tif, random_stack(30L, 40L, 5L, 1))

  out <- archive_tiff(tif, verify = TRUE, verbose = FALSE)
  expect_equal(out, file.path(dir, "stack.mkv"))
  expect_true(file.exists(out))
  expect_true(AV1R:::.ffv1_verify(out, frames))

  js <- readLines(paste0(out, ".json"))
  expect_true(any(grepl("\"verified_bit_exact\": true", js, fixed = TRUE)))
  expect_true(any(grepl("\"frames\": 5", js, fixed = TRUE)))
  expect_true(any(grepl("\"bits_per_sample\": 16", js, fixed = TRUE)))
  expect_true(any(grepl("\"ome_xml\": null", js, fixed = TRUE)))
})

test_that("archive_tiff: chunked streaming gives the same result", {
  skip_if_not_installed("tiff")
  skip_if_not(has_ffmpeg(), "ffmpeg not installed")
  dir <- tempfile(); dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE))
  tif <- file.path(dir, "stack.tif")
  frames <- write_tiff16(tif, random_stack(17L, 23L, 7L, 2))

  old <- options(AV1R.archive_chunk_frames = 2L)
  on.exit(options(old), add = TRUE)
  a <- archive_tiff(tif, file.path(dir, "a.mkv"), slices = c(3L, 2L),
                    verify = TRUE, sidecar = FALSE, verbose = FALSE)
  options(AV1R.archive_chunk_frames = 100L)
  b <- archive_tiff(tif, file.path(dir, "b.mkv"), slices = c(3L, 2L),
                    sidecar = FALSE, verbose = FALSE)

  expect_identical(unname(tools::md5sum(a)), unname(tools::md5sum(b)))
  expect_false(file.exists(paste0(a, ".json")))
})

test_that("archive_tiff: rejects non-16-bit and RGB TIFFs", {
  skip_if_not_installed("tiff")
  dir <- tempfile(); dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE))

  t8 <- file.path(dir, "g8.tif")
  tiff::writeTIFF(matrix(0.5, 4, 4), t8, bits.per.sample = 8L)
  expect_error(archive_tiff(t8, verbose = FALSE), "16-bit grayscale only")

  trgb <- file.path(dir, "rgb16.tif")
  tiff::writeTIFF(array(0.5, c(4, 4, 3)), trgb, bits.per.sample = 16L)
  expect_error(archive_tiff(trgb, verbose = FALSE), "16-bit grayscale only")

  expect_false(file.exists(file.path(dir, "g8.mkv")))
  expect_false(file.exists(file.path(dir, "rgb16.mkv")))
})

test_that("archive_tiff: input validation", {
  skip_if_not_installed("tiff")
  expect_error(archive_tiff("/nonexistent/x.tif", verbose = FALSE),
               "File not found")
  dir <- tempfile(); dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE))
  tif <- file.path(dir, "s.tif")
  write_tiff16(tif, random_stack(4L, 4L, 1L, 3))
  expect_error(archive_tiff(tif, tif, verbose = FALSE), "must differ")
})

test_that("verify detects a mismatch", {
  skip_if_not(has_ffmpeg(), "ffmpeg not installed")
  out <- tempfile(fileext = ".mkv")
  on.exit(unlink(out))
  frames <- random_stack(8L, 8L, 2L, 4)
  AV1R:::.ffv1_encode(frames, out, slices = c(1L, 1L))
  wrong <- frames
  wrong[3, 5, 2] <- (wrong[3, 5, 2] + 1L) %% 65536L
  expect_error(AV1R:::.ffv1_verify(out, wrong),
               "1 mismatching samples; first at frame 2, row 3, col 5")
})

test_that("verify detects a frame count mismatch", {
  skip_if_not(has_ffmpeg(), "ffmpeg not installed")
  out <- tempfile(fileext = ".mkv")
  on.exit(unlink(out))
  frames <- random_stack(8L, 8L, 3L, 5)
  AV1R:::.ffv1_encode(frames, out, slices = c(1L, 1L))
  expect_error(AV1R:::.ffv1_verify(out, frames[, , 1:2]), "more frames")
  expect_error(AV1R:::.ffv1_verify(out, array(c(frames, frames[, , 1]),
                                             c(8L, 8L, 4L))),
               "decoded")
})

test_that("OME-XML is stored as MKV attachment", {
  skip_if_not(nchar(Sys.which("ffprobe")) > 0, "ffprobe not installed")
  out <- tempfile(fileext = ".mkv")
  on.exit(unlink(out))
  frames <- random_stack(8L, 8L, 1L, 6)
  ome <- "<?xml version=\"1.0\"?><OME><Image ID=\"Image:0\"/></OME>"
  AV1R:::.ffv1_encode_stream(function(idx) frames[, , idx, drop = FALSE], 1L,
                             out, slices = c(1L, 1L), chunk = 1L,
                             ome_xml = ome)
  tags <- suppressWarnings(system2(
    Sys.which("ffprobe"),
    c("-v", "error", "-select_streams", "t:0", "-show_entries",
      "stream_tags=filename,mimetype", "-of", "default=noprint_wrappers=1",
      shQuote(out)),
    stdout = TRUE, stderr = FALSE))
  expect_true("TAG:filename=metadata.ome.xml" %in% tags)
  expect_true("TAG:mimetype=application/xml" %in% tags)
})

test_that("archive_tiff: OME-XML from ImageDescription of a real OME-TIFF", {
  skip_if_not_installed("tiff")
  skip_if_not(has_ffmpeg(), "ffmpeg not installed")
  skip_on_os("windows")  # convert.exe is a system utility there
  convert <- Sys.which("convert")
  skip_if_not(nchar(convert) > 0, "ImageMagick not installed")
  dir <- tempfile(); dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE))

  plain <- file.path(dir, "plain.tif")
  tif <- file.path(dir, "stack.ome.tif")
  frames <- write_tiff16(plain, random_stack(37L, 53L, 3L, 7))
  ome <- paste0(
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
    "<OME xmlns=\"http://www.openmicroscopy.org/Schemas/OME/2016-06\">",
    "<Image ID=\"Image:0\" Name=\"test\"><Pixels ID=\"Pixels:0\" ",
    "DimensionOrder=\"XYZCT\" Type=\"uint16\" SizeX=\"53\" SizeY=\"37\" ",
    "SizeZ=\"1\" SizeC=\"1\" SizeT=\"3\" PhysicalSizeX=\"0.65\" ",
    "PhysicalSizeXUnit=\"µm\"><Channel ID=\"Channel:0:0\"/>",
    "<TiffData IFD=\"0\" PlaneCount=\"3\"/></Pixels></Image></OME>")
  system2(convert, c(shQuote(plain), "-set", "comment", shQuote(ome),
                     "-compress", "none", shQuote(tif)))
  meta <- tiff::readTIFF(tif, all = TRUE, payload = FALSE)
  skip_if_not(identical(meta$description[1L], ome),
              "ImageMagick did not write ImageDescription")

  out <- archive_tiff(tif, file.path(dir, "out.mkv"), verify = TRUE,
                      verbose = FALSE)
  expect_true(AV1R:::.ffv1_verify(out, frames))

  # attachment bytes == ImageDescription
  owd <- setwd(dir); on.exit(setwd(owd), add = TRUE)
  system2(Sys.which("ffmpeg"), c("-v", "error", "-y", "-dump_attachment:t",
                                 "\"\"", "-i", shQuote(out)),
          stdout = FALSE, stderr = FALSE)
  att <- file.path(dir, "metadata.ome.xml")
  expect_true(file.exists(att))
  expect_identical(readBin(att, "raw", file.size(att)),
                   charToRaw(enc2utf8(ome)))

  js <- readLines(paste0(out, ".json"), encoding = "UTF-8")
  expect_true(any(grepl("\"ome_xml_attachment\": \"metadata.ome.xml\"", js,
                        fixed = TRUE)))
  expect_true(any(grepl("\"verified_bit_exact\": true", js, fixed = TRUE)))
})

test_that("JSON writer escapes strings and handles scalars", {
  expect_equal(AV1R:::.json_string("a\"b\\c\nd\te"),
               "\"a\\\"b\\\\c\\nd\\te\"")
  expect_equal(AV1R:::.json_string("x\001y"), "\"x\\u0001y\"")
  expect_equal(AV1R:::.json_scalar(NULL), "null")
  expect_equal(AV1R:::.json_scalar(TRUE), "true")
  expect_equal(AV1R:::.json_scalar(123456789012), "123456789012")
  expect_equal(AV1R:::.json_scalar(25), "25")
  expect_equal(AV1R:::.json_object(list(a = 1L, b = "x")),
               "{\n  \"a\": 1,\n  \"b\": \"x\"\n}")
})

test_that("archive_tiff: Vulkan backend gives a byte-identical archive", {
  skip_if_not_installed("tiff")
  skip_if_not(has_ffmpeg(), "ffmpeg not installed")
  skip_if(is.null(AV1R:::.ffv1_gpu_device()), "no FFV1 GPU path / device")
  dir <- tempfile(); dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE))
  tif <- file.path(dir, "stack.tif")
  # 37 frames in chunks of 20: GPU batches 16 + 4 and 16 + 1
  write_tiff16(tif, random_stack(29L, 41L, 37L, 9))
  old <- options(AV1R.archive_chunk_frames = 20L)
  on.exit(options(old), add = TRUE)

  for (cm in 0:1) {
    cpu <- archive_tiff(tif, file.path(dir, "cpu.mkv"), slices = c(3L, 2L),
                        context_model = cm, backend = "cpu", verbose = FALSE)
    gpu <- archive_tiff(tif, file.path(dir, "gpu.mkv"), slices = c(3L, 2L),
                        context_model = cm, backend = "vulkan", verify = TRUE,
                        verbose = FALSE)
    expect_identical(unname(tools::md5sum(gpu)), unname(tools::md5sum(cpu)),
                     info = paste("context_model =", cm))
  }
  js <- readLines(paste0(gpu, ".json"))
  expect_true(any(grepl("\"encoder_backend\": \"vulkan: ", js, fixed = TRUE)))
  expect_true(any(grepl("\"verified_bit_exact\": true", js, fixed = TRUE)))
  js <- readLines(paste0(cpu, ".json"))
  expect_true(any(grepl("\"encoder_backend\": \"cpu\"", js, fixed = TRUE)))
})

test_that("archive_tiff: backend argument is validated", {
  skip_if_not_installed("tiff")
  dir <- tempfile(); dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE))
  tif <- file.path(dir, "stack.tif")
  write_tiff16(tif, random_stack(8L, 8L, 1L, 3))
  expect_error(archive_tiff(tif, backend = "cuda", verbose = FALSE))
  if (is.null(AV1R:::.ffv1_gpu_device()))
    expect_error(archive_tiff(tif, backend = "vulkan", verbose = FALSE),
                 "no FFV1 GPU path")
})
