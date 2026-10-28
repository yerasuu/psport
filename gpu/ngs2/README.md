# Ngs2 (native)

psport's own `libSceNgs2`, written from firmware 11.00's `libSceNgs2.sprx` (read with rea, kb #812
and #813) and the PS4 structures and error codes of shadPS4's headers
(`gpu/shadps4/core/libraries/ngs2/`, checked against the firmware in `Abi.cpp`). No code from
AnyPS5: the implementation it replaces lives apart in `gpu/ngs2_anyps5/` until this one reaches
parity (kb #625, decision #809).

`PS_NGS2` picks the one a run registers, once at start-up (`Runtime: Ngs2 backend: ...`):
`anyps5` (the default) or `native`. Phase F1 (kb #841) is the skeleton: `Exports.cpp` registers
the 47 exports of firmware 11.00 plus the three PS5 ones `gpu/ngs2_anyps5` also answers, and
each returns `ORBIS_NGS2_ERROR_FAIL` (`0x804A0001`), logged on its first call and counted in the
call report (`Calls: summary stub <n> sceNgs2... (HLE)`). Later phases fill the API (F2), the
render with the sampler, submixer and mastering racks (F3), waveforms (F4), the reverb (F5),
custom racks (F6) and pan, geom and FFT (F7).
