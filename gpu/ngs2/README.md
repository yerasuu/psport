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
| `Waveform.cpp` | `sceNgs2ParseWaveformData`, `File` and `User`, `sceNgs2CalcWaveformBlock`, `sceNgs2GetWaveformFrameInfo`, and the frame shape of each waveform type |
| `SamplerControl.cpp`, `Sampler.cpp` | the sampler's parameters and waveform block queue; its source (linear PCM, loops, streaming, pitch) and chain |
| `Source.h`, `Vag.cpp`, `Generator.cpp` | the queue as the decoders use it; the VAG and HEVAG decoder; the waveform generators |
| `Atrac9Source.cpp`, `Atrac9Glue.c` | ATRAC9 waveforms over LibAtrac9 (`third_party/LibAtrac9`, MIT), whose decoders live in the rack's buffer |
| `Submixer.cpp`, `Mastering.cpp`, `ReverbRack.cpp` | those racks' parameters and processing |
| `ModuleParams.cpp` | the parameters the sampler and submixer share: envelope, distortion, UserFx, peak meter, filters |
| `Dsp.h`, `Gain.cpp`, `Envelope.cpp`, `Filter.cpp`, `Limiter.cpp`, `PeakMeter.cpp` | the DSP modules: ramps, distortion, channel matrix, envelope, biquad and LFE filters, compressor and limiter, peak meter |
| `Reverb.h`, `ReverbModule.cpp`, `ReverbDsp.cpp` | the reverb module `0x1b`: its parameter block, the explicit network's checks, and its network |
| `Custom.h`, `CustomRack.cpp` | the custom racks `0x4001` to `0x4003`: their blocks, setups, module parameter routing and module chain |
| `CustomModules.cpp` | the modules a custom rack's list may hold: their blocks, parameters by selector and processing |
| `UserFx.cpp` | the game's UserFx and UserFx2 handlers, called without the voice's lock |
| `Spatial.h`, `Pan.cpp`, `Geom.cpp`, `Fft.cpp` | `sceNgs2Pan*`, `sceNgs2Geom*` and `sceNgs2Fft*`: pure math over the game's structures |

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

