test_that("vulkan_available returns logical", {
  result <- vulkan_available()
  expect_type(result, "logical")
  expect_length(result, 1)
})

test_that("vulkan_devices returns character vector", {
  result <- vulkan_devices()
  expect_type(result, "character")
})

test_that("vulkan_devices marks AV1-capable devices", {
  devs <- vulkan_devices()
  # If Vulkan is available, at least one device should be listed
  if (vulkan_available()) {
    expect_gt(length(devs), 0L)
    # AV1-capable devices must be marked with [AV1]
    av1_devs <- grep("\\[AV1\\]", devs, value = TRUE)
    expect_gt(length(av1_devs), 0L)
  }
})

test_that("detect_backend returns valid value", {
  result <- detect_backend()
  expect_true(result %in% c("vulkan", "vaapi", "cpu"))
})

test_that("detect_backend returns vulkan when Vulkan is available", {
  if (vulkan_available()) {
    result <- detect_backend("vulkan")
    expect_equal(result, "vulkan")
  } else {
    skip("Vulkan not available on this machine")
  }
})

test_that("detect_backend falls back to vaapi or cpu when Vulkan unavailable", {
  if (!vulkan_available()) {
    result <- detect_backend("auto")
    expect_true(result %in% c("vaapi", "cpu"))
  } else {
    skip("Vulkan is available, fallback not tested")
  }
})

test_that("detect_backend respects cpu preference", {
  result <- detect_backend("cpu")
  expect_equal(result, "cpu")
})

test_that("detect_backend returns vaapi when available", {
  if (AV1R:::.vaapi_av1_available()) {
    result <- detect_backend("vaapi")
    expect_equal(result, "vaapi")
  } else {
    skip("VAAPI AV1 not available on this machine")
  }
})

test_that("detect_backend rejects invalid prefer value", {
  expect_error(detect_backend("cuda"))
  expect_error(detect_backend("amf"))
})
