// Suppress noisy driver warnings (e.g. RADV "not a conformant Vulkan
// implementation") by temporarily redirecting fd 2 to the null device
// (/dev/null, or NUL on Windows).
// RAII: create on stack, fd 2 restored when object goes out of scope.
// Avoids direct use of 'stderr' symbol (CRAN portability NOTE).

#ifndef AV1R_STDERR_SUPPRESS_H
#define AV1R_STDERR_SUPPRESS_H

#include <cstdio>
#include <unistd.h>
#include <fcntl.h>

struct StderrSuppressor {
    int saved_fd = -1;
    StderrSuppressor() {
        fflush(NULL);
        saved_fd = dup(STDERR_FILENO);
#ifdef _WIN32
        int devnull = open("NUL", O_WRONLY);
#else
        int devnull = open("/dev/null", O_WRONLY);
#endif
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
    }
    ~StderrSuppressor() {
        if (saved_fd >= 0) {
            fflush(NULL);
            dup2(saved_fd, STDERR_FILENO);
            close(saved_fd);
        }
    }
};

#endif // AV1R_STDERR_SUPPRESS_H
