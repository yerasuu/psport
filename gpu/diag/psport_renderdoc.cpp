// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_renderdoc.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <renderdoc_app.h>

namespace PsRenderDoc {
namespace {
std::uint64_t ReadCaptureFrame() {
    const char* sz_frame = std::getenv("PS_RENDERDOC_CAPTURE_FRAME");
    if (!sz_frame || !*sz_frame) {
        return 0;
    }
    char* p_end = nullptr;
    const unsigned long long frame = std::strtoull(sz_frame, &p_end, 10);
    if (*p_end || frame == 0) {
        std::printf("RenderDoc: PS_RENDERDOC_CAPTURE_FRAME=%s is not a frame number, no capture\n",
                    sz_frame);
        return 0;
    }
    return frame;
}

RENDERDOC_API_1_1_2* LoadApi() {
#ifdef _WIN32
    std::printf("RenderDoc: PS_RENDERDOC_CAPTURE_FRAME is not supported on Windows yet, "
                "no capture\n");
    return nullptr;
#else
    // RTLD_NOLOAD: RenderDoc must hook the process from its start to capture it; loading it
    // now would only add a library that sees none of the Vulkan objects already created.
    void* h_module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h_module) {
        std::printf("RenderDoc: PS_RENDERDOC_CAPTURE_FRAME is set but RenderDoc is not loaded "
                    "(start under renderdoccmd capture), no capture\n");
        return nullptr;
    }
    const auto pfn_get_api =
        reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(h_module, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_1_2* p_api = nullptr;
    if (!pfn_get_api ||
        pfn_get_api(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void**>(&p_api)) != 1) {
        std::printf("RenderDoc: its API 1.1.2 is unavailable, no capture\n");
        return nullptr;
    }
    if (const char* sz_template = std::getenv("PS_RENDERDOC_CAPTURE_PATH")) {
        p_api->SetCaptureFilePathTemplate(sz_template);
    }
    return p_api;
#endif
}
} // namespace

void OnPresent() {
    static const std::uint64_t s_i_capture_frame = ReadCaptureFrame();
    if (!s_i_capture_frame) {
        return;
    }
    static RENDERDOC_API_1_1_2* const s_p_api = LoadApi();
    static std::atomic<std::uint64_t> s_c_presented{0};
    static std::atomic<std::uint32_t> s_c_captures_before{UINT32_MAX};
    if (!s_p_api) {
        return;
    }
    const std::uint64_t c_presented = s_c_presented.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c_presented == s_i_capture_frame) {
        s_c_captures_before.store(s_p_api->GetNumCaptures(), std::memory_order_relaxed);
        s_p_api->TriggerCapture();
        std::printf("RenderDoc: capture triggered after game frame %llu\n",
                    static_cast<unsigned long long>(c_presented));
        return;
    }
    const std::uint32_t c_before = s_c_captures_before.load(std::memory_order_relaxed);
    if (c_before == UINT32_MAX || s_p_api->GetNumCaptures() <= c_before) {
        return;
    }
    s_c_captures_before.store(UINT32_MAX, std::memory_order_relaxed);
    // GetCapture writes the whole path without a buffer size: its length is asked first.
    char sz_path[4096];
    std::uint32_t cb_path = 0;
    if (s_p_api->GetCapture(c_before, nullptr, &cb_path, nullptr) && cb_path < sizeof(sz_path) &&
        s_p_api->GetCapture(c_before, sz_path, nullptr, nullptr)) {
        sz_path[cb_path] = '\0';
        std::printf("RenderDoc: frame captured to %s\n", sz_path);
    } else {
        std::printf("RenderDoc: frame captured (path unavailable)\n");
    }
}
} // namespace PsRenderDoc
