# Internal utility functions

# Проверить что ffmpeg бинарник доступен (не -dev пакеты)
check_ffmpeg <- function() {
  ffmpeg <- Sys.which("ffmpeg")
  if (nchar(ffmpeg) == 0L) {
    stop("ffmpeg not found in PATH.\n",
         "  Ubuntu/Debian: sudo apt install ffmpeg\n",
         "  Fedora:        sudo dnf install ffmpeg\n",
         "  macOS:         brew install ffmpeg\n",
         "  No -dev packages required.")
  }
  invisible(ffmpeg)
}

# Форматировать байты
fmt_bytes <- function(x) {
  units <- c("B", "KB", "MB", "GB", "TB")
  i <- 1L
  while (x >= 1024 && i < length(units)) { x <- x / 1024; i <- i + 1L }
  sprintf("%.1f %s", x, units[i])
}
