// SPDX-License-Identifier: GPL-2.0-or-later
// psport: D3D12 call recorder for fsr4cap.exe (capture.c, rootsig.c).
#pragma once
#include <stdio.h>
#include <d3d12.h>

/// Hooks the vtables of `device` and `list` (all objects of their classes); writes to `directory`.
void CaptureInstall(ID3D12Device* p_device, ID3D12GraphicsCommandList* p_list, const char* sz_directory);
/// A line in the trace (frame boundaries).
void CaptureMark(const char* sz_text);
/// Names a resource in the trace (the inputs and output of the test).
void CaptureNoteResource(ID3D12Resource* p_resource, const char* sz_name);
/// Writes the parameters of a serialized root signature.
void CaptureDescribeRootSignature(FILE* p_out, const void* p_blob, size_t cb_blob);
