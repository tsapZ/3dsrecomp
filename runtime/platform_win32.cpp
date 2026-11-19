// runtime/platform.h on Windows (MinGW-w64 with clang, e.g. MSYS2's CLANG64: pthreads come from winpthreads).
#include "platform.h"

#include <SDL3/SDL.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "uiview.h"

namespace platform {

static std::string utf8(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string slashes(std::string s) {
    for (char &c : s)
        if (c == '\\') c = '/';
    return s;
}

std::string exe_path() {
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    return n > 0 && n < sizeof buf / sizeof buf[0] ? slashes(utf8(buf)) : "";
}

std::string app_data_dir() {
    wchar_t *path = nullptr;
    std::string out;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path) == S_OK) out = slashes(utf8(path));
    CoTaskMemFree(path);
    return (out.empty() ? std::string(".") : out) + "/3dsrecomp";
}

const char *module_suffix() { return ".dll"; }

void open_folder(const std::string &path) { SDL_OpenURL(("file:///" + path).c_str()); }

std::string device_name() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &n) || !name[0]) return "Windows";
    return utf8(name);
}

void set_thread_interactive() { SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH); }

void set_thread_name(const char *name) {
    wchar_t w[64];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64);
    w[63] = 0;
    SetThreadDescription(GetCurrentThread(), w);
}

// (not measured on Windows yet)
std::string thread_cpu_report(double) { return ""; }

// Thread sampling (thread dumps): not on Windows yet. A real one would SuspendThread + GetThreadContext +
// RtlVirtualUnwind; the dumps then show no guest functions, which nothing depends on.
uint64_t thread_handle(pthread_t) { return 0; }
bool is_current_thread(uint64_t) { return false; }
int sample_thread_pcs(uint64_t, uint64_t *, int) { return 0; }

bool can_single_step() { return false; }
void set_single_step(void *, bool) {}
uint64_t fault_pc(void *) { return 0; }

}  // namespace platform

// (iOS only: runtime/uiview.mm)
namespace uiview {
bool keep_filling(SDL_Renderer *) { return false; }
}  // namespace uiview
