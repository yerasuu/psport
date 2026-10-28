# Ngs2 (native)

psport's own `libSceNgs2`, written from firmware 11.00's `libSceNgs2.sprx` (read with rea, kb #812
and #813) and the PS4 structures and error codes of shadPS4's headers
(`gpu/shadps4/core/libraries/ngs2/`, checked against the firmware in `Abi.cpp`). No code from
AnyPS5: the implementation it replaces lives apart in `gpu/ngs2_anyps5/` until this one reaches
parity (kb #625, decision #809).

`PS_NGS2` picks the one a run registers, once at start-up (`Runtime: Ngs2 backend: ...`):
`anyps5` (the default) or `native`.

| File | What it holds |
|---|---|
| `Exports.cpp` | the NID table: firmware 11.00's 47 exports plus the three PS5 ones `gpu/ngs2_anyps5` answers |
| `Internal.h`, `VoiceData.h` | the objects (system, rack, voice) the game's buffers hold, what the render keeps per voice, and the declarations |
| `Handle.cpp` | the firmware's 0x20-byte handles, the game-visible recursive locks, the buffer allocator |
| `Report.cpp` | `sceNgs2Report*` and the error message every failing call hands to report handlers |
| `System.cpp` | `sceNgs2System*` except Render, and the racks each system links by class |
| `Render.cpp` | `sceNgs2SystemRender` |
| `RackOptions.cpp` | the seven rack kinds and their options: ranges, defaults, checking order |
| `Rack.cpp` | `sceNgs2Rack*` and `sceNgs2CustomRackGetModuleInfo` |
| `VoiceControl.cpp` | `sceNgs2VoiceControl` and the parameters every rack shares |
| `Voice.cpp` | the voice queries: state, flags, owner, matrix and port information |
| `Graph.cpp` | the render's voice life cycle: flush, graph order, grain events, post and voice callbacks |
| `Mixer.cpp` | event fades, ports into inputs through matrices and delays, the fixed port matrices, render buffer output |
| `SamplerControl.cpp`, `Sampler.cpp` | the sampler's parameters and waveform block queue; its source (linear PCM, loops, streaming, pitch) and chain |
| `Submixer.cpp`, `Mastering.cpp` | those racks' parameters and processing |
| `ModuleParams.cpp` | the parameters the sampler and submixer share: envelope, distortion, UserFx, peak meter, filters |
| `Dsp.h`, `Gain.cpp`, `Envelope.cpp`, `Filter.cpp`, `Limiter.cpp`, `PeakMeter.cpp` | the DSP modules: ramps, distortion, channel matrix, envelope, biquad and LFE filters, compressor and limiter, peak meter |

Phase F2 (kb #1094) implements the system, rack, voice and report API: the validation order and
error codes of each export, the game's buffers and allocators, handles, locks, user data,
enumeration, voice matrices, ports, patches, events and callbacks. `QueryBufferSize` gives the
firmware's exact size for a system (0x10400 bytes with the default option); a rack's covers psport's
own layout, which also holds everything the render keeps per voice, so that a render never
allocates. `tools/hwtest/ngs2_api` checks the codes and sizes on a console.

Phase F3 (kb #1095) renders. PLAY reserves channel works (0x804A0052, 0x306, 0x307) and the render
flushes what VoiceControl left pending unless the game holds SystemLock or RackLock, processes the
sampler voices in start order and the submixer and mastering voices in graph order (sources before
their destinations, cycles flagged), applies the grain's events (PAUSE, RESUME, STOP, STOP_IMM,
KILL fades), mixes each port through its matrix, volume ramp and delay into its destination's input,
and converts the mastering output into the game's I16 or F32 buffers (big-endian ones swapped). The
sampler plays linear PCM (`0x10` to `0x1b`) from its waveform block queue with loops, exit-loop
requests, streaming and the callbacks of block ends and loops, resampled by the pitch with the
firmware's 4-tap interpolation; the envelope, distortion, filters and peak meter follow. The submixer
adds the compressor; the mastering applies its sixteen channel matrices, the LFE low-pass, the
limiter, the gain and its two meters.

Still missing, each counted in the call report (`Calls: summary stub <n> ...`) or returned as
`0x804A0001`: VAG and ATRAC9 waveforms (F4) and the waveform generators, the reverb (F5), custom
racks and the UserFx handlers (F6), pan, geom and FFT (F7), the sampler's address change
(`0x10000002`) and queued block read (`0x10008001`), the module read `0xc001`, the compressor's
detector and link flag bits, and a submixer's second input.
