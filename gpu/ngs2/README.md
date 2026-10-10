# Ngs2

The PS4 `libSceNgs2` software mixer (systems, racks, voices, render, reverb, panning, the custom
submixer, ATRAC9 and PCM waveforms), from AnyPS5 (https://github.com/boykopovar/AnyPS5, GPL-2.0)
at commit `5edae353`, `core/libs/prx/libSceNgs2.native`. Its reverb follows PS4 firmware 11.00's
`libSceNgs2.sprx` and its layouts shadPS4's `Orbis*` types: the library is the same on both
consoles. The first commit here is the files as they were there; later ones adapt them to libbbgpu.
