has_ffmpeg <- function() nchar(Sys.which("ffmpeg")) > 0 &&
  nchar(Sys.which("ffprobe")) > 0

# Short test video: 8-bit colour test pattern with a sine audio track.
make_video <- function(path, frames = 12L, audio = TRUE) {
  args <- c("-v", "error", "-y",
            "-f", "lavfi", "-i", sprintf("testsrc2=size=64x48:rate=24:duration=%g",
                                         frames / 24),
            if (audio) c("-f", "lavfi", "-i", "sine=frequency=440:duration=1"),
            "-c:v", "ffv1", if (audio) c("-c:a", "pcm_s16le", "-shortest"),
            shQuote(path))
  stopifnot(identical(system2(Sys.which("ffmpeg"), args), 0L))
}

n_streams <- function(path, type) {
  length(system2(Sys.which("ffprobe"),
                 c("-v", "error", "-select_streams", type, "-show_entries",
                   "stream=index", "-of", "csv=p=0", shQuote(path)),
                 stdout = TRUE))
}

test_that("archive_video: lossless gray16 archive, video only by default", {
  skip_if_not(has_ffmpeg(), "ffmpeg/ffprobe not installed")
  dir <- tempfile(); dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE))
  src <- file.path(dir, "clip.mkv")
  make_video(src)

  out <- archive_video(src, verify = TRUE, verbose = FALSE, backend = "cpu")
  expect_equal(out, file.path(dir, "clip.ffv1.mkv"))
  expect_equal(n_streams(out, "v"), 1L)
  expect_equal(n_streams(out, "a"), 0L)

  js <- readLines(paste0(out, ".json"))
  expect_true(any(grepl("\"verified_bit_exact\": true", js, fixed = TRUE)))
  expect_true(any(grepl("\"frames\": 12", js, fixed = TRUE)))
  expect_true(any(grepl("\"audio\": \"none\"", js, fixed = TRUE)))
})

test_that("archive_video: audio = TRUE copies the sound track", {
  skip_if_not(has_ffmpeg(), "ffmpeg/ffprobe not installed")
  dir <- tempfile(); dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE))
  src <- file.path(dir, "clip.mkv")
  make_video(src)

  out <- archive_video(src, file.path(dir, "a.mkv"), audio = TRUE,
                       verify = TRUE, verbose = FALSE, backend = "cpu")
  expect_equal(n_streams(out, "v"), 1L)
  expect_equal(n_streams(out, "a"), 1L)
  expect_false(file.exists(paste0(out, ".video.mkv")))
  js <- readLines(paste0(out, ".json"))
  expect_true(any(grepl("\"audio\": \"copied\"", js, fixed = TRUE)))
})
