test_that("av1r_options returns correct defaults", {
  o <- av1r_options()
  expect_equal(o$crf,     28L)
  expect_equal(o$preset,   8L)
  expect_equal(o$threads,  0L)
  expect_equal(o$backend, "auto")
  expect_null(o$bitrate)
  expect_s3_class(o, "av1r_options")
})

test_that("av1r_options accepts bitrate parameter", {
  expect_no_error(av1r_options(bitrate = 3000))
  expect_equal(av1r_options(bitrate = 3000)$bitrate, 3000L)
  expect_error(av1r_options(bitrate = 0))
  expect_error(av1r_options(bitrate = -1))
})

test_that("av1r_options validates crf range", {
  expect_error(av1r_options(crf = -1))
  expect_error(av1r_options(crf = 64))
  expect_no_error(av1r_options(crf = 0))
  expect_no_error(av1r_options(crf = 63))
})

test_that("av1r_options validates preset range", {
  expect_error(av1r_options(preset = -1))
  expect_error(av1r_options(preset = 14))
  expect_no_error(av1r_options(preset = 0))
  expect_no_error(av1r_options(preset = 13))
})

test_that("av1r_options validates backend", {
  expect_no_error(av1r_options(backend = "auto"))
  expect_no_error(av1r_options(backend = "cpu"))
  expect_no_error(av1r_options(backend = "vulkan"))
  expect_no_error(av1r_options(backend = "vaapi"))
  expect_error(av1r_options(backend = "gpu"))
  expect_error(av1r_options(backend = "invalid"))
})

test_that("print.av1r_options prints without error", {
  o <- av1r_options()
  expect_output(print(o))
})
