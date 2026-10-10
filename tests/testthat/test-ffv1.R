ffv1_roundtrip <- function(frames, slices = c(2L, 2L), fps = 25) {
  out <- tempfile(fileext = ".mkv")
  on.exit(unlink(out))
  AV1R:::.ffv1_encode(frames, out, fps = fps, slices = slices)
  expect_true(file.exists(out))
  expect_true(AV1R:::.ffv1_verify(out, frames))
}

test_that("FFV1 gray16 round-trip: random full-range samples", {
  skip_if_not(nchar(Sys.which("ffmpeg")) > 0, "ffmpeg not installed")
  set.seed(1)
  h <- 48L; w <- 64L; n <- 3L
  frames <- array(sample.int(65536L, h * w * n, replace = TRUE) - 1L,
                  dim = c(h, w, n))
  ffv1_roundtrip(frames)
})

test_that("FFV1 gray16 round-trip: values around the signed 16-bit boundary", {
  skip_if_not(nchar(Sys.which("ffmpeg")) > 0, "ffmpeg not installed")
  # RFC 9043 3.3.1: predictor works on samples interpreted as int16, so
  # neighbours on both sides of 32768 must be covered.
  set.seed(2)
  h <- 32L; w <- 40L
  edge <- c(0L, 1L, 32766L, 32767L, 32768L, 32769L, 65534L, 65535L)
  f1 <- matrix(sample(edge, h * w, replace = TRUE), h, w)
  f2 <- outer(seq_len(h), seq_len(w), function(i, j)
    ifelse((i + j) %% 2 == 0, 32767L, 32768L))            # checkerboard
  f3 <- outer(seq_len(h), seq_len(w), function(i, j)
    ifelse(j %% 3 == 0, 0L, ifelse(j %% 3 == 1, 65535L, 32768L)))
  f4 <- matrix(32768L + as.integer(round(rnorm(h * w, sd = 50))), h, w)
  frames <- array(c(f1, f2, f3, f4), dim = c(h, w, 4L))
  ffv1_roundtrip(frames)
})

test_that("FFV1 gray16 round-trip: smooth image with noise", {
  skip_if_not(nchar(Sys.which("ffmpeg")) > 0, "ffmpeg not installed")
  set.seed(3)
  h <- 120L; w <- 160L
  base <- outer(seq_len(h), seq_len(w), function(i, j)
    20000 + 15000 * sin(i / 17) * cos(j / 23))
  frames <- array(0L, dim = c(h, w, 2L))
  for (k in 1:2)
    frames[, , k] <- pmin(65535L, pmax(0L, as.integer(round(
      base + rnorm(h * w, sd = 30 * k)))))
  ffv1_roundtrip(frames)
})

test_that("FFV1 gray16 round-trip: slice layouts and odd frame sizes", {
  skip_if_not(nchar(Sys.which("ffmpeg")) > 0, "ffmpeg not installed")
  set.seed(4)
  h <- 23L; w <- 37L
  frames <- array(sample.int(65536L, h * w * 2L, replace = TRUE) - 1L,
                  dim = c(h, w, 2L))
  for (s in list(c(1L, 1L), c(2L, 1L), c(1L, 3L), c(3L, 2L), c(4L, 4L)))
    ffv1_roundtrip(frames, slices = s)
})

test_that("FFV1 gray16 round-trip: tiny frames", {
  skip_if_not(nchar(Sys.which("ffmpeg")) > 0, "ffmpeg not installed")
  ffv1_roundtrip(array(c(0L, 65535L, 32768L, 32767L), dim = c(2L, 2L, 1L)),
                 slices = c(1L, 1L))
  ffv1_roundtrip(array(c(12345L, 54321L), dim = c(1L, 2L, 1L)),
                 slices = c(1L, 1L))
})

test_that("range coder round-trips through RFC 9043 decoder", {
  for (n in c(0L, 1L, 2L, 10L, 1000L, 100000L)) {
    for (seed in 1:5) {
      set.seed(seed)
      res <- .Call("R_av1r_ffv1_rac_selftest", n, PACKAGE = "AV1R")
      expect_identical(res[1], -1L,
                       info = sprintf("n = %d, seed = %d, bytes = %d",
                                      n, seed, res[2]))
    }
  }
})

