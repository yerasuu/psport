// SPDX-License-Identifier: GPL-2.0-or-later
// psport: fsr4cap.exe — runs AMD's FSR 4 upscaler DLL through the FidelityFX API on D3D12 (under
// Wine/Proton with vkd3d-proton) on synthetic inputs, for recording what it does (docs/en/upscaler.md,
// FSR 4.1.1). Step 1: list the upscaler versions, create a context with a chosen version, run
// frames, read back the output.
//
//   fsr4cap.exe [version substring, default FSR4] [render WxH] [output WxH] [frames] [noise]
// noise: the benchmark's pseudo-random inputs (tools/fsr4_bench.cpp, BENCH_NOISE) and the output
// after the last frame in output_<output WxH>.raw (RGBA16F rows), for comparing the replay.
// The upscaler DLL is next to the exe (and, for a DLL not exporting the API, the loader
// amd_fidelityfx_loader_dx12.dll).
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#define _WINDOWS
#include <windows.h>
#include <d3d12.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"
#include "capture.h"

#define CHECK(x)                                                                                  \
    do {                                                                                          \
        HRESULT hr_ = (x);                                                                        \
        if (FAILED(hr_)) {                                                                        \
            fprintf(stderr, "%s failed: 0x%08lx (line %d)\n", #x, (unsigned long)hr_, __LINE__);  \
            exit(1);                                                                              \
        }                                                                                         \
    } while (0)

static ID3D12Device* s_p_device;
static ID3D12CommandQueue* s_p_queue;
static ID3D12CommandAllocator* s_p_allocator;
static ID3D12GraphicsCommandList* s_p_list;
static ID3D12Fence* s_p_fence;
static UINT64 s_fence_value;
static HANDLE s_h_fence_event;

static void Submit(void) {
    CHECK(ID3D12GraphicsCommandList_Close(s_p_list));
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)s_p_list};
    ID3D12CommandQueue_ExecuteCommandLists(s_p_queue, 1, lists);
    CHECK(ID3D12CommandQueue_Signal(s_p_queue, s_p_fence, ++s_fence_value));
    CHECK(ID3D12Fence_SetEventOnCompletion(s_p_fence, s_fence_value, s_h_fence_event));
    WaitForSingleObject(s_h_fence_event, INFINITE);
    CHECK(ID3D12CommandAllocator_Reset(s_p_allocator));
    CHECK(ID3D12GraphicsCommandList_Reset(s_p_list, s_p_allocator, NULL));
}

static ID3D12Resource* Texture(DXGI_FORMAT format, UINT w, UINT h, bool is_uav) {
    D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = w,
        .Height = h,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = format,
        .SampleDesc = {1, 0},
        .Flags = is_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE,
    };
    ID3D12Resource* p_resource;
    CHECK(ID3D12Device_CreateCommittedResource(s_p_device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COMMON, NULL,
                                               &IID_ID3D12Resource, (void**)&p_resource));
    return p_resource;
}

static struct FfxApiResource Resource(ID3D12Resource* p_resource, uint32_t format, uint32_t state) {
    D3D12_RESOURCE_DESC desc = ID3D12Resource_GetDesc(p_resource);
    struct FfxApiResource out = {0};
    out.resource = p_resource;
    out.state = state;
    out.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    out.description.format = format;
    out.description.width = (uint32_t)desc.Width;
    out.description.height = desc.Height;
    out.description.depth = 1;
    out.description.mipCount = 1;
    out.description.usage = state == FFX_API_RESOURCE_STATE_UNORDERED_ACCESS ? FFX_API_RESOURCE_USAGE_UAV : 0;
    return out;
}

// The benchmark's test data generator (tools/fsr4_bench.cpp): same seed, order and conversion.
static uint16_t Half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = (int)((x >> 23) & 0xffu) - 127 + 15;
    if (exp <= 0) return (uint16_t)sign;
    return (uint16_t)(sign | ((uint32_t)exp << 10) | ((x >> 13) & 0x3ffu));
}