Phase F4 (kb #1096) reads waveform files, and F4b (kb #1119) does it as the firmware's own parse
functions do (spec 10 §1, §2). `sceNgs2ParseWaveformData`, `File` (a guest path through the runtime's
mounts, from an offset) and `User` (the game's read handler) clear the info's type and channels first,
then check in the firmware's order: `Data` the info (`0x804A0053`), a size below 4 (`0x804A0408`) and
null data (`0x804A0406`); `File` opens the file before anything (`0x8002000E` when it cannot); `User`
the null handler (`0x804A0408`) before the info. A failed read, or a handler answering below 0, is
`0x804A0408`. Only `VAGp` and `RIFF` files are taken, the `WAVE` form not looked at. Each RIFF chunk is
searched from offset 12 up to riffSize - 4 with no padding between chunks, a chunk of size 0 ending the
search; without `fmt ` or `data` the parse fails with `0x804A0408`. `fmt ` gives PCM of 8 to 32 bits,
IEEE float of 32 and 64 bits, or WAVE_FORMAT_EXTENSIBLE with the PCM subformat or ATRAC9 (its GUID, or
any fmt chunk of 0x34 bytes); any other format is `0x804A0401`. Channels, sample rates, nBlockAlign,
the ATRAC9 config and the data's size against the file are not checked. `fact` gives the samples
(required for ATRAC9; PCM counts them from the data when it is missing or 0), and the first `smpl` loop
the loop points, its end + 1 as the exclusive end, its play count ignored, both moved back by ATRAC9's
delay of one frame (256 samples at 48 kHz, 128 at 24 kHz, 64 at 12 kHz). A VAGp's loop comes from
channel 0's block flags (6 its start, 3 its end) when frame 1 has the loop bit, and without it the last
frame is left out; configData stays 0, the blocks doing the loop. The blocks are the part before the
loop (left out when empty for SDK 1.70 and later), the loop repeating until an exit request, and the
part after it; the game's info gets its fields and those blocks only. `sceNgs2CalcWaveformBlock` and
`sceNgs2GetWaveformFrameInfo` clear their outputs first, then answer `0x804A0053` for a null block,
`0x804A0400` for a null format and `0x804A0401` for any type but PCM, VAG and ATRAC9: PCM by samples,
VAG by whole frames, ATRAC9 from the superframe holding the first sample, skipping up to it plus the
frame of delay, frameMargin ignored, in 32-bit arithmetic. Where the firmware would loop forever, divide
by zero or read its stack, psport departs from it: a chunk search gives up after 64 chunks, PCM with 0
channels and no `fact` and an ATRAC9 config without frames (sample rate indices other than 1, 4 and 7,
in a parse or in `sceNgs2CalcWaveformBlock`) are `0x804A0401`, and the bytes a chunk shorter than the
firmware's read leaves out count as zeros.

The sampler decodes VAG and HEVAG (`0x1c`): per channel a 16-byte block of 28 nibbles, low nibble
first, worth nibble / 8 / 2^shift with no clamp, through the 4th-order predictor of the firmware's
128 (`0x45dc0`, kept as its IEEE-754 words; an index past 127, where the firmware reads past its
table, decodes without one and is counted), with each channel's history kept across blocks. With
configData bit 0 the last channel's flags drive the block: 6 marks the loop start, 3 repeats from it
(a forced repeat of 0x21a30: an exit request ends the block there instead) and 1 ends the voice once
its history drains. Frames are decoded whole and what the grain does not use waits in the resampler's
history. The generators (`0x81` sine, `0x82`/`0x83` saws, `0x84` triangle, `0x85` square, `0x86`
white noise from the LCG 0x19660D, `0x87` the periodic noise register) fill the source's input like a
decoder, the same on every channel, without data or end, and go through the resampler as waveforms at
the system's rate: the voice's pitch applies and their first sample comes out at output 3 (spec 10 §4).
The phase runs in float from configData / 360 (configData modulo 360) by (1 / system rate) * frequency,
its fraction by truncation; the noise registers start at 0, and the periodic one shows its value before
each step, steps every period + 1 samples and reads its pattern from bit 0, then down from bit 31.
`0x80` is silence, where the firmware lets the resampler read the rack's shared decode buffer as the
previous voice left it.

ATRAC9 (`0x40`) decodes over LibAtrac9. The setup asks for the rack's internal decoders
(`maxAtrac9Decoders`) or, with flags bit 0, its AJM ones (`maxAjmAtrac9Decoders`), refusing with
`0x804A0402` when the rack has none of that kind, `0x804A0403` without the 0xFE header byte and
`0x804A0052` when the config's channels differ. Each kind is a pool of decoders laid out in the rack's
buffer (LibAtrac9's state, about 90 KiB, and one frame of float planes each, which `QueryBufferSize`
counts: 27.5 MB for the default sampler option's 256 decoders, against 1.7 MB without any). PLAY
reserves what the setup asks for (spec 10 §5): an internal decoder and one of `maxAtrac9ChannelWorks`
per channel, or an AJM decoder; a pool of one decoder per voice gives each voice its own. An empty pool
fails PLAY with `0x804A0307`, keeping nothing, and the voice gives its decoder back with its channel
works (STOP_IMMEDIATE, KILL, its end, its next PLAY). The decoder restarts, overlap included, on the
voice's first decode and at the start of each pass through a block that skips samples, whose first
`numSkipSamples` are decoded and dropped; a block that skips none goes on from the decoder's state, so
that streamed blocks and their loops join without a gap (spec 10 §3). Frames come back as float planes
before LibAtrac9 rounds them to 16 bits, and a decoding failure or a config the decoder cannot start
with (`0x804A0805`) silences the voice with `ERROR` until it starts again, logged with its count. The
firmware runs its AJM decoders a grain ahead through libSceAjm; here they decode at once, the same
samples one grain sooner. `maxCodecCaches` is checked but bounds nothing, LibAtrac9 holding every
channel. `tools/hwtest/ngs2_waveform` compares the parse, frame and block answers, the decoder pools and
the VAG, generator and ATRAC9 renders of a console, the firmware module and this one.

Phase F5 (kb #1097) renders the reverb rack `0x2001` with module `0x1b` as firmware 11.00 builds it
(rea spec 07), not AnyPS5's network. The setup `0x20010000` (1, 2, 6 or 8 channels in and out, no more
than `maxChannels`, else `0x804A0052`) applies the I3DL2 defaults; `0x20010001` stores I3DL2 levels as
linear gains, reverb 24 dB lower, with the firmware's clamps (wet and dry up to 32 from SDK 6.00 on),
and `0x20018001` reads them back in millibels, -1 while the explicit network `0x20010002` (0xBC bytes;
taps within the reflection line, combs and all-passes within their memory, else `0x804A030A`) is set.
Per grain the input is mixed to mono without the LFE, a reflection line of 4800 or 14400 samples
(`reverbSize` 0 or 1) gives four early taps from the reflection pattern and a late one, fading from the
old taps to the new ones on every grain that reconfigures (from silence after a reset); the late tap
feeds sixteen damped combs whose length follows density and whose gain follows decayTime and
decayHFRatio; each output channel sums its own combs through four all-passes of diffusion's gain, adds
the early taps (each sum times its group gain, reflections or reverb times room), goes through the
room's two-pole high-frequency filter (run as two one-pole sections, which float rounding cannot make
unstable) and is mixed with the dry input by ramps from the last grain's levels; the LFE passes dry.
The delay lines live in the rack's buffer, sized as the firmware sizes them at 48 kHz: above 48 kHz
they are shortened to fit it and counted (the firmware's half-rate mode is never switched on).
`tools/hwtest/ngs2_reverb` records an impulse response per test room.

kb #1121 closes F5's gaps from rea spec 11: I3DL2 tap delays round in samples as 0x282e0 does; the
combs' lengths are rounded up to odd and laid back to back, the prefilter is always on from the square
root of decayHFRatio, the firmware's damping and mix constants are used bit for bit, and the combs are
cleared only when density changes. The explicit network runs as 0x28440 and 0x292d0 build it: only the
masked taps and combs change (align8 and align4 of their delays), the other combs keep their lines and
go on sounding, nothing is cleared, the prefilter and group gains stay as the last I3DL2 grain left
them (1 after a reset), the per-channel density is the tap mask's bits read as a float, the all-passes
take align4 plus their channel's offset, and the first level scales the input (the reverb on the LFE)
and the second the reverb. `0xc001` copies the 0x150-byte block both setters update. Kept apart from
the firmware, each in `FORK.md`: a comb of length 0 is silent and counted where the firmware hangs; with
the prefilter off after a reset the late tap reaches the combs as it is, where the firmware's
four-sample path feeds them 0; lines that would pass the instance's memory or the reflection line are
shortened; an index equal to a comb's new length goes back to 0; a NaN explicit delay answers
`0x804A030A` and a NaN explicit level, gain, feedback or damping is stored as 0.

Phase F6 (kb #1098) renders the custom racks `0x4001` (sampler), `0x4002` (submixer) and `0x4003`
(mastering) as firmware 11.00 builds them (rea spec 02 §4.5, 03 §5.5, 04 §2.2, §2.5, §2.6, §4.3, 09 §12). The
creation checks the module list in the firmware's order and codes (`0x804A0A06` stateSize, `0A02`
numBuffers, `0A01` numModules, `0A00` a module id outside the table at 0x50000, `0A03` a buffer id,
`0A04`/`0A05` a module's state against the rack's own 0x30 or 0xc bytes and stateSize, `0A00` a port's
buffer) and, after the rack's common fields, the options of the modules that take one (peak meter blocks,
reverb line, envelope points, chorus phases, delay type, taps and length, pitch shift quality, UserFx2's
0x48 bytes; `0x804A0081` for a wrong size). `QueryBufferSize` counts the work buffers, each voice's state
(stateSize, 16-byte aligned) and every module's blocks: two copies of its parameters, its DSP state and
what that points into (meter ring, reverb lines sized from its option, UserFx2's common, parameter and
work blocks). `0x40010000` to `0x40010005` and `0x40018000/1` are the sampler's own ids on the custom
sampler; `0x40020000` sets a custom submixer's input and output channels and `0x40030000`/`0x40030001`
a custom mastering's channels and output buffer. A module's parameter id is `0x40000000 | 0x8000 (read) |
moduleId << 8 | sub << 5 | index`, refused with `0x804A0308` unless the list holds that module at that
index. Implemented, with their parameters, read-backs, `0xc001` blocks and DSP: the compressor and
limiter `0x12` (sub 0 and 1; the sidechain reads the module's extra buffer), the filter `0x13` (sub 0
keeps type and mask as bytes at location 0; sub 1 the rack's layout), the LFE filter `0x14` (channel 3,
whose second stage reads the source again when the destination differs), the gain `0x15` (8 levels in
[-32, 32], ramps from the last grain), the peak meter `0x18`, UserFx `0x19`, the reverb `0x1b` (the F5
network: I3DL2 and the explicit network) and UserFx2 `0x1f`. Each grain the voice state is cleared, the
source or the inputs fill buffer 0 (and 1 with two inputs), the modules run in list order: one without
a parameter copies its source buffer into its destination, one with a parameter gets a cleared source
if nothing wrote it, writes its destination and its part of the voice state (compressor level and gain,
meter peaks); the grain's events fade every buffer written, and the ports mix their source buffers or
the output module writes buffer 0 to the game's. The other modules (`0x10`, `0x11`, `0x16`, `0x17`,
`0x1a`, `0x1c` to `0x1e`, `0x20`, `0x70` and `0x71` in a list) take their parameters, pass their input
through and are counted. The UserFx handlers of the sampler (`0x10000008`), the submixer (`0x20000004`)
and custom racks are now called, with the module's grain flags; a negative answer sets `ERROR`. The voice's
lock is released around the game's code, so a handler may call back into Ngs2 for its own voice; the
render's process lock stays held, as the firmware does around voice callbacks. UserFx2's control handler
takes its parameter (or the data is copied when its size is paramSize, else `0x804A0209`), its process
handler gets the common, parameter, work and state blocks, and its setup and cleanup handlers run once per
voice when the rack is created and destroyed (inferred, until rea reads when 0x243c0 runs).
`tools/hwtest/ngs2_custom` compares the checks and the PCM of filter, gain, compressor, meter, LFE, reverb
and UserFx chains on a console, the firmware module and this one.

Phase F7 (kb #1099) adds pan, geom and FFT as firmware 11.00 computes them (rea spec 12), none of which
any title on disk imports. `sceNgs2PanInit` keeps a custom layout's angles wrapped onto the circle (360
degrees or 2π, else `0x804A0910`; 2, 4, 5 or 7 speakers, else `0x804A0911`) in the order round it from
the front speaker, closed by that speaker one unit on; the default layouts (0 speakers is 7) take any
unit and convert to radians only the speakers asked for, the closing angle staying in degrees.
`sceNgs2PanGetVolumeMatrix` (checks `0x804A0913`, `0914`, `0053`, `0912` for formats other than 1, 2, 6
and 8, `0911`, `0910`, `0052` past 8 parameters) pans each source between the two speakers around it
with sine weights, adds its opposite point weighted by `distance` (1 the source alone, 0 both halves,
-1 the opposite), scales by the full-band and LFE levels and folds the speakers into the format
through the fixed port matrices of `Mixer.cpp`, laid out by output channel then parameter. The geom
builds the listener's frame (left-handed with coordinate 1, sound speed at least 0.1) and, for a
source, the distance gain of the six rolloff models (inverse, linear and exponential, clamped or not),
the cone, the level between minLevel and maxLevel, the A3D position and volume, the Doppler pitch, the
second-order spherical harmonics or the speaker matrix through the pan, with the firmware's fast
inverse square roots and table arctangent kept word for word, so that its results match the
firmware's bit for bit. The FFT takes 2^8 to 2^12 points (-1 otherwise or for a short buffer) and
gives the unscaled DFT, packed as [X0, X(N/2), Re X1, Im X1, ...], or in the firmware's internal order
(its zreorder ported exactly) when unordered, scaled by 1/N with flag 8; Init's flag 0x2 asks for the
real transform, and without it 11.00 only moves the game's buffers, which psport does too. Where the
firmware would loop forever or fault (an angle its wrap never brings onto the circle, null FFT
pointers), psport answers `0x804A0911`, `0x804A0914` or -1, counted. `tools/hwtest/ngs2_pan` dumps the
matrices, attributes and spectra of a console, the firmware module and this one.

Still missing, each counted in the call report (`Calls: summary stub <n> ...`) or returned as
`0x804A0001`: the custom modules rea has not specified as DSP for custom racks yet (see above), the
module info callbacks of `sceNgs2CustomRackGetModuleInfo`, and the three PS5 exports.
