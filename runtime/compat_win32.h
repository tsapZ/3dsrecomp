// Windows (MinGW-w64): the few POSIX calls the runtime uses that MinGW lacks. CMakeLists.txt force-includes this
// header into the runtime's C++ files on WIN32 only; the definitions are in runtime/compat_win32.cpp. Kept free of
// <windows.h> (its macros clash with portable code).
#pragma once
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/types.h>

#include <cstdlib>
#include <ctime>

inline struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) == 0 ? out : nullptr; }
inline struct tm *gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) == 0 ? out : nullptr; }

inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
inline int unsetenv(const char *name) { return _putenv_s(name, "") == 0 ? 0 : -1; }

inline int fsync(int fd) { return _commit(fd); }

// POSIX mkdir takes a mode; Windows has no permission bits to give
inline int mkdir(const char *path, int) { return _mkdir(path); }

// reads and writes at an offset without moving the file position (thread-safe, as on POSIX)
ssize_t pread(int fd, void *buf, size_t n, long long offset);
ssize_t pwrite(int fd, const void *buf, size_t n, long long offset);
#endif
