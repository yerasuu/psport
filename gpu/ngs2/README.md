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
| `Internal.h` | the objects (system, rack, voice) the game's buffers hold, and the API's declarations |
| `Handle.cpp` | the firmware's 0x20-byte handles, the game-visible recursive locks, the buffer allocator |
| `Report.cpp` | `sceNgs2Report*` and the error message every failing call hands to report handlers |
| `System.cpp` | `sceNgs2System*` except Render, and the racks each system links by class |
| `Render.cpp` | `sceNgs2SystemRender` |
| `RackOptions.cpp` | the seven rack kinds and their options: ranges, defaults, checking order |
| `Rack.cpp` | `sceNgs2Rack*` and `sceNgs2CustomRackGetModuleInfo` |
| `VoiceControl.cpp` | `sceNgs2VoiceControl` and the parameters every rack shares |
| `Voice.cpp` | the voice queries: state, flags, owner, matrix and port information |
| `Dsp.h`, `Gain.cpp`, `Envelope.cpp`, `Filter.cpp`, `Limiter.cpp`, `PeakMeter.cpp` | the DSP modules: ramps, distortion, channel matrix, envelope, biquad and LFE filters, compressor and limiter, peak meter |

Phase F2 (kb #1094) implements the system, rack, voice and report API: the validation order and
error codes of each export, the game's buffers and allocators, handles, locks, user data,
enumeration, voice matrices, ports, patches, events and callbacks. Nothing is rendered yet:
`sceNgs2SystemRender` checks its buffers and writes silence, the racks' own voice parameters
(`0x1000xxxx` and the like) and the module read `0xc001` return `0x804A0001`, and a voice that
receives PLAY stays in its initial state. Each of those is counted in the call report
(`Calls: summary stub <n> ...`), as are the exports still missing, which return `0x804A0001`:
waveform parsing (F4), pan, geom and FFT (F7). `QueryBufferSize` gives the firmware's exact size for
a system (0x10400 bytes with the default option); a rack's covers psport's own layout, smaller than
the console's, whose sum includes DSP blocks not all read yet. `tools/hwtest/ngs2_api` checks the
codes and sizes on a console. Later phases bring the render with the sampler, submixer and
mastering racks (F3), waveforms (F4), the reverb (F5), custom racks (F6) and pan, geom and FFT (F7).