static uint32_t s_seed = 12345u;
static float Next(void) {
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) / (float)(1u << 24);
}

static ID3D12Resource* Buffer(D3D12_HEAP_TYPE type, UINT64 cb_buffer) {
    D3D12_HEAP_PROPERTIES heap = {.Type = type};
    D3D12_RESOURCE_DESC desc = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = cb_buffer, .Height = 1,
                                .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
                                .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ID3D12Resource* p_resource;
    CHECK(ID3D12Device_CreateCommittedResource(s_p_device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                              : D3D12_RESOURCE_STATE_COPY_DEST,
                                               NULL, &IID_ID3D12Resource, (void**)&p_resource));
    return p_resource;
}

// Fills `p_tex` (w x h, bpp bytes per texel) row by row from `pfn_fill`, through an upload buffer.
static void Upload(ID3D12Resource* p_tex, UINT w, UINT h, UINT bpp, void (*pfn_fill)(uint8_t* p_row, UINT w)) {
    const UINT pitch = (w * bpp + 255) & ~255u;
    ID3D12Resource* p_upload = Buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)pitch * h);
    uint8_t* p_data;
    CHECK(ID3D12Resource_Map(p_upload, 0, NULL, (void**)&p_data));
    for (UINT y = 0; y < h; ++y) pfn_fill(p_data + (size_t)y * pitch, w);
    ID3D12Resource_Unmap(p_upload, 0, NULL);
    D3D12_RESOURCE_DESC desc = ID3D12Resource_GetDesc(p_tex);
    D3D12_TEXTURE_COPY_LOCATION dst = {.pResource = p_tex, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src = {.pResource = p_upload, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Footprint.Format = desc.Format;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    ID3D12GraphicsCommandList_CopyTextureRegion(s_p_list, &dst, 0, 0, 0, &src, NULL);
    Submit();
    ID3D12Resource_Release(p_upload);
}

static void FillColor(uint8_t* p_row, UINT w) {
    uint16_t* p_pixel = (uint16_t*)p_row;
    for (UINT x = 0; x < w; ++x) {
        p_pixel[4 * x + 0] = Half(Next() * 1.5f);
        p_pixel[4 * x + 1] = Half(Next() * 1.5f);
        p_pixel[4 * x + 2] = Half(Next() * 1.5f);
        p_pixel[4 * x + 3] = Half(1.0f);
    }
}
static void FillMotion(uint8_t* p_row, UINT w) {
    uint16_t* p_pixel = (uint16_t*)p_row;
    for (UINT x = 0; x < 2 * w; ++x) p_pixel[x] = Half(Next() * 4.0f - 2.0f);
}
static void FillDepth(uint8_t* p_row, UINT w) {
    float* p_pixel = (float*)p_row;
    for (UINT x = 0; x < w; ++x) p_pixel[x] = 0.9f + Next() * 0.1f;
}

static void Message(uint32_t type, const wchar_t* sz_message) {
    fprintf(stderr, "FFX %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", sz_message);
}

int main(int argc, char** argv) {
    // Proton does not pass the console through: log next to the exe.
    char sz_log_path[MAX_PATH];
    GetModuleFileNameA(NULL, sz_log_path, MAX_PATH);
    strcpy(strrchr(sz_log_path, '\\') + 1, "fsr4cap.log");
    freopen(sz_log_path, "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);
    *stderr = *stdout;
    const char* sz_want = argc > 1 ? argv[1] : "FSR4";
    uint32_t rw = 1280, rh = 720, ow = 1920, oh = 1080;
    int frames = argc > 4 ? atoi(argv[4]) : 4;
    if (argc > 2) sscanf(argv[2], "%ux%u", &rw, &rh);
    if (argc > 3) sscanf(argv[3], "%ux%u", &ow, &oh);

    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void**)&s_p_device));
    D3D12_COMMAND_QUEUE_DESC qd = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(ID3D12Device_CreateCommandQueue(s_p_device, &qd, &IID_ID3D12CommandQueue, (void**)&s_p_queue));
    CHECK(ID3D12Device_CreateCommandAllocator(s_p_device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              &IID_ID3D12CommandAllocator, (void**)&s_p_allocator));
    CHECK(ID3D12Device_CreateCommandList(s_p_device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_p_allocator, NULL,
                                         &IID_ID3D12GraphicsCommandList, (void**)&s_p_list));
    CHECK(ID3D12Device_CreateFence(s_p_device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&s_p_fence));
    s_h_fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    // Record everything the upscaler does with the device (capture_<sizes>/ next to the exe, or in
    // the folder FSR4CAP_SUBDIR names there: fp8/ for the DLL's FP8 matrix variant).
    char sz_capture_dir[MAX_PATH];
    GetModuleFileNameA(NULL, sz_capture_dir, MAX_PATH);
    char* sz_name = strrchr(sz_capture_dir, '\\') + 1;
    const char* sz_subdir = getenv("FSR4CAP_SUBDIR");
    if (sz_subdir && sz_subdir[0]) {
        snprintf(sz_name, 64, "%s", sz_subdir);
        CreateDirectoryA(sz_capture_dir, NULL);
        sz_name += strlen(sz_name);
        *sz_name++ = '\\';
    }
    // Test runs with pseudo-random inputs (verify.sh) record next to, not over, the captures.
    snprintf(sz_name, 64, "%scapture_%ux%u_%ux%u",
             argc > 5 && strcmp(argv[5], "noise") == 0 ? "noise_" : "", rw, rh, ow, oh);
    CaptureInstall(s_p_device, s_p_list, sz_capture_dir);

    // AMD's upscaler DLL exports the FidelityFX API itself; the loader only for one that does not.
    HMODULE h_api = LoadLibraryA("amd_fidelityfx_upscaler_dx12.dll");
    if (!h_api || !GetProcAddress(h_api, "ffxCreateContext")) {
        h_api = LoadLibraryA("amd_fidelityfx_loader_dx12.dll");
    }
    if (!h_api) {
        fprintf(stderr, "cannot load amd_fidelityfx_upscaler_dx12.dll (or a loader)\n");
        return 1;
    }
    ffxFunctions ffx;
    ffxLoadFunctions(&ffx, h_api);

    // Versions of the upscaler.
    uint64_t c_versions = 0;
    struct ffxQueryDescGetVersions versions = {0};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = s_p_device;
    versions.outputCount = &c_versions;
    ffx.Query(NULL, &versions.header);
    uint64_t ids[32];
    const char* names[32];
    if (c_versions > 32) c_versions = 32;
    versions.versionIds = ids;
    versions.versionNames = names;
    ffx.Query(NULL, &versions.header);
    uint64_t chosen = 0;
    for (uint64_t i = 0; i < c_versions; ++i) {
        printf("version %llu: id 0x%llx %s\n", (unsigned long long)i, (unsigned long long)ids[i], names[i]);
        if (!chosen && strstr(names[i], sz_want)) chosen = ids[i];
    }
    if (!chosen) {
        fprintf(stderr, "no version matching '%s'\n", sz_want);
        return 1;
    }

    struct ffxCreateBackendDX12Desc backend = {0};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = s_p_device;
    struct ffxOverrideVersion override = {0};
    override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override.versionId = chosen;
    override.header.pNext = &backend.header;
    struct ffxCreateContextDescUpscale create = {0};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.header.pNext = &override.header;
    create.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    create.maxRenderSize.width = ow;
    create.maxRenderSize.height = oh;
    create.maxUpscaleSize.width = ow;
    create.maxUpscaleSize.height = oh;
    create.fpMessage = Message;
    ffxContext p_context = NULL;
    ffxReturnCode_t rc = ffx.CreateContext(&p_context, &create.header, NULL);
    printf("create context: %d\n", (int)rc);
    if (rc != FFX_API_RETURN_OK) return 1;
    struct ffxQueryGetProviderVersion provider = {0};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    ffx.Query(&p_context, &provider.header);
    printf("provider: %s (0x%llx)\n", provider.versionName ? provider.versionName : "?",
           (unsigned long long)provider.versionId);

    ID3D12Resource* p_color = Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, rw, rh, false);
    ID3D12Resource* p_depth = Texture(DXGI_FORMAT_R32_FLOAT, rw, rh, false);
    ID3D12Resource* p_motion = Texture(DXGI_FORMAT_R16G16_FLOAT, rw, rh, false);
    ID3D12Resource* p_output = Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, ow, oh, true);
    CaptureNoteResource(p_color, "color");
    CaptureNoteResource(p_depth, "depth");
    CaptureNoteResource(p_motion, "motion");
    CaptureNoteResource(p_output, "output");
    const bool is_noise = argc > 5 && strcmp(argv[5], "noise") == 0;
    if (is_noise) {
        Upload(p_color, rw, rh, 8, FillColor);
        Upload(p_motion, rw, rh, 4, FillMotion);
        Upload(p_depth, rw, rh, 4, FillDepth);
    }
    for (int frame = 0; frame < frames; ++frame) {
        char sz_mark[32];
        snprintf(sz_mark, sizeof(sz_mark), "frame %d", frame);
        CaptureMark(sz_mark);
        struct ffxDispatchDescUpscale d = {0};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = s_p_list;
        d.color = Resource(p_color, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_STATE_COMMON);
        d.depth = Resource(p_depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_STATE_COMMON);
        d.motionVectors = Resource(p_motion, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_STATE_COMMON);
        d.output = Resource(p_output, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset.x = 0.25f * (float)(frame % 4) - 0.375f;
        d.jitterOffset.y = 0.125f;
        d.motionVectorScale.x = d.motionVectorScale.y = 1.0f;
        d.renderSize.width = rw;
        d.renderSize.height = rh;
        d.upscaleSize.width = ow;
        d.upscaleSize.height = oh;
        d.enableSharpening = true;
        d.sharpness = 0.5f;
        d.frameTimeDelta = 10.0f;
        d.preExposure = 1.0f;
        d.reset = frame == 0;
        d.cameraNear = 0.05f;
        d.cameraFar = 3000.0f;
        d.cameraFovAngleVertical = 0.75f;
        d.viewSpaceToMetersFactor = 1.0f;
        rc = ffx.Dispatch(&p_context, &d.header);
        if (rc != FFX_API_RETURN_OK) {
            printf("dispatch %d: %d\n", frame, (int)rc);
            return 1;
        }
        Submit();
    }
    CaptureMark("end");
    if (is_noise) {
        const UINT pitch = (ow * 8 + 255) & ~255u;
        ID3D12Resource* p_readback = Buffer(D3D12_HEAP_TYPE_READBACK, (UINT64)pitch * oh);
        D3D12_TEXTURE_COPY_LOCATION src = {.pResource = p_output, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst = {.pResource = p_readback, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dst.PlacedFootprint.Footprint.Width = ow;
        dst.PlacedFootprint.Footprint.Height = oh;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = pitch;
        D3D12_RESOURCE_BARRIER b = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition.pResource = p_output;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(s_p_list, 1, &b);
        ID3D12GraphicsCommandList_CopyTextureRegion(s_p_list, &dst, 0, 0, 0, &src, NULL);
        Submit();
        uint8_t* p_data;
        CHECK(ID3D12Resource_Map(p_readback, 0, NULL, (void**)&p_data));
        char sz_path[MAX_PATH];
        GetModuleFileNameA(NULL, sz_path, MAX_PATH);
        snprintf(strrchr(sz_path, '\\') + 1, 64, "output_%ux%u.raw", ow, oh);
        FILE* p_file = fopen(sz_path, "wb");
        for (UINT y = 0; p_file && y < oh; ++y) fwrite(p_data + (size_t)y * pitch, 1, (size_t)ow * 8, p_file);
        if (p_file) fclose(p_file);
        printf("output written to %s\n", sz_path);
    }
    printf("%d frames done\n", frames);
    ffx.DestroyContext(&p_context, NULL);
    return 0;
}
