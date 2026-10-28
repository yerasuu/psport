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
| `ModuleRead.cpp` | the module read `0xc001`: each module's pending block in the firmware's layout |
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
limiter, the gain and its two meters. `tools/hwtest/ngs2_render` compares the rendered PCM of a
console, the firmware module and this one.

Phase F3b (kb #1113) follows the firmware where rea's reading of F3's open questions (spec 09)
showed it differs. The fixed port matrices cover sources and destinations of 1 to 8 channels. A
module runs only once its parameter is set since the voice's last setup, which sets only its own
modules, reads back zeros until then, and starts from its reset state in its own first grain. The
compressor and limiter keep a gain per channel and honour their flag bits (RMS detector, the
submixer's second input as sidechain, LFE out of the channel link); the mastering setup's limiter
attacks in 1 ms; the mastering's matrix type takes 3, 4, 5 and 7 channels as the 6-channel class,
leaves the four types whose classes match unset after a setup, and its read-back writes the type's
whole matrix.
The envelope's curves advance their table position in float and weight the table by 2^-15, report
the last sample's level, and a curved segment that does not move scales the rest of the grain.
A sampler voice starts with four zero samples of history. Its waveform block queue is a port of
0x21890, 0x21a30 and 0x21fe0: a ring of slots the flush copies to the render, whole frames only, a
block whose slots end before its region going on with the next slot, a partial frame forcing the
block's end with the rest of that grain silent, slots continuing a block or marked by a later
0x10000001 dropped at their turn, and a block-end callback per slot handing back only the bytes not
handed back yet; the rack's callback queue holds (maxWaveformBlocks + 8) records per voice.
`0x10000002` moves every slot holding an address, and `0x10008001` lists the ring as the firmware
walks it. KILL locks the voice's data (the rack's two bits per voice at rack+0x130) before it
returns: from then on the render fades the voice's last sample out instead of reading its waveform,
which the game may free at once, until a new PLAY reaches the render. `0xc001` copies a module's
pending block in the firmware's own layout, zeros for a module without a parameter.

Still missing, each counted in the call report (`Calls: summary stub <n> ...`) or returned as
`0x804A0001`: VAG and ATRAC9 waveforms (F4) and the waveform generators, the reverb (F5), custom
racks and the UserFx handlers (F6), and pan, geom and FFT (F7).
