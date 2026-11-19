// runtime/compat_win32.h's definitions (Windows only; empty elsewhere).
#ifdef _WIN32
#include "compat_win32.h"

#include <fcntl.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// open() and fopen() default to binary, as on POSIX (text mode would turn \r\n into \n inside save data)
static const int binary_files = (_set_fmode(_O_BINARY), 0);

static ssize_t at_offset(int fd, void *buf, size_t n, long long offset, bool write) {
    const HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov{};
    ov.Offset = (DWORD)(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
    DWORD done = 0;
    const DWORD want = n > 0x7FFFFFFFu ? 0x7FFFFFFFu : (DWORD)n;
    const BOOL ok = write ? WriteFile(h, buf, want, &done, &ov) : ReadFile(h, buf, want, &done, &ov);
    if (!ok) return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return (ssize_t)done;
}

ssize_t pread(int fd, void *buf, size_t n, long long offset) { return at_offset(fd, buf, n, offset, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, long long offset) {
    return at_offset(fd, const_cast<void *>(buf), n, offset, true);
}
#endif
