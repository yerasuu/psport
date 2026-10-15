// SPDX-License-Identifier: GPL-2.0-or-later
// psport: D3D12 call recorder for fsr4cap.exe. Patches the vtables of the device, command list
// and resource classes (vkd3d-proton shares one vtable per class across interface versions) and
// writes every compute dispatch with its pipeline, root parameters and the descriptors they
// resolve to into capture/trace.txt; shaders (DXIL), root signatures and buffer uploads go to
// capture/ as files.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture.h"

static FILE* s_p_trace;
static char s_sz_dir[MAX_PATH];
static int s_c_dispatches;

static uint64_t Fnv(const void* p_data, size_t cb_data) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < cb_data; ++i) h = (h ^ ((const uint8_t*)p_data)[i]) * 1099511628211ull;
    return h;
}

static void WriteFile2(const char* sz_name, const void* p_data, size_t cb_data) {
    char sz_path[2 * MAX_PATH]; // dir and a name: no truncation (-Wformat-truncation)
    snprintf(sz_path, sizeof(sz_path), "%s\\%s", s_sz_dir, sz_name);
    FILE* p_file = fopen(sz_path, "rb");
    if (p_file) { fclose(p_file); return; } // content-addressed: already there
    p_file = fopen(sz_path, "wb");
    if (p_file) { fwrite(p_data, 1, cb_data, p_file); fclose(p_file); }
}

// ---- resources ---------------------------------------------------------------------------
typedef struct { ID3D12Resource* p_resource; int id; D3D12_RESOURCE_DESC desc; D3D12_HEAP_TYPE heap;
                 D3D12_GPU_VIRTUAL_ADDRESS va_gpu; } ResInfo;
static ResInfo s_resources[4096];
static int s_c_resources;

typedef D3D12_GPU_VIRTUAL_ADDRESS (STDMETHODCALLTYPE* PfnGetGpuVa)(ID3D12Resource*);
static void* s_p_real_get_va;

static ResInfo* Res(ID3D12Resource* p_resource) {
    if (!p_resource) return NULL;
    for (int i = 0; i < s_c_resources; ++i) if (s_resources[i].p_resource == p_resource) return &s_resources[i];
    if (s_c_resources == 4096) return NULL;
    ResInfo* p_info = &s_resources[s_c_resources];
    p_info->p_resource = p_resource;
    p_info->id = s_c_resources++;
    p_info->desc = ID3D12Resource_GetDesc(p_resource);
    D3D12_HEAP_PROPERTIES props = {0};
    D3D12_HEAP_FLAGS flags;
    p_info->heap = SUCCEEDED(ID3D12Resource_GetHeapProperties(p_resource, &props, &flags)) ? props.Type : 0;
    p_info->va_gpu = p_info->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                   ? (s_p_real_get_va ? ((PfnGetGpuVa)s_p_real_get_va)(p_resource) : ID3D12Resource_GetGPUVirtualAddress(p_resource)) : 0;
    fprintf(s_p_trace, "RESOURCE r%d dim=%d %llux%ux%u mips=%u format=%d flags=0x%x heap=%d va=0x%llx\n",
            p_info->id, p_info->desc.Dimension, (unsigned long long)p_info->desc.Width, p_info->desc.Height,
            p_info->desc.DepthOrArraySize, p_info->desc.MipLevels, p_info->desc.Format, p_info->desc.Flags,
            p_info->heap, (unsigned long long)p_info->va_gpu);
    return p_info;
}

static ResInfo* ResByVa(D3D12_GPU_VIRTUAL_ADDRESS va_gpu, uint64_t* p_out_offset) {
    for (int i = 0; i < s_c_resources; ++i) {
        ResInfo* p_info = &s_resources[i];
        if (p_info->va_gpu && va_gpu >= p_info->va_gpu && va_gpu < p_info->va_gpu + p_info->desc.Width) { *p_out_offset = va_gpu - p_info->va_gpu; return p_info; }
    }
    return NULL;
}

