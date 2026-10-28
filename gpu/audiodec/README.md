# Audiodec

psport's own `libSceAudiodec`: the PS4 audio decoders for ATRAC9, MP3 and MPEG-4 AAC (ADTS or raw access
units). It replaces AnyPS5's port and keeps none of its code: the API follows what the game links against
(NIDs, the structure layouts and error codes in `Abi.hpp`), and the contract of each export (order of the
checks, error codes, defaults) was read as facts from AnyPS5's `Export.cpp`, not copied (FORK.md). The decoding
goes through FFmpeg's libavcodec API (`mp3float` and `aac`, the FFmpeg libpsport-gpu links for AvPlayer) and
LibAtrac9's API (`third_party/LibAtrac9`, 16-bit frames, scaled for float output).

- `Library.cpp`: the six exports, decoder handles 1 to 64, the parameter checks and the info blocks.
- `Decoders.cpp`: one access unit per call (an MP3 frame, an ADTS frame, a raw AAC access unit or an ATRAC9
  superframe) to interleaved 16-bit or float PCM.
- `Ajm.cpp`: the C interface (`psport_gpu_audio_decoder_*`, `gpu/psport_gpu.h`) through which libSceAjm's MP3
  and AAC instances (`src/audio/runtime_ajm.c`) decode one frame at a time to float with the same decoders. It
  also reads the gapless window an MP3 frame's LAME (Xing/Info), VBRI or FGH header announces, which Ajm
  applies unless the instance sets IgnoreOfl.

The parameters of the first decoders created are logged (`Audiodec: create ...`); unsupported codecs and
parameters (24-bit PCM, an unknown AAC configuration) and failed decodes are logged with their counts.
