// RECOMP_RDOC_FRAME=<n>: a RenderDoc capture of the GPU work between frame n's and frame n+1's presentation (Windows),
// for looking at a picture draw by draw where the GPU renderer and the software one differ (Core of Light #168).
// RenderDoc's Vulkan layer has to be in the process: start with VK_ADD_IMPLICIT_LAYER_PATH=<RenderDoc's folder> and
// ENABLE_VULKAN_RENDERDOC_CAPTURE=1 (no registration needed); RECOMP_RDOC_OUT=<path template> names the capture.
// Built only when RenderDoc's in-app API header (renderdoc_app.h, MIT) has been copied to external/ (not in the
// repository); otherwise this does nothing.
#include "rdoc_capture.h"

#if defined(_WIN32) && __has_include("../external/renderdoc_app.h")
#include <windows.h>

#include <cstdlib>

#include "../external/renderdoc_app.h"
#include "rt.h"

void rdoc_frame(unsigned long long frame) {
    static const char *want = getenv("RECOMP_RDOC_FRAME");
    if (!want) return;
    static const unsigned long long at = strtoull(want, nullptr, 10);
    if (frame != at && frame != at + 1) return;
    static RENDERDOC_API_1_6_0 *api = [] {
        RENDERDOC_API_1_6_0 *a = nullptr;
        HMODULE m = GetModuleHandleA("renderdoc.dll");
        pRENDERDOC_GetAPI get = m ? (pRENDERDOC_GetAPI)GetProcAddress(m, "RENDERDOC_GetAPI") : nullptr;
        if (!get || !get(eRENDERDOC_API_Version_1_6_0, (void **)&a)) a = nullptr;
        if (!a) LOG("rdoc: RenderDoc isn't loaded (VK_ADD_IMPLICIT_LAYER_PATH, ENABLE_VULKAN_RENDERDOC_CAPTURE=1?)\n");
        if (a && getenv("RECOMP_RDOC_OUT")) a->SetCaptureFilePathTemplate(getenv("RECOMP_RDOC_OUT"));
        return a;
    }();
    if (!api) return;
    if (frame == at) {
        api->StartFrameCapture(nullptr, nullptr);
        LOG("rdoc: capturing frame %llu\n", frame);
    } else {
        const uint32_t ok = api->EndFrameCapture(nullptr, nullptr);
        LOG("rdoc: capture %s\n", ok ? "written" : "FAILED");
    }
}
#else
void rdoc_frame(unsigned long long) {}
#endif