// Reads `size` bytes of an upload/readback buffer at `offset` (CPU visible), or null.
static const void* MapRead(ResInfo* p_info, uint64_t off_data, uint64_t cb_data) {
    if (!p_info || (p_info->heap != D3D12_HEAP_TYPE_UPLOAD && p_info->heap != D3D12_HEAP_TYPE_READBACK)) return NULL;
    if (off_data >= p_info->desc.Width) return NULL;
    void* p_data = NULL;
    if (FAILED(ID3D12Resource_Map(p_info->p_resource, 0, NULL, &p_data)) || !p_data) return NULL;
    ID3D12Resource_Unmap(p_info->p_resource, 0, NULL);
    return (const uint8_t*)p_data + off_data;
}

static void DumpBytes(const char* sz_what, ResInfo* p_info, uint64_t off_data, uint64_t cb_data) {
    if (p_info && p_info->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && off_data < p_info->desc.Width && off_data + cb_data > p_info->desc.Width)
        cb_data = p_info->desc.Width - off_data; // a footprint's last row is shorter than its pitch
    const void* p_data = MapRead(p_info, off_data, cb_data);
    if (!p_data) { fprintf(s_p_trace, " %s=(not CPU visible)", sz_what); return; }
    char sz_name[64];
    const uint64_t h = Fnv(p_data, cb_data);
    snprintf(sz_name, sizeof(sz_name), "data_%016llx.bin", (unsigned long long)h);
    WriteFile2(sz_name, p_data, cb_data);
    fprintf(s_p_trace, " %s=%s", sz_what, sz_name);
}

// ---- descriptors ---------------------------------------------------------------------------
typedef struct { int kind; ResInfo* p_resource; uint64_t a, b; int format; char sz_text[160]; } View;
enum { VIEW_NONE, VIEW_CBV, VIEW_SRV, VIEW_UAV, VIEW_SAMPLER };
typedef struct { SIZE_T va_cpu; UINT64 va_gpu; UINT c_views, cb_increment; View* p_views; } Heap;
static Heap s_heaps[64];
static int s_c_heaps;

static View* ViewAtCpu(SIZE_T va_cpu) {
    for (int i = 0; i < s_c_heaps; ++i) {
        Heap* p_heap = &s_heaps[i];
        if (va_cpu >= p_heap->va_cpu && va_cpu < p_heap->va_cpu + (SIZE_T)p_heap->c_views * p_heap->cb_increment) return &p_heap->p_views[(va_cpu - p_heap->va_cpu) / p_heap->cb_increment];
    }
    return NULL;
}

static View* ViewAtGpu(UINT64 va_gpu, Heap** pp_out_heap) {
    for (int i = 0; i < s_c_heaps; ++i) {
        Heap* p_heap = &s_heaps[i];
        if (p_heap->va_gpu && va_gpu >= p_heap->va_gpu && va_gpu < p_heap->va_gpu + (UINT64)p_heap->c_views * p_heap->cb_increment) {
            *pp_out_heap = p_heap;
            return &p_heap->p_views[(va_gpu - p_heap->va_gpu) / p_heap->cb_increment];
        }
    }
    return NULL;
}

// ---- pipelines and root signatures ----------------------------------------------------------
typedef struct { void* p_object; uint64_t hash; void* p_root; } Pso;
static Pso s_psos[1024];
static int s_c_psos;
typedef struct { void* p_object; uint64_t hash; } Root;
static Root s_roots[256];
static int s_c_roots;

static uint64_t RootHash(void* p_object) {
    for (int i = 0; i < s_c_roots; ++i) if (s_roots[i].p_object == p_object) return s_roots[i].hash;
    return 0;
}

// ---- command list state ---------------------------------------------------------------------
typedef struct { int type; UINT64 value; UINT32 consts[64]; UINT c_consts; } RootArg;
enum { ARG_NONE, ARG_TABLE, ARG_CBV, ARG_SRV, ARG_UAV, ARG_CONSTS };
typedef struct { void* p_list; void* p_root; void* p_pso; RootArg args[32]; } ListState;
static ListState s_lists[16];

static ListState* State(void* p_list) {
    for (int i = 0; i < 16; ++i) if (s_lists[i].p_list == p_list) return &s_lists[i];
    for (int i = 0; i < 16; ++i) if (!s_lists[i].p_list) { s_lists[i].p_list = p_list; return &s_lists[i]; }
    return &s_lists[0];
}

