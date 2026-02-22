library(AV1R)

input  <- "/mnt/Data2/DS_projects/AV_test/test.mp4"
output <- "/mnt/Data2/DS_projects/AV_test/test_av1_gpu.mp4"

# Check what GPU backend is available
bk <- detect_backend()
cat("Detected backend:", bk, "\n")

# Show GPU devices if Vulkan is available
if (vulkan_available()) {
  cat("Vulkan devices:\n")
  cat(paste(" ", vulkan_devices()), sep = "\n")
}

# Encode using best available GPU backend (vaapi on this machine)
convert_to_av1(
  input  = input,
  output = output,
  options = av1r_options(
    crf     = 28,     # quality
    backend = "vaapi" # GPU via VAAPI (AMD RX 9070)
  )
)

# Compare file sizes
size_in  <- file.info(input)$size  / 1024^2
size_out <- file.info(output)$size / 1024^2
cat(sprintf("\nOriginal:  %.1f MB\nAV1 (GPU): %.1f MB\nRatio:     %.2fx smaller\n",
            size_in, size_out, size_in / size_out))