test_that(".ffv1_encode rejects out-of-range values", {
  expect_error(AV1R:::.ffv1_encode(array(70000, c(4, 4, 1)), tempfile()),
               "0..65535")
})

test_that(".ffv1_encode rejects more slices than pixels", {
  expect_error(AV1R:::.ffv1_encode(array(1L, c(2, 2, 1)), tempfile(),
                                   slices = c(4L, 1L)),
               "invalid slice count")
})

test_that("FFV1 GPU compute self-test (shader runs, results match CPU)", {
  res <- tryCatch(.Call("R_av1r_ffv1_gpu_selftest", 1L, PACKAGE = "AV1R"),
                  error = function(e) e)
  skip_if(is.null(res), "built without the FFV1 GPU path")
  if (inherits(res, "error")) skip(conditionMessage(res))
  set.seed(11)
  for (n in c(1L, 63L, 64L, 65L, 100003L)) {
    res <- .Call("R_av1r_ffv1_gpu_selftest", n, PACKAGE = "AV1R")
    expect_identical(res$first_mismatch, -1L, info = paste("n =", n))
  }
  expect_true(nzchar(res$device))
})

test_that("FFV1 GPU range coder is byte-identical to RangeEncoder", {
  res <- tryCatch(.Call("R_av1r_ffv1_gpu_rac_selftest", 1L, 1L, FALSE,
                        PACKAGE = "AV1R"),
                  error = function(e) e)
  skip_if(is.null(res), "built without the FFV1 GPU path")
  if (inherits(res, "error")) skip(conditionMessage(res))
  cases <- list(c(1L, 0L), c(1L, 100000L), c(7L, 1000L), c(64L, 200000L))
  for (cs in cases) for (custom in c(FALSE, TRUE)) for (seed in 1:2) {
    set.seed(seed)
    res <- .Call("R_av1r_ffv1_gpu_rac_selftest", cs[1], cs[2], custom,
                 PACKAGE = "AV1R")
    expect_identical(res$first_mismatch_stream, -1L,
                     info = sprintf("streams = %d, ops = %d, custom = %s, seed = %d",
                                    cs[1], cs[2], custom, seed))
  }
})

test_that("FFV1 GPU slice encoder is byte-identical to the CPU Encoder", {
  gpu_slice <- function(fr, nh, nv, cm, batch = 2L, bps = 4) {
    storage.mode(fr) <- "integer"
    .Call("R_av1r_ffv1_gpu_slice_selftest", fr, as.integer(nh),
          as.integer(nv), as.integer(cm), as.integer(batch), bps,
          PACKAGE = "AV1R")
  }
  res <- tryCatch(gpu_slice(array(0L, c(1, 1, 1)), 1, 1, 0),
                  error = function(e) e)
  skip_if(is.null(res), "built without the FFV1 GPU path")
  if (inherits(res, "error")) skip(conditionMessage(res))

  set.seed(21)
  near <- c(0:3, 32764:32771, 65532:65535)  # signed 16-bit wrap points
  A <- function(w, h, n, v) array(v, c(w, h, n))
  cases <- list(
    noise     = list(A(64, 48, 3, sample(0:65535, 64 * 48 * 3, TRUE)), 2, 2),
    near      = list(A(37, 29, 5, sample(near, 37 * 29 * 5, TRUE)), 3, 2),
    constants = list(array(rep(c(0, 32767, 32768, 65535), each = 20 * 16),
                           c(20, 16, 4)), 2, 2),
    alt_wrap  = list(A(33, 17, 2, rep(c(32767, 32768), length.out = 33 * 17 * 2)), 2, 3),
    uneven_v  = list(A(16, 7, 2, sample(0:65535, 16 * 7 * 2, TRUE)), 1, 3),
    uneven_h  = list(A(7, 16, 2, sample(0:65535, 7 * 16 * 2, TRUE)), 3, 1),
    one_px    = list(A(1, 1, 3, c(0, 32768, 65535)), 1, 1),
    px_slices = list(A(2, 3, 2, sample(near, 12, TRUE)), 2, 3),
    column    = list(A(1, 40, 2, sample(near, 80, TRUE)), 1, 4),
    row       = list(A(40, 1, 2, sample(near, 80, TRUE)), 4, 1),
    chunked   = list(A(2500, 6, 1, sample(0:65535, 2500 * 6, TRUE)), 1, 2),
    gradient  = list(A(200, 150, 3, outer(outer(1:200, 1:150, "+") * 100, 1:3,
                                          "+") %% 65536), 4, 4))
  for (nm in names(cases)) for (cm in 0:1) {
    cs <- cases[[nm]]
    res <- gpu_slice(cs[[1]], cs[[2]], cs[[3]], cm)
    expect_identical(res$first_mismatch_frame, -1L,
                     info = sprintf("%s, context_model = %d, byte %.0f",
                                    nm, cm, res$first_mismatch_byte))
  }

  # Output buffer overflow is an error, never silent truncation.
  expect_error(gpu_slice(A(64, 48, 1, sample(0:65535, 64 * 48, TRUE)), 1, 1,
                         0, bps = 0.1),
               "output buffer holds")
})

