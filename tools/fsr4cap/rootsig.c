// SPDX-License-Identifier: GPL-2.0-or-later
// psport: prints a serialized D3D12 root signature (fsr4cap capture).
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>

#include "capture.h"

static const char* RangeType(D3D12_DESCRIPTOR_RANGE_TYPE t) {
    switch (t) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: return "SRV";
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: return "UAV";
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: return "CBV";
    default: return "SAMPLER";
    }
}

void CaptureDescribeRootSignature(FILE* p_out, const void* p_blob, size_t cb_blob) {
    ID3D12VersionedRootSignatureDeserializer* p_deserializer = NULL;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(p_blob, cb_blob, &IID_ID3D12VersionedRootSignatureDeserializer, (void**)&p_deserializer))) {
        fprintf(p_out, "  (cannot deserialize)\n");
        return;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* p_versioned = NULL;
    if (FAILED(ID3D12VersionedRootSignatureDeserializer_GetRootSignatureDescAtVersion(p_deserializer, D3D_ROOT_SIGNATURE_VERSION_1_1, &p_versioned)) || !p_versioned) {
        fprintf(p_out, "  (no 1.1 desc)\n");
        ID3D12VersionedRootSignatureDeserializer_Release(p_deserializer);
        return;
    }
    const D3D12_ROOT_SIGNATURE_DESC1* p_root = &p_versioned->Desc_1_1;
    for (UINT i = 0; i < p_root->NumParameters; ++i) {
        const D3D12_ROOT_PARAMETER1* p_parameter = &p_root->pParameters[i];
        fprintf(p_out, "  param %u: ", i);
        switch (p_parameter->ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            fprintf(p_out, "table");
            for (UINT k = 0; k < p_parameter->DescriptorTable.NumDescriptorRanges; ++k) {
                const D3D12_DESCRIPTOR_RANGE1* p_range = &p_parameter->DescriptorTable.pDescriptorRanges[k];
                fprintf(p_out, " [%s x%u reg %u space %u offset %d]", RangeType(p_range->RangeType), p_range->NumDescriptors,
                        p_range->BaseShaderRegister, p_range->RegisterSpace, (int)p_range->OffsetInDescriptorsFromTableStart);
            }
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            fprintf(p_out, "constants x%u reg b%u space %u", p_parameter->Constants.Num32BitValues, p_parameter->Constants.ShaderRegister, p_parameter->Constants.RegisterSpace);
            break;
        default:
            fprintf(p_out, "%s reg %u space %u", p_parameter->ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ? "CBV" :
                    p_parameter->ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? "SRV" : "UAV",
                    p_parameter->Descriptor.ShaderRegister, p_parameter->Descriptor.RegisterSpace);
        }
        fprintf(p_out, "\n");
    }
    for (UINT i = 0; i < p_root->NumStaticSamplers; ++i) {
        const D3D12_STATIC_SAMPLER_DESC* p_sampler = &p_root->pStaticSamplers[i];
        fprintf(p_out, "  static sampler s%u space %u filter 0x%x address %d,%d,%d\n", p_sampler->ShaderRegister, p_sampler->RegisterSpace,
                p_sampler->Filter, p_sampler->AddressU, p_sampler->AddressV, p_sampler->AddressW);
    }
    ID3D12VersionedRootSignatureDeserializer_Release(p_deserializer);
}
