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

Phase F4 (kb #1096) reads waveform files. `sceNgs2ParseWaveformData`, `File` (a guest path through
the runtime's mounts, from an offset) and `User` (the game's read handler) take RIFF WAVE with PCM of
8 to 32 bits, IEEE float of 32 and 64 bits, WAVE_FORMAT_EXTENSIBLE with either or with ATRAC9's
subformat (configData from its fmt chunk, samples and delay from `fact`), and VAGp files; a `smpl`
loop, or the loop flags of a VAG's blocks, gives the loop points and up to three blocks: before the
loop, the loop itself (endless for a play count of 0) and after it, which an exit-loop request reaches.
The loop of an `.at9` file counts the codec's delay and is moved back by it. `sceNgs2CalcWaveformBlock`
covers samples with whole frames from the one holding the first sample after the delay, skipping up to
it, and `sceNgs2GetWaveformFrameInfo` answers a frame's bytes and samples, its units (ATRAC9 frames per
superframe) and the delay (one ATRAC9 frame). Every size and offset is checked in 64 bits before it
becomes a 32-bit field, a chunk walk stops after 64 chunks, and a parse reads nothing past the data it
was given. rea has not read these functions yet (spec 08 §4): their checking order and codes are
psport's (`0x804A0401` an unknown file, `0x804A0400` a missing or unusable fmt chunk, `0x804A0408` a
malformed chunk or no data, `0x804A0407` data past the end or past 32 bits, and the setup's codes for
channels, sample rate, frame and ATRAC9 config).

The sampler decodes VAG and HEVAG (`0x1c`): per channel a 16-byte block of 28 nibbles, low nibble
first, worth nibble / 8 / 2^shift with no clamp, through the 4th-order predictor of the firmware's
128 (`0x45dc0`, kept as its IEEE-754 words; an index past 127, where the firmware reads past its
table, decodes without one and is counted), with each channel's history kept across blocks. With
configData bit 0 the last channel's flags drive the block: 6 marks the loop start, 3 repeats from it
(a forced repeat of 0x21a30: an exit request ends the block there instead) and 1 ends the voice once
its history drains. Frames are decoded whole and what the grain does not use waits in the resampler's
history. The generators (`0x80` silence, `0x81` sine, `0x82`/`0x83` saws, `0x84` triangle, `0x85`
square, `0x86` white noise from the LCG 0x19660D, `0x87` the periodic noise register) run at the
system's rate from the setup's frequency and starting phase (configData modulo 360), the same on every
channel, without data, resampler or end. Where the firmware seeds its noise registers is not read yet:
they start at 0.

ATRAC9 (`0x40`) decodes over LibAtrac9. The setup asks for the rack's internal decoders
(`maxAtrac9Decoders`) or, with flags bit 0, its AJM ones (`maxAjmAtrac9Decoders`), refusing with
`0x804A0402` when the rack has none of that kind, `0x804A0403` without the 0xFE header byte and
`0x804A0052` when the config's channels differ. Each kind is a pool of decoders laid out in the rack's
buffer (LibAtrac9's state, about 90 KiB, and one frame of float planes each, which `QueryBufferSize`
counts: 27.5 MB for the default sampler option's 256 decoders, against 1.7 MB without any); the
render lends one to a voice at its first ATRAC9 grain and takes it back when the voice ends or is set
up again, and a voice that finds none reports `ERROR` (`0x804A0801`). Every pass through a block
restarts the decoder at the block's first superframe, the first `numSkipSamples` of the pass are
decoded and dropped, frames come back as float planes before LibAtrac9 rounds them to 16 bits, and a
decoding failure (`0x804A0805`, or `0x804A0803` for a sample rate index the firmware's frame table
leaves out) silences the voice with `ERROR` until it starts again, logged with its count. The firmware
runs its AJM decoders a grain ahead through libSceAjm; here they decode at once, the same samples one
grain sooner. `maxAtrac9ChannelWorks` and `maxCodecCaches` are checked but bound nothing, LibAtrac9
holding every channel. `tools/hwtest/ngs2_waveform` compares the parse, frame and block answers and
the VAG, generator and ATRAC9 renders of a console, the firmware module and this one.

Phase F5 (kb #1097) renders the reverb rack `0x2001` with module `0x1b` as firmware 11.00 builds it
(rea spec 07), not AnyPS5's network. The setup `0x20010000` (1, 2, 6 or 8 channels in and out, no more
than `maxChannels`, else `0x804A0052`) applies the I3DL2 defaults; `0x20010001` stores I3DL2 levels as
linear gains, reverb 24 dB lower, with the firmware's clamps (wet and dry up to 32 from SDK 6.00 on),
and `0x20018001` reads them back in millibels, -1 while the explicit network `0x20010002` (0xBC bytes;
taps within the reflection line, combs and all-passes within their memory, else `0x804A030A`) is set.
Per grain the input is mixed to mono without the LFE, a reflection line of 4800 or 14400 samples
(`reverbSize` 0 or 1) gives four early taps from the reflection pattern and a late one, fading from the
old taps to the new ones when the parameters change; the late tap feeds sixteen damped combs whose
length follows density and whose gain follows decayTime and decayHFRatio; each output channel sums its
own combs through four all-passes of diffusion's gain, adds the early taps, goes through the room's
two-pole high-frequency filter (run as two one-pole sections, which float rounding cannot make
unstable) and is mixed with the dry input by ramps from the last grain's levels; the LFE passes dry.
The delay lines live in the rack's buffer, sized as the firmware sizes them at 48 kHz: above 48 kHz
they are clamped to it, counted, until rea reads the half-rate mode. The explicit network's tap
routing, comb filter, per-channel levels and fields at 0xa0 to 0xb8 are not read yet: it runs with
I3DL2's choices and is counted. `0xc001` copies the fields of the 0x150-byte block rea placed.
`tools/hwtest/ngs2_reverb` records an impulse response per test room.

Still missing, each counted in the call report (`Calls: summary stub <n> ...`) or returned as
`0x804A0001`: custom racks and the UserFx handlers (F6), and pan, geom and FFT (F7).