// ---- hooks ------------------------------------------------------------------------------------
#define SLOT(vtbl, method) (offsetof(vtbl, method) / sizeof(void*))
static void** s_pp_device_vtbl;
static void** s_pp_list_vtbl;
static void* s_real[512];
#define REAL(fn_type, slot) ((fn_type)s_real[slot])

static void Patch(void** pp_vtbl, size_t slot, void* p_hook, int store) {
    DWORD old;
    VirtualProtect(&pp_vtbl[slot], sizeof(void*), PAGE_READWRITE, &old);
    s_real[store] = pp_vtbl[slot];
    pp_vtbl[slot] = p_hook;
    VirtualProtect(&pp_vtbl[slot], sizeof(void*), old, &old);
}

// Device hooks use store slots 0..99, command list 100..299, resource 300..
typedef HRESULT (STDMETHODCALLTYPE* PfnCreateComputePipelineState)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
static HRESULT STDMETHODCALLTYPE HookCreateComputePipelineState(ID3D12Device* This, const D3D12_COMPUTE_PIPELINE_STATE_DESC* p_desc, REFIID p_riid, void** pp_out) {
    HRESULT hr = REAL(PfnCreateComputePipelineState, 0)(This, p_desc, p_riid, pp_out);
    if (SUCCEEDED(hr) && pp_out && *pp_out && s_c_psos < 1024) {
        const uint64_t h = Fnv(p_desc->CS.pShaderBytecode, p_desc->CS.BytecodeLength);
        char sz_name[64];
        snprintf(sz_name, sizeof(sz_name), "cs_%016llx.dxil", (unsigned long long)h);
        WriteFile2(sz_name, p_desc->CS.pShaderBytecode, p_desc->CS.BytecodeLength);
        s_psos[s_c_psos++] = (Pso){*pp_out, h, p_desc->pRootSignature};
        fprintf(s_p_trace, "PSO %p cs=%016llx root=%016llx\n", *pp_out, (unsigned long long)h,
                (unsigned long long)RootHash(p_desc->pRootSignature));
    }
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE* PfnCreateRootSignature)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
static HRESULT STDMETHODCALLTYPE HookCreateRootSignature(ID3D12Device* This, UINT mask, const void* p_blob, SIZE_T cb_blob, REFIID p_riid, void** pp_out) {
    HRESULT hr = REAL(PfnCreateRootSignature, 1)(This, mask, p_blob, cb_blob, p_riid, pp_out);
    if (SUCCEEDED(hr) && pp_out && *pp_out && s_c_roots < 256) {
        const uint64_t h = Fnv(p_blob, cb_blob);
        char sz_name[64];
        snprintf(sz_name, sizeof(sz_name), "root_%016llx.bin", (unsigned long long)h);
        WriteFile2(sz_name, p_blob, cb_blob);
        s_roots[s_c_roots++] = (Root){*pp_out, h};
        fprintf(s_p_trace, "ROOTSIG %p %016llx\n", *pp_out, (unsigned long long)h);
        CaptureDescribeRootSignature(s_p_trace, p_blob, cb_blob);
    }
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE* PfnCreateDescriptorHeap)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
static HRESULT STDMETHODCALLTYPE HookCreateDescriptorHeap(ID3D12Device* This, const D3D12_DESCRIPTOR_HEAP_DESC* p_desc, REFIID p_riid, void** pp_out) {
    HRESULT hr = REAL(PfnCreateDescriptorHeap, 2)(This, p_desc, p_riid, pp_out);
    if (SUCCEEDED(hr) && pp_out && *pp_out && s_c_heaps < 64) {
        ID3D12DescriptorHeap* p_descriptor_heap = *pp_out;
        Heap* p_heap = &s_heaps[s_c_heaps++];
        p_heap->va_cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(p_descriptor_heap).ptr;
        p_heap->va_gpu = (p_desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
                        ? ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(p_descriptor_heap).ptr : 0;
        p_heap->c_views = p_desc->NumDescriptors;
        p_heap->cb_increment = ID3D12Device_GetDescriptorHandleIncrementSize(This, p_desc->Type);
        p_heap->p_views = calloc(p_desc->NumDescriptors, sizeof(View));
        fprintf(s_p_trace, "HEAP type=%d count=%u visible=%d\n", p_desc->Type, p_desc->NumDescriptors, p_heap->va_gpu != 0);
    }
    return hr;
}

typedef void (STDMETHODCALLTYPE* PfnCreateCBV)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static void STDMETHODCALLTYPE HookCreateCBV(ID3D12Device* This, const D3D12_CONSTANT_BUFFER_VIEW_DESC* p_desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    REAL(PfnCreateCBV, 3)(This, p_desc, h);
    View* p_view = ViewAtCpu(h.ptr);
    if (!p_view || !p_desc) return;
    uint64_t off_buffer = 0;
    *p_view = (View){VIEW_CBV, ResByVa(p_desc->BufferLocation, &off_buffer), off_buffer, p_desc->SizeInBytes, 0, ""};
    snprintf(p_view->sz_text, sizeof(p_view->sz_text), "CBV r%d+%llu size %u", p_view->p_resource ? p_view->p_resource->id : -1, (unsigned long long)off_buffer, p_desc->SizeInBytes);
}

typedef void (STDMETHODCALLTYPE* PfnCreateSRV)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static void STDMETHODCALLTYPE HookCreateSRV(ID3D12Device* This, ID3D12Resource* p_res, const D3D12_SHADER_RESOURCE_VIEW_DESC* p_desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    REAL(PfnCreateSRV, 4)(This, p_res, p_desc, h);
    View* p_view = ViewAtCpu(h.ptr);
    if (!p_view) return;
    ResInfo* p_info = Res(p_res);
    *p_view = (View){VIEW_SRV, p_info, 0, 0, p_desc ? p_desc->Format : -1, ""};
    if (p_desc && p_desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
        snprintf(p_view->sz_text, sizeof(p_view->sz_text), "SRV r%d buffer first=%llu num=%u stride=%u format=%d flags=%d", p_info ? p_info->id : -1,
                 (unsigned long long)p_desc->Buffer.FirstElement, p_desc->Buffer.NumElements, p_desc->Buffer.StructureByteStride, p_desc->Format, p_desc->Buffer.Flags);
    else
        snprintf(p_view->sz_text, sizeof(p_view->sz_text), "SRV r%d dim=%d format=%d", p_info ? p_info->id : -1, p_desc ? p_desc->ViewDimension : -1, p_desc ? p_desc->Format : -1);
}

typedef void (STDMETHODCALLTYPE* PfnCreateUAV)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static void STDMETHODCALLTYPE HookCreateUAV(ID3D12Device* This, ID3D12Resource* p_res, ID3D12Resource* p_counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* p_desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    REAL(PfnCreateUAV, 5)(This, p_res, p_counter, p_desc, h);
    View* p_view = ViewAtCpu(h.ptr);
    if (!p_view) return;
    ResInfo* p_info = Res(p_res);
    *p_view = (View){VIEW_UAV, p_info, 0, 0, p_desc ? p_desc->Format : -1, ""};
    if (p_desc && p_desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
        snprintf(p_view->sz_text, sizeof(p_view->sz_text), "UAV r%d buffer first=%llu num=%u stride=%u format=%d flags=%d", p_info ? p_info->id : -1,
                 (unsigned long long)p_desc->Buffer.FirstElement, p_desc->Buffer.NumElements, p_desc->Buffer.StructureByteStride, p_desc->Format, p_desc->Buffer.Flags);
    else
        snprintf(p_view->sz_text, sizeof(p_view->sz_text), "UAV r%d dim=%d format=%d mip=%u", p_info ? p_info->id : -1, p_desc ? p_desc->ViewDimension : -1,
                 p_desc ? p_desc->Format : -1, p_desc && p_desc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D ? p_desc->Texture2D.MipSlice : 0);
}

typedef void (STDMETHODCALLTYPE* PfnCreateSampler)(ID3D12Device*, const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static void STDMETHODCALLTYPE HookCreateSampler(ID3D12Device* This, const D3D12_SAMPLER_DESC* p_desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    REAL(PfnCreateSampler, 6)(This, p_desc, h);
    View* p_view = ViewAtCpu(h.ptr);
    if (!p_view || !p_desc) return;
    *p_view = (View){VIEW_SAMPLER, NULL, 0, 0, 0, ""};
    snprintf(p_view->sz_text, sizeof(p_view->sz_text), "SAMPLER filter=0x%x address=%d,%d,%d", p_desc->Filter, p_desc->AddressU, p_desc->AddressV, p_desc->AddressW);
}

typedef void (STDMETHODCALLTYPE* PfnCopyDescriptorsSimple)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
static void STDMETHODCALLTYPE HookCopyDescriptorsSimple(ID3D12Device* This, UINT c_descriptors, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    REAL(PfnCopyDescriptorsSimple, 7)(This, c_descriptors, dst, src, type);
    const UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(This, type);
    for (UINT i = 0; i < c_descriptors; ++i) {
        View* p_source = ViewAtCpu(src.ptr + (SIZE_T)i * inc);
        View* p_destination = ViewAtCpu(dst.ptr + (SIZE_T)i * inc);
        if (p_source && p_destination) *p_destination = *p_source;
    }
}

typedef void (STDMETHODCALLTYPE* PfnCopyDescriptors)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
static void STDMETHODCALLTYPE HookCopyDescriptors(ID3D12Device* This, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* p_dsts, const UINT* p_dsizes, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* p_srcs, const UINT* p_ssizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    REAL(PfnCopyDescriptors, 8)(This, nd, p_dsts, p_dsizes, ns, p_srcs, p_ssizes, type);
    const UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(This, type);
    UINT di = 0, dk = 0;
    for (UINT si = 0; si < ns; ++si) {
        const UINT scount = p_ssizes ? p_ssizes[si] : 1;
        for (UINT k = 0; k < scount && di < nd; ++k) {
            View* p_source = ViewAtCpu(p_srcs[si].ptr + (SIZE_T)k * inc);
            View* p_destination = ViewAtCpu(p_dsts[di].ptr + (SIZE_T)dk * inc);
            if (p_source && p_destination) *p_destination = *p_source;
            if (++dk == (p_dsizes ? p_dsizes[di] : 1)) { dk = 0; ++di; }
        }
    }
}

// Command list.
typedef void (STDMETHODCALLTYPE* PfnSetPtr)(ID3D12GraphicsCommandList*, void*);
static void STDMETHODCALLTYPE HookSetComputeRootSignature(ID3D12GraphicsCommandList* This, void* p_rs) {
    State(This)->p_root = p_rs;
    memset(State(This)->args, 0, sizeof(State(This)->args));
    REAL(PfnSetPtr, 100)(This, p_rs);
}
static void STDMETHODCALLTYPE HookSetPipelineState(ID3D12GraphicsCommandList* This, void* p_pso) {
    State(This)->p_pso = p_pso;
    REAL(PfnSetPtr, 101)(This, p_pso);
}
typedef void (STDMETHODCALLTYPE* PfnSetTable)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
static void STDMETHODCALLTYPE HookSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (i < 32) State(This)->args[i] = (RootArg){ARG_TABLE, h.ptr};
    REAL(PfnSetTable, 102)(This, i, h);
}
typedef void (STDMETHODCALLTYPE* PfnSet32)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
static void STDMETHODCALLTYPE HookSetComputeRoot32BitConstant(ID3D12GraphicsCommandList* This, UINT i, UINT v, UINT off_dw_value) {
    if (i < 32 && off_dw_value < 64) {
        RootArg* p_arg = &State(This)->args[i];
        p_arg->type = ARG_CONSTS;
        p_arg->consts[off_dw_value] = v;
        if (off_dw_value + 1 > p_arg->c_consts) p_arg->c_consts = off_dw_value + 1;
    }
    REAL(PfnSet32, 103)(This, i, v, off_dw_value);
}
typedef void (STDMETHODCALLTYPE* PfnSet32s)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
static void STDMETHODCALLTYPE HookSetComputeRoot32BitConstants(ID3D12GraphicsCommandList* This, UINT i, UINT c_values, const void* p_values, UINT off_dw_values) {
    if (i < 32 && off_dw_values + c_values <= 64) {
        RootArg* p_arg = &State(This)->args[i];
        p_arg->type = ARG_CONSTS;
        memcpy(&p_arg->consts[off_dw_values], p_values, c_values * 4);
        if (off_dw_values + c_values > p_arg->c_consts) p_arg->c_consts = off_dw_values + c_values;
    }
    REAL(PfnSet32s, 104)(This, i, c_values, p_values, off_dw_values);
}
typedef void (STDMETHODCALLTYPE* PfnSetVa)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
static void STDMETHODCALLTYPE HookSetComputeRootCBV(ID3D12GraphicsCommandList* This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va_gpu) {
    if (i < 32) State(This)->args[i] = (RootArg){ARG_CBV, va_gpu};
    REAL(PfnSetVa, 105)(This, i, va_gpu);
}
static void STDMETHODCALLTYPE HookSetComputeRootSRV(ID3D12GraphicsCommandList* This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va_gpu) {
    if (i < 32) State(This)->args[i] = (RootArg){ARG_SRV, va_gpu};
    REAL(PfnSetVa, 106)(This, i, va_gpu);
}
static void STDMETHODCALLTYPE HookSetComputeRootUAV(ID3D12GraphicsCommandList* This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va_gpu) {
    if (i < 32) State(This)->args[i] = (RootArg){ARG_UAV, va_gpu};
    REAL(PfnSetVa, 107)(This, i, va_gpu);
}

static uint64_t PsoHash(void* p_pso) {
    for (int i = 0; i < s_c_psos; ++i) if (s_psos[i].p_object == p_pso) return s_psos[i].hash;
    return 0;
}

typedef void (STDMETHODCALLTYPE* PfnDispatch)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
static void STDMETHODCALLTYPE HookDispatch(ID3D12GraphicsCommandList* This, UINT x, UINT y, UINT z) {
    ListState* p_state = State(This);
    fprintf(s_p_trace, "DISPATCH #%d cs=%016llx root=%016llx groups=%u,%u,%u\n", s_c_dispatches++,
            (unsigned long long)PsoHash(p_state->p_pso), (unsigned long long)RootHash(p_state->p_root), x, y, z);
    for (int i = 0; i < 32; ++i) {
        RootArg* p_arg = &p_state->args[i];
        if (p_arg->type == ARG_NONE) continue;
        fprintf(s_p_trace, "  param %d:", i);
        if (p_arg->type == ARG_CONSTS) {
            for (UINT k = 0; k < p_arg->c_consts; ++k) fprintf(s_p_trace, " %08x", p_arg->consts[k]);
        } else if (p_arg->type == ARG_TABLE) {
            Heap* p_heap = NULL;
            View* p_view = ViewAtGpu(p_arg->value, &p_heap);
            fprintf(s_p_trace, " table@%llu", p_heap ? (unsigned long long)((p_arg->value - p_heap->va_gpu) / p_heap->cb_increment) : 0ull);
            // The table's size comes from the root signature; print the next 24 non-empty slots.
            for (int k = 0; p_view && k < 24 && (p_view + k) < p_heap->p_views + p_heap->c_views; ++k) {
                if (p_view[k].kind == VIEW_NONE) continue;
                fprintf(s_p_trace, "\n    [%d] %s", k, p_view[k].sz_text);
                if (p_view[k].kind == VIEW_CBV) DumpBytes("data", p_view[k].p_resource, p_view[k].a, p_view[k].b);
            }
        } else {
            uint64_t off_resource = 0;
            ResInfo* p_info = ResByVa(p_arg->value, &off_resource);
            fprintf(s_p_trace, " %s r%d+%llu", p_arg->type == ARG_CBV ? "CBV" : p_arg->type == ARG_SRV ? "SRV" : "UAV",
                    p_info ? p_info->id : -1, (unsigned long long)off_resource);
            if (p_arg->type == ARG_CBV && p_info) {
                const uint64_t cb_dump = p_info->desc.Width - off_resource < 4096 ? p_info->desc.Width - off_resource : 4096;
                DumpBytes("data", p_info, off_resource, cb_dump);
            }
        }
        fprintf(s_p_trace, "\n");
    }
    REAL(PfnDispatch, 108)(This, x, y, z);
}

typedef void (STDMETHODCALLTYPE* PfnCopyBufferRegion)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
static void STDMETHODCALLTYPE HookCopyBufferRegion(ID3D12GraphicsCommandList* This, ID3D12Resource* p_dst, UINT64 doff, ID3D12Resource* p_src, UINT64 soff, UINT64 cb_copy) {
    ResInfo* p_destination = Res(p_dst);
    ResInfo* p_source = Res(p_src);
    fprintf(s_p_trace, "COPYBUFFER r%d+%llu <- r%d+%llu size %llu", p_destination ? p_destination->id : -1, (unsigned long long)doff,
            p_source ? p_source->id : -1, (unsigned long long)soff, (unsigned long long)cb_copy);
    DumpBytes("data", p_source, soff, cb_copy);
    fprintf(s_p_trace, "\n");
    REAL(PfnCopyBufferRegion, 109)(This, p_dst, doff, p_src, soff, cb_copy);
}

typedef void (STDMETHODCALLTYPE* PfnCopyResource)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
static void STDMETHODCALLTYPE HookCopyResource(ID3D12GraphicsCommandList* This, ID3D12Resource* p_dst, ID3D12Resource* p_src) {
    ResInfo* p_destination = Res(p_dst);
    ResInfo* p_source = Res(p_src);
    fprintf(s_p_trace, "COPYRESOURCE r%d <- r%d", p_destination ? p_destination->id : -1, p_source ? p_source->id : -1);
    if (p_source && p_source->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) DumpBytes("data", p_source, 0, p_source->desc.Width);
    fprintf(s_p_trace, "\n");
    REAL(PfnCopyResource, 110)(This, p_dst, p_src);
}

typedef void (STDMETHODCALLTYPE* PfnCopyTextureRegion)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
static void STDMETHODCALLTYPE HookCopyTextureRegion(ID3D12GraphicsCommandList* This, const D3D12_TEXTURE_COPY_LOCATION* p_dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* p_src, const D3D12_BOX* p_box) {
    ResInfo* p_destination = Res(p_dst->pResource);
    ResInfo* p_source = Res(p_src->pResource);
    fprintf(s_p_trace, "COPYTEXTURE r%d (type %d) <- r%d (type %d)", p_destination ? p_destination->id : -1, p_dst->Type, p_source ? p_source->id : -1, p_src->Type);
    if (p_src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && p_source) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* p_footprint = &p_src->PlacedFootprint;
        fprintf(s_p_trace, " footprint offset %llu %ux%u format %d pitch %u", (unsigned long long)p_footprint->Offset,
                p_footprint->Footprint.Width, p_footprint->Footprint.Height, p_footprint->Footprint.Format, p_footprint->Footprint.RowPitch);
        DumpBytes("data", p_source, p_footprint->Offset, (uint64_t)p_footprint->Footprint.RowPitch * p_footprint->Footprint.Height);
    }
    fprintf(s_p_trace, "\n");
    REAL(PfnCopyTextureRegion, 111)(This, p_dst, x, y, z, p_src, p_box);
}

typedef void (STDMETHODCALLTYPE* PfnClearUavFloat)(ID3D12GraphicsCommandList*, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const FLOAT[4], UINT, const D3D12_RECT*);
static void STDMETHODCALLTYPE HookClearUavFloat(ID3D12GraphicsCommandList* This, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* p_resource, const FLOAT v[4], UINT c_rects, const D3D12_RECT* p_rects) {
    ResInfo* p_info = Res(p_resource);
    fprintf(s_p_trace, "CLEARUAVFLOAT r%d %g %g %g %g\n", p_info ? p_info->id : -1, v[0], v[1], v[2], v[3]);
    REAL(PfnClearUavFloat, 112)(This, g, c, p_resource, v, c_rects, p_rects);
}
typedef void (STDMETHODCALLTYPE* PfnClearUavUint)(ID3D12GraphicsCommandList*, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const UINT[4], UINT, const D3D12_RECT*);
static void STDMETHODCALLTYPE HookClearUavUint(ID3D12GraphicsCommandList* This, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* p_resource, const UINT v[4], UINT c_rects, const D3D12_RECT* p_rects) {
    ResInfo* p_info = Res(p_resource);
    fprintf(s_p_trace, "CLEARUAVUINT r%d %u %u %u %u\n", p_info ? p_info->id : -1, v[0], v[1], v[2], v[3]);
    REAL(PfnClearUavUint, 113)(This, g, c, p_resource, v, c_rects, p_rects);
}

// Every buffer whose address the upscaler takes (constant buffer rings, scratch) becomes known.
static D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE HookGetGpuVa(ID3D12Resource* This) {
    Res(This);
    return ((PfnGetGpuVa)s_p_real_get_va)(This);
}

void CaptureInstall(ID3D12Device* p_device, ID3D12GraphicsCommandList* p_list, const char* sz_directory) {
    snprintf(s_sz_dir, sizeof(s_sz_dir), "%s", sz_directory);
    CreateDirectoryA(s_sz_dir, NULL);
    char sz_path[MAX_PATH + 16];
    snprintf(sz_path, sizeof(sz_path), "%s\\trace.txt", s_sz_dir);
    s_p_trace = fopen(sz_path, "w");
    setvbuf(s_p_trace, NULL, _IOFBF, 1 << 20);
    s_pp_device_vtbl = *(void***)p_device;
    s_pp_list_vtbl = *(void***)p_list;
    Patch(s_pp_device_vtbl, SLOT(ID3D12DeviceVtbl, CreateComputePipelineState), HookCreateComputePipelineState, 0);
    Patch(s_pp_device_vtbl, SLOT(ID3D12DeviceVtbl, CreateRootSignature), HookCreateRootSignature, 1);
    Patch(s_pp_device_vtbl, SLOT(ID3D12DeviceVtbl, CreateDescriptorHeap), HookCreateDescriptorHeap, 2);
    Patch(s_pp_device_vtbl, SLOT(ID3D12DeviceVtbl, CreateConstantBufferView), HookCreateCBV, 3);
    Patch(s_pp_device_vtbl, SLOT(ID3D12DeviceVtbl, CreateShaderResourceView), HookCreateSRV, 4);
    Patch(s_pp_device_vtbl, SLOT(ID3D12DeviceVtbl, CreateUnorderedAccessView), HookCreateUAV, 5);
    Patch(s_pp_device_vtbl, SLOT(ID3D12DeviceVtbl, CreateSampler), HookCreateSampler, 6);
    Patch(s_pp_device_vtbl, SLOT(ID3D12DeviceVtbl, CopyDescriptorsSimple), HookCopyDescriptorsSimple, 7);
    Patch(s_pp_device_vtbl, SLOT(ID3D12DeviceVtbl, CopyDescriptors), HookCopyDescriptors, 8);
    // Resources: one vtable for committed and placed ones; take it from a small buffer.
    {
        D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC desc = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = 256, .Height = 1,
                                    .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
                                    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
        ID3D12Resource* p_probe = NULL;
        if (SUCCEEDED(ID3D12Device_CreateCommittedResource(p_device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&p_probe))) {
            void** pp_vtbl = *(void***)p_probe;
            Patch(pp_vtbl, SLOT(ID3D12ResourceVtbl, GetGPUVirtualAddress), HookGetGpuVa, 300);
            s_p_real_get_va = s_real[300];
        }
    }
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootSignature), HookSetComputeRootSignature, 100);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, SetPipelineState), HookSetPipelineState, 101);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootDescriptorTable), HookSetComputeRootDescriptorTable, 102);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstant), HookSetComputeRoot32BitConstant, 103);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstants), HookSetComputeRoot32BitConstants, 104);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootConstantBufferView), HookSetComputeRootCBV, 105);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootShaderResourceView), HookSetComputeRootSRV, 106);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootUnorderedAccessView), HookSetComputeRootUAV, 107);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, Dispatch), HookDispatch, 108);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, CopyBufferRegion), HookCopyBufferRegion, 109);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, CopyResource), HookCopyResource, 110);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, CopyTextureRegion), HookCopyTextureRegion, 111);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, ClearUnorderedAccessViewFloat), HookClearUavFloat, 112);
    Patch(s_pp_list_vtbl, SLOT(ID3D12GraphicsCommandListVtbl, ClearUnorderedAccessViewUint), HookClearUavUint, 113);
}

void CaptureMark(const char* sz_text) {
    if (s_p_trace) { fprintf(s_p_trace, "MARK %s\n", sz_text); fflush(s_p_trace); }
}

void CaptureNoteResource(ID3D12Resource* p_resource, const char* sz_name) {
    ResInfo* p_info = Res(p_resource);
    if (p_info) fprintf(s_p_trace, "NAME r%d %s\n", p_info->id, sz_name);
}
