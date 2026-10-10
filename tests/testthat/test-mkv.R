mkv_selftest <- function(path, n, w = 64L, h = 48L, fps = 25,
                         frame_size = 100L, att_name = character(),
                         att_data = raw()) {
  .Call("R_av1r_mkv_selftest", path, as.integer(w), as.integer(h),
        as.numeric(fps), as.integer(n), as.integer(frame_size),
        att_name, att_data, PACKAGE = "AV1R")
}

# Run ffprobe and return "key=value" output lines as a named character vector.
ffprobe_kv <- function(path, args) {
  out <- suppressWarnings(system2(
    Sys.which("ffprobe"),
    c("-v", "error", args, "-of", "default=noprint_wrappers=1", shQuote(path)),
    stdout = TRUE, stderr = FALSE
  ))
  kv <- strsplit(out[grepl("=", out, fixed = TRUE)], "=", fixed = TRUE)
  stats::setNames(vapply(kv, function(x) paste(x[-1], collapse = "="), ""),
                  vapply(kv, `[`, "", 1))
}

test_that("MKV writer: ffprobe sees V_FFV1 track, size and all packets", {
  skip_if_not(nchar(Sys.which("ffprobe")) > 0, "ffprobe not installed")
  out <- tempfile(fileext = ".mkv")
  on.exit(unlink(out))

  n <- 60L   # 2.4 s at 25 fps -> 3 clusters
  mkv_selftest(out, n, w = 640L, h = 480L, fps = 25)

  v <- ffprobe_kv(out, c("-count_packets", "-select_streams", "v:0",
                         "-show_entries",
                         "stream=codec_name,width,height,nb_read_packets"))
  expect_equal(unname(v["codec_name"]), "ffv1")
  expect_equal(unname(v["width"]), "640")
  expect_equal(unname(v["height"]), "480")
  expect_equal(unname(v["nb_read_packets"]), as.character(n))

  f <- ffprobe_kv(out, c("-show_entries", "format=format_name,duration"))
  expect_match(f["format_name"], "matroska")
  expect_equal(as.numeric(f["duration"]), n / 25, tolerance = 1e-3)
})

test_that("MKV writer: packets carry frame data and timestamps", {
  skip_if_not(nchar(Sys.which("ffprobe")) > 0, "ffprobe not installed")
  out <- tempfile(fileext = ".mkv")
  on.exit(unlink(out))

  n <- 30L
  mkv_selftest(out, n, fps = 10, frame_size = 37L)

  p <- suppressWarnings(system2(
    Sys.which("ffprobe"),
    c("-v", "error", "-select_streams", "v:0",
      "-show_entries", "packet=pts_time,size,flags",
      "-of", "csv=p=0", shQuote(out)),
    stdout = TRUE, stderr = FALSE
  ))
  p <- read.csv(text = p, header = FALSE,
                col.names = c("pts_time", "size", "flags"))
  expect_equal(nrow(p), n)
  expect_equal(p$pts_time, (seq_len(n) - 1) / 10, tolerance = 1e-6)
  expect_true(all(p$size == 37L))
  expect_true(all(startsWith(p$flags, "K")))
})

test_that("MKV writer: attachment is stored with name and mime type", {
  skip_if_not(nchar(Sys.which("ffprobe")) > 0, "ffprobe not installed")
  out <- tempfile(fileext = ".mkv")
  on.exit(unlink(out))

  xml <- charToRaw("<OME><Image ID=\"Image:0\"/></OME>")
  mkv_selftest(out, 5L, att_name = "metadata.ome.xml", att_data = xml)

  a <- ffprobe_kv(out, c("-select_streams", "t:0", "-show_entries",
                         "stream_tags=filename,mimetype"))
  expect_equal(unname(a["TAG:filename"]), "metadata.ome.xml")
  expect_equal(unname(a["TAG:mimetype"]), "application/xml")

  # Extract it back and compare bytes
  dump_dir <- tempfile()
  dir.create(dump_dir)
  on.exit(unlink(dump_dir, recursive = TRUE), add = TRUE)
  old <- setwd(dump_dir)
  on.exit(setwd(old), add = TRUE)
  suppressWarnings(system2(Sys.which("ffmpeg"),
                           c("-v", "error", "-dump_attachment:t:0",
                             "metadata.ome.xml", "-i", shQuote(out)),
                           stdout = FALSE, stderr = FALSE))
  skip_if_not(file.exists("metadata.ome.xml"), "ffmpeg -dump_attachment failed")
  expect_identical(readBin("metadata.ome.xml", "raw", 1000L), xml)
})

test_that("MKV writer: output is deterministic", {
  a <- tempfile(fileext = ".mkv"); b <- tempfile(fileext = ".mkv")
  on.exit(unlink(c(a, b)))
  mkv_selftest(a, 40L)
  mkv_selftest(b, 40L)
  expect_identical(tools::md5sum(a)[[1]], tools::md5sum(b)[[1]])
})

test_that("MKV writer: no frames is an error and leaves no file", {
  out <- tempfile(fileext = ".mkv")
  on.exit(unlink(out))
  expect_error(mkv_selftest(out, 0L), "no frames written")
  expect_false(file.exists(out))
})
