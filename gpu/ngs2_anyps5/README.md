# Ngs2

The PS4 `libSceNgs2` software mixer (systems, racks, voices, render, reverb, panning, the custom
submixer, ATRAC9 and PCM waveforms), from AnyPS5 (https://github.com/boykopovar/AnyPS5, GPL-2.0)
at commit `5edae353`, `core/libs/prx/libSceNgs2.native`. Its reverb, filters, gain, sampler envelope,
mastering LFE low-pass, matrix and limiter, and peak meters follow PS4 firmware 11.00's `libSceNgs2.sprx`; the API is the same on both
consoles, but its structures are PS5 layouts where those differ. The first commit here is the
files as they were there; later ones adapt them to libpsport-gpu and to PS4 layouts.
