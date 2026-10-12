# Audiodec

The PS4 `libSceAudiodec` audio decoders (ATRAC9, MP3 and MPEG-4 AAC), from AnyPS5
(https://github.com/boykopovar/AnyPS5, GPL-2.0) at commit `a11c8f7f`,
`core/libs/prx/libSceAudiodec` (`Export.cpp`, `src/Codecs.cpp`, `src/Codecs.hpp`). The first commit
here is the files as they were there; later ones adapt them to libbbgpu. Its structures
(`AudiodecTypes.hpp`, from AnyPS5's `core/libs/SceTypes.hpp`) are the PS4 layouts as well: the API
is the same on both consoles. AAC and MP3 decode over the FFmpeg libavcodec libbbgpu links for
AvPlayer, ATRAC9 over upstream LibAtrac9 (16-bit frames, scaled for float output). The parameters of
the first decoders created are logged (`Audiodec: ...`).