test_that("FFV1 bitstream is byte-identical to ffmpeg -coder 1", {
  ffmpeg  <- Sys.which("ffmpeg")
  ffprobe <- Sys.which("ffprobe")
  skip_if_not(nzchar(ffmpeg) && nzchar(ffprobe), "ffmpeg/ffprobe not installed")
  dir <- tempfile(); dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE))

  # Per-packet MD5 of the FFV1 stream (stream copy) and the CodecPrivate.
  packet_md5 <- function(f) {
    x <- system2(ffmpeg, c("-v", "error", "-i", shQuote(f), "-map", "0:v",
                           "-c", "copy", "-f", "framemd5", "-"), stdout = TRUE)
    sub(".*,\\s*", "", grep("^0,", x, value = TRUE))
  }
  extradata <- function(f)
    system2(ffprobe, c("-v", "error", "-select_streams", "v:0",
                       "-show_entries", "stream=extradata", "-show_data",
                       shQuote(f)), stdout = TRUE)

  set.seed(31)
  near <- c(0:3, 32764:32771, 65532:65535)
  h <- 37L; w <- 53L
  frames <- array(c(sample(0:65535, h * w, TRUE),
                    sample(near, h * w, TRUE),
                    round(outer(1:h, 1:w, function(y, x) 30000 + 90 * x - 70 * y))),
                  c(h, w, 3L))
  raw <- file.path(dir, "in.raw")
  con <- file(raw, "wb")
  for (k in seq_len(dim(frames)[3L]))
    writeBin(as.integer(t(frames[, , k])), con, size = 2L, endian = "little")
  close(con)

  backends <- c("cpu", if (!is.null(AV1R:::.ffv1_gpu_device())) "vulkan")
  for (sl in list(c(2L, 2L), c(4L, 4L))) for (cm in 0:1) {
    # Reference in NUT, not MKV: ffmpeg 6.1.1's Matroska muxer appends a zero
    # byte to the CodecPrivate in some cases (same encoder output is 201
    # bytes in NUT/AVI, 202 in MKV, also after a -c copy remux). The test
    # compares the encoder, not ffmpeg's muxer.
    ref <- file.path(dir, "ffmpeg.nut")
    system2(ffmpeg, c("-v", "error", "-y", "-f", "rawvideo",
                      "-pix_fmt", "gray16le", "-s", sprintf("%dx%d", w, h),
                      "-r", "25", "-i", shQuote(raw), "-c:v", "ffv1",
                      "-level", "3", "-coder", "1", "-context", cm,
                      "-slices", prod(sl), "-slicecrc", "1", "-g", "1",
                      shQuote(ref)))
    for (be in backends) {
      out <- file.path(dir, "ours.mkv")
      AV1R:::.ffv1_encode(frames, out, slices = sl, context_model = cm,
                          backend = be)
      info <- sprintf("slices %dx%d, context %d, backend %s", sl[1], sl[2],
                      cm, be)
      expect_identical(extradata(out), extradata(ref), info = info)
      expect_identical(packet_md5(out), packet_md5(ref), info = info)
    }
  }
})
