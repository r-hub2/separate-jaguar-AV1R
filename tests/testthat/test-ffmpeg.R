test_that(".ffmpeg_video_info errors on missing file", {
  skip_if_not(nchar(Sys.which("ffprobe")) > 0 || nchar(Sys.which("ffmpeg")) > 0,
              "ffprobe/ffmpeg not installed")
  expect_error(
    AV1R:::.ffmpeg_video_info("/nonexistent/file.mp4"),
    "Could not read video dimensions"
  )
})

test_that(".ffmpeg_video_info returns correct structure on real video", {
  skip_if_not(nchar(Sys.which("ffprobe")) > 0 || nchar(Sys.which("ffmpeg")) > 0,
              "ffprobe/ffmpeg not installed")
  skip_if_not(file.exists("/mnt/Data2/DS_projects/AV_test/test.mp4"),
              "Test video not available")

  info <- AV1R:::.ffmpeg_video_info("/mnt/Data2/DS_projects/AV_test/test.mp4")

  expect_type(info, "list")
  expect_named(info, c("width", "height", "fps"))
  expect_gt(info$width,  0L)
  expect_gt(info$height, 0L)
  expect_gt(info$fps,    0L)
  expect_equal(info$width  %% 2, 0L)  # Vulkan requires even dimensions
  expect_equal(info$height %% 2, 0L)
})

test_that(".ffmpeg_video_info returns known dimensions for test.mp4", {
  skip_if_not(nchar(Sys.which("ffprobe")) > 0 || nchar(Sys.which("ffmpeg")) > 0,
              "ffprobe/ffmpeg not installed")
  skip_if_not(file.exists("/mnt/Data2/DS_projects/AV_test/test.mp4"),
              "Test video not available")

  info <- AV1R:::.ffmpeg_video_info("/mnt/Data2/DS_projects/AV_test/test.mp4")
  expect_equal(info$width,  1920L)
  expect_equal(info$height, 1080L)
})
