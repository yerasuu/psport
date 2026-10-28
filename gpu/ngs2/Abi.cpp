// SPDX-License-Identifier: GPL-2.0-or-later
// The vendored shadPS4 structures (gpu/shadps4/core/libraries/ngs2) checked against what firmware
// 11.00's libSceNgs2.sprx reads and writes: the sizes its API validates and the offsets it uses
// (kb #812, #813). A header that drifts from the console breaks the build here instead of a game.
#include <cstddef>

#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_custom.h"
#include "core/libraries/ngs2/ngs2_mastering.h"
#include "core/libraries/ngs2/ngs2_report.h"
#include "core/libraries/ngs2/ngs2_reverb.h"
#include "core/libraries/ngs2/ngs2_sampler.h"
#include "core/libraries/ngs2/ngs2_submixer.h"

namespace Libraries::Ngs2 {

static_assert(sizeof(OrbisNgs2Handle) == 8);
static_assert(sizeof(OrbisNgs2ContextBufferInfo) == 0x40);
static_assert(offsetof(OrbisNgs2ContextBufferInfo, userData) == 0x38);
static_assert(sizeof(OrbisNgs2BufferAllocator) == 0x18);

static_assert(sizeof(OrbisNgs2SystemOption) == 0x40);
static_assert(offsetof(OrbisNgs2SystemOption, flags) == 0x18);
static_assert(offsetof(OrbisNgs2SystemOption, maxGrainSamples) == 0x1c);
static_assert(offsetof(OrbisNgs2SystemOption, numGrainSamples) == 0x20);
static_assert(offsetof(OrbisNgs2SystemOption, sampleRate) == 0x24);

static_assert(sizeof(OrbisNgs2SystemInfo) == 0x88);
static_assert(offsetof(OrbisNgs2SystemInfo, systemHandle) == 0x10);
static_assert(offsetof(OrbisNgs2SystemInfo, bufferInfo) == 0x18);
static_assert(offsetof(OrbisNgs2SystemInfo, uid) == 0x58);
static_assert(offsetof(OrbisNgs2SystemInfo, stateFlags) == 0x64);
static_assert(offsetof(OrbisNgs2SystemInfo, rackCount) == 0x68);
static_assert(offsetof(OrbisNgs2SystemInfo, lastRenderRatio) == 0x6c);
static_assert(offsetof(OrbisNgs2SystemInfo, lastRenderTick) == 0x70);
static_assert(offsetof(OrbisNgs2SystemInfo, renderCount) == 0x78);
static_assert(offsetof(OrbisNgs2SystemInfo, sampleRate) == 0x80);
static_assert(offsetof(OrbisNgs2SystemInfo, numGrainSamples) == 0x84);

static_assert(sizeof(OrbisNgs2RenderBufferInfo) == 0x18);
static_assert(offsetof(OrbisNgs2RenderBufferInfo, waveformType) == 0x10);
static_assert(offsetof(OrbisNgs2RenderBufferInfo, numChannels) == 0x14);

static_assert(sizeof(OrbisNgs2RackOption) == 0x80);
static_assert(offsetof(OrbisNgs2RackOption, flags) == 0x18);
static_assert(offsetof(OrbisNgs2RackOption, maxGrainSamples) == 0x1c);
static_assert(offsetof(OrbisNgs2RackOption, maxVoices) == 0x20);
static_assert(offsetof(OrbisNgs2RackOption, maxInputDelayBlocks) == 0x24);
static_assert(offsetof(OrbisNgs2RackOption, maxMatrices) == 0x28);
static_assert(offsetof(OrbisNgs2RackOption, maxPorts) == 0x2c);
static_assert(sizeof(OrbisNgs2SamplerRackOption) == 0xa8);
static_assert(offsetof(OrbisNgs2SamplerRackOption, maxChannelWorks) == 0x80);
static_assert(offsetof(OrbisNgs2SamplerRackOption, maxWaveformBlocks) == 0x88);
static_assert(offsetof(OrbisNgs2SamplerRackOption, maxAtrac9Decoders) == 0x94);
static_assert(offsetof(OrbisNgs2SamplerRackOption, maxAjmAtrac9Decoders) == 0x9c);
static_assert(offsetof(OrbisNgs2SamplerRackOption, numPeakMeterBlocks) == 0xa0);
static_assert(sizeof(OrbisNgs2SubmixerRackOption) == 0x98);
static_assert(offsetof(OrbisNgs2SubmixerRackOption, maxInputs) == 0x8c);
static_assert(offsetof(OrbisNgs2SubmixerRackOption, numPeakMeterBlocks) == 0x90);
static_assert(sizeof(OrbisNgs2ReverbRackOption) == 0x88);
static_assert(offsetof(OrbisNgs2ReverbRackOption, reverbSize) == 0x84);
static_assert(sizeof(OrbisNgs2MasteringRackOption) == 0x88);
static_assert(offsetof(OrbisNgs2MasteringRackOption, numPeakMeterBlocks) == 0x84);
static_assert(sizeof(OrbisNgs2CustomRackOption) == 0x4d0);
static_assert(offsetof(OrbisNgs2CustomRackOption, stateSize) == 0x80);
static_assert(sizeof(OrbisNgs2CustomSamplerRackOption) == 0x4e8);
static_assert(sizeof(OrbisNgs2CustomSubmixerRackOption) == 0x4d8);
static_assert(sizeof(OrbisNgs2CustomMasteringRackOption) == 0x4d8);
static_assert(offsetof(OrbisNgs2SamplerRackOption, maxCodecCaches) == 0x84);
static_assert(offsetof(OrbisNgs2SamplerRackOption, maxEnvelopePoints) == 0x8c);
static_assert(offsetof(OrbisNgs2SamplerRackOption, maxFilters) == 0x90);
static_assert(offsetof(OrbisNgs2SamplerRackOption, maxAtrac9ChannelWorks) == 0x98);
static_assert(offsetof(OrbisNgs2SubmixerRackOption, maxChannels) == 0x80);
static_assert(offsetof(OrbisNgs2SubmixerRackOption, maxEnvelopePoints) == 0x84);
static_assert(offsetof(OrbisNgs2SubmixerRackOption, maxFilters) == 0x88);
static_assert(offsetof(OrbisNgs2ReverbRackOption, maxChannels) == 0x80);
static_assert(offsetof(OrbisNgs2MasteringRackOption, maxChannels) == 0x80);
static_assert(offsetof(OrbisNgs2CustomRackOption, numBuffers) == 0x84);
static_assert(offsetof(OrbisNgs2CustomRackOption, numModules) == 0x88);
static_assert(offsetof(OrbisNgs2CustomRackOption, aModule) == 0x90);
static_assert(offsetof(OrbisNgs2CustomRackOption, aPort) == 0x450);
static_assert(sizeof(OrbisNgs2CustomRackModuleInfo) == 0x28);
static_assert(offsetof(OrbisNgs2CustomRackModuleInfo, moduleId) == 0x8);
static_assert(offsetof(OrbisNgs2CustomRackModuleInfo, destBufferId) == 0x14);
static_assert(offsetof(OrbisNgs2CustomRackModuleInfo, stateOffset) == 0x18);
static_assert(offsetof(OrbisNgs2CustomRackModuleInfo, stateSize) == 0x1c);
static_assert(sizeof(OrbisNgs2CustomRackPortInfo) == 8);
static_assert(offsetof(OrbisNgs2CustomSamplerRackOption, maxChannelWorks) == 0x4d0);
static_assert(offsetof(OrbisNgs2CustomSamplerRackOption, maxCodecCaches) == 0x4e4);
static_assert(offsetof(OrbisNgs2CustomSubmixerRackOption, maxChannels) == 0x4d0);
static_assert(offsetof(OrbisNgs2CustomSubmixerRackOption, maxInputs) == 0x4d4);
static_assert(offsetof(OrbisNgs2CustomMasteringRackOption, maxInputs) == 0x4d4);

static_assert(sizeof(OrbisNgs2RackInfo) == 0xa8);
static_assert(offsetof(OrbisNgs2RackInfo, ownerSystemHandle) == 0x58);
static_assert(offsetof(OrbisNgs2RackInfo, type) == 0x60);
static_assert(offsetof(OrbisNgs2RackInfo, rackId) == 0x64);
static_assert(offsetof(OrbisNgs2RackInfo, maxVoices) == 0x74);
static_assert(offsetof(OrbisNgs2RackInfo, maxPorts) == 0x84);
static_assert(offsetof(OrbisNgs2RackInfo, stateFlags) == 0x88);
static_assert(offsetof(OrbisNgs2RackInfo, lastProcessTick) == 0x90);
static_assert(offsetof(OrbisNgs2RackInfo, renderCount) == 0x98);
static_assert(offsetof(OrbisNgs2RackInfo, activeVoiceCount) == 0xa0);
static_assert(offsetof(OrbisNgs2RackInfo, activeChannelWorkCount) == 0xa4);
static_assert(sizeof(OrbisNgs2SamplerRackInfo) == 0xc8);
static_assert(sizeof(OrbisNgs2SubmixerRackInfo) == 0xb8);
static_assert(sizeof(OrbisNgs2ReverbRackInfo) == 0xb0);
static_assert(sizeof(OrbisNgs2MasteringRackInfo) == 0xb0);
static_assert(offsetof(OrbisNgs2SamplerRackInfo, maxChannelWorks) == 0xa8);
static_assert(offsetof(OrbisNgs2SamplerRackInfo, maxAjmAtrac9Decoders) == 0xc4);
static_assert(offsetof(OrbisNgs2SubmixerRackInfo, maxChannels) == 0xa8);
static_assert(offsetof(OrbisNgs2SubmixerRackInfo, maxInputs) == 0xb4);
static_assert(offsetof(OrbisNgs2ReverbRackInfo, reverbSize) == 0xac);
static_assert(offsetof(OrbisNgs2MasteringRackInfo, maxChannels) == 0xa8);
static_assert(sizeof(OrbisNgs2CustomRackInfo) == 0x4f8);
static_assert(offsetof(OrbisNgs2CustomRackInfo, aModule) == 0xb8);
static_assert(offsetof(OrbisNgs2CustomRackInfo, aPort) == 0x478);
static_assert(sizeof(OrbisNgs2CustomSamplerRackInfo) == 0x510);
static_assert(sizeof(OrbisNgs2CustomSubmixerRackInfo) == 0x500);
static_assert(sizeof(OrbisNgs2CustomMasteringRackInfo) == 0x500);
static_assert(offsetof(OrbisNgs2CustomSubmixerRackInfo, maxChannels) == 0x4f8);
static_assert(sizeof(OrbisNgs2CustomModuleInfo) == 0x20);
static_assert(offsetof(OrbisNgs2CustomModuleInfo, stateSize) == 0x14);

static_assert(sizeof(OrbisNgs2VoiceParamHeader) == 8);
static_assert(offsetof(OrbisNgs2VoiceParamHeader, next) == 2);
static_assert(offsetof(OrbisNgs2VoiceParamHeader, id) == 4);
static_assert(sizeof(OrbisNgs2VoiceMatrixLevelsParam) == 0x18);
static_assert(offsetof(OrbisNgs2VoiceMatrixLevelsParam, aLevel) == 0x10);
static_assert(sizeof(OrbisNgs2VoicePortVolumeParam) == 0x10);
static_assert(sizeof(OrbisNgs2VoicePortMatrixParam) == 0x10);
static_assert(sizeof(OrbisNgs2VoicePortDelayParam) == 0x10);
static_assert(sizeof(OrbisNgs2VoicePatchParam) == 0x18);
static_assert(offsetof(OrbisNgs2VoicePatchParam, destHandle) == 0x10);
static_assert(sizeof(OrbisNgs2VoiceEventParam) == 0xc);
static_assert(sizeof(OrbisNgs2VoiceCallbackParam) == 0x20);
static_assert(offsetof(OrbisNgs2VoicePortVolumeParam, level) == 0xc);
static_assert(offsetof(OrbisNgs2VoicePortMatrixParam, matrixId) == 0xc);
static_assert(offsetof(OrbisNgs2VoicePortDelayParam, numSamples) == 0xc);
static_assert(offsetof(OrbisNgs2VoicePatchParam, destInputId) == 0xc);
static_assert(offsetof(OrbisNgs2VoiceEventParam, eventId) == 8);
static_assert(offsetof(OrbisNgs2VoiceCallbackParam, callbackHandler) == 8);
static_assert(offsetof(OrbisNgs2VoiceCallbackParam, callbackData) == 0x10);
static_assert(offsetof(OrbisNgs2VoiceCallbackParam, flags) == 0x18);
static_assert(sizeof(OrbisNgs2VoiceCallbackInfo) == 0x38);
static_assert(offsetof(OrbisNgs2VoiceCallbackInfo, flag) == 0x10);
static_assert(offsetof(OrbisNgs2VoiceCallbackInfo, param.waveformBlock.userData) == 0x18);
static_assert(offsetof(OrbisNgs2VoiceCallbackInfo, param.waveformBlock.data) == 0x20);
static_assert(offsetof(OrbisNgs2VoiceCallbackInfo, param.waveformBlock.dataSize) == 0x28);
static_assert(offsetof(OrbisNgs2VoiceCallbackInfo, param.waveformBlock.repeatedCount) == 0x2c);
static_assert(offsetof(OrbisNgs2VoiceCallbackInfo, param.waveformBlock.attributeFlags) == 0x30);
static_assert(sizeof(OrbisNgs2VoicePortInfo) == 0x18);
static_assert(offsetof(OrbisNgs2VoicePortInfo, volume) == 4);
static_assert(offsetof(OrbisNgs2VoicePortInfo, numDelaySamples) == 8);
static_assert(offsetof(OrbisNgs2VoicePortInfo, destInputId) == 0xc);
static_assert(offsetof(OrbisNgs2VoicePortInfo, destHandle) == 0x10);
static_assert(sizeof(OrbisNgs2VoiceMatrixInfo) == 0x104);
static_assert(sizeof(OrbisNgs2VoiceState) == 4);

static_assert(sizeof(OrbisNgs2WaveformBlock) == 0x20);
static_assert(offsetof(OrbisNgs2WaveformBlock, numSamples) == 0x10);
static_assert(offsetof(OrbisNgs2WaveformBlock, userData) == 0x18);
static_assert(sizeof(OrbisNgs2EnvelopePoint) == 0xc);

static_assert(sizeof(OrbisNgs2SamplerVoiceSetupParam) == 0x28);
static_assert(offsetof(OrbisNgs2SamplerVoiceSetupParam, flags) == 0x20);
static_assert(sizeof(OrbisNgs2SamplerVoiceWaveformBlocksParam) == 0x20);
static_assert(offsetof(OrbisNgs2SamplerVoiceWaveformBlocksParam, aBlock) == 0x18);
static_assert(sizeof(OrbisNgs2SamplerVoiceWaveformAddressParam) == 0x18);
static_assert(sizeof(OrbisNgs2SamplerVoiceWaveformFrameOffsetParam) == 0x10);
static_assert(sizeof(OrbisNgs2SamplerVoiceExitLoopParam) == 8);
static_assert(sizeof(OrbisNgs2SamplerVoicePitchParam) == 0x10);
static_assert(sizeof(OrbisNgs2SamplerVoiceEnvelopeParam) == 0x18);
static_assert(sizeof(OrbisNgs2SamplerVoiceDistortionParam) == 0x28);
static_assert(sizeof(OrbisNgs2SamplerVoiceUserFxParam) == 0x28);
static_assert(sizeof(OrbisNgs2SamplerVoicePeakMeterParam) == 0x10);
static_assert(sizeof(OrbisNgs2SamplerVoiceFilterParam) == 0x30);
static_assert(sizeof(OrbisNgs2SamplerVoiceState) == 0x30);
static_assert(offsetof(OrbisNgs2SamplerVoiceState, envelopeHeight) == 4);
static_assert(offsetof(OrbisNgs2SamplerVoiceState, peakHeight) == 8);
static_assert(offsetof(OrbisNgs2SamplerVoiceState, numDecodedSamples) == 0x10);

static_assert(sizeof(OrbisNgs2SubmixerVoiceSetupParam) == 0x10);
static_assert(sizeof(OrbisNgs2SubmixerVoiceCompressorParam) == 0x28);
static_assert(sizeof(OrbisNgs2SubmixerVoicePeakMeterParam) == 0x10);
static_assert(sizeof(OrbisNgs2SubmixerVoiceState) == 0x10);

static_assert(sizeof(OrbisNgs2ReverbVoiceSetupParam) == 0x18);
static_assert(sizeof(OrbisNgs2ReverbVoiceI3DL2Param) == 0x60);
static_assert(sizeof(OrbisNgs2ReverbVoiceState) == 4);

static_assert(sizeof(OrbisNgs2MasteringVoiceSetupParam) == 0x10);
static_assert(sizeof(OrbisNgs2MasteringVoiceMatrixParam) == 0x18);
static_assert(sizeof(OrbisNgs2MasteringVoiceLfeParam) == 0x10);
static_assert(sizeof(OrbisNgs2MasteringVoiceLimiterParam) == 0x10);
static_assert(sizeof(OrbisNgs2MasteringVoiceGainParam) == 0x10);
static_assert(sizeof(OrbisNgs2MasteringVoiceOutputParam) == 0x10);
static_assert(sizeof(OrbisNgs2MasteringVoicePeakMeterParam) == 0x10);
static_assert(sizeof(OrbisNgs2MasteringVoiceState) == 0x4c);
static_assert(offsetof(OrbisNgs2MasteringVoiceState, aInputPeakHeight) == 0xc);
static_assert(offsetof(OrbisNgs2MasteringVoiceState, aOutputPeakHeight) == 0x2c);

static_assert(sizeof(OrbisNgs2ReportDataHeader) == 0x18);
static_assert(offsetof(OrbisNgs2ReportDataHeader, result) == 0x14);
static_assert(offsetof(OrbisNgs2ReportDataHeader, handle) == 8);
static_assert(offsetof(OrbisNgs2ReportDataHeader, type) == 0x10);
static_assert(offsetof(OrbisNgs2ReportMessageData, message) == 0x18);
static_assert(sizeof(OrbisNgs2ReportMessageData) == 0x20);

} // namespace Libraries::Ngs2
