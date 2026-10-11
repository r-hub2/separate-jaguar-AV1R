# Skip a test that needs a real Vulkan AV1 encode device.
#
# Being compiled with Vulkan headers is not enough: a CRAN/r-hub machine can
# build the GPU path and still have no device, so ask for an AV1-capable device.
skip_if_no_vulkan_av1 <- function(what = "Vulkan AV1 not available") {
  ok <- tryCatch(isTRUE(vulkan_available()), error = function(e) FALSE)
  skip_if_not(ok, what)
}
