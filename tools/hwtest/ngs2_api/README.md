# Ngs2 system, rack, voice and report API (kb #1094)

Checks on a PS4 what psport's native Ngs2 (`gpu/ngs2/`, phase F2 of kb #625) answers from
firmware 11.00's `libSceNgs2.sprx` as rea read it: the return code of every export it implements
with valid and invalid arguments, the order in which each checks its arguments, the buffer sizes
`QueryBufferSize` asks for and the fields `GetInfo`, `GetState`, `GetPortInfo` and
`GetMatrixInfo` read back. Nothing is rendered beyond silence: that is `ngs2_render` (F3).

It writes one CSV, `/data/psport-hwtest/ngs2_api/results.csv`, one row per check:
`case,value,expected,match`. `expected` is the firmware value the spec gives and `match` says
whether the console agreed (`ok` or `MISMATCH`); a row with no `expected` is something the spec
leaves open and the console teaches psport:

| Rows | What the console tells |
|---|---|
| `rack_query_default_*`, `*_option_*_size` | the exact buffer size of each rack kind (psport's own layout is smaller, spec 08 §2) |
| `sys_handle_offset_in_buffer` | where in the game's buffer the system handle lies (0x58 in the firmware's object) |
| `sys_info_uid` | the first system's uid |
| `rack_info_reverb_channel_works`, `rack_info_mastering_channel_works` | `maxChannelWorks` of those racks |
| `report_error_message` | the text a type-0 report handler receives |

| Area | Cases |
|---|---|
| System | QueryBufferSize sizes (0x10400 by default, 0x20400 with 1024-sample grains, 0x10410 with guards) and option checks in order (size, max grain, grain, rate); Create and CreateWithAllocator argument order (0x206, 0x53, option, 0x207, 0x209, 0x20A, the allocator's own error); GetInfo fields; user data; SetGrainSamples and SetSampleRate ranges; Lock and Unlock, also from another thread (0x80020001); Render buffer checks in order (channels, type, address, size) and the clearing a failed render does; EnumHandles; Destroy |
| Rack options | every range of the sampler, submixer, reverb, mastering and custom racks, and that the rack's own fields come before the common ones |
| Racks | Create argument order with the system handle checked last; class order of EnumRackHandles; GetInfo's common part and each kind's extra part; user data; Lock and Unlock; GetVoiceHandle; CustomRackGetModuleInfo; a rack's maximum grain bounding the system's; racks outliving their system |
| Voices | VoiceControl's frame (0x309, 0x308, 0x30A), matrix levels (0x700, 0x150, 0x153, clamping to [-4, 4]), ports (0x304, volume clamp, matrix ids -1, 0..maxMatrices and 0x10..0x13, delay saturation), patches (0x305 for an input, a sampler destination and another system; 0x200 for a non-rack handle), events (0x303; STOP on an idle voice), state after PLAY and with short buffers, callbacks (0x30C, 0x30D, 0x30E), chains stopping at their first error and cycles (0x30B), patches cleared when the destination rack goes |
| Reports | RegisterHandler and UnregisterHandler codes (0x202 to 0x205), the type-0 report of a failing call, and a system destroyed from a report handler during its own render (0x30F) |

## Build and run

With the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and
`OO_PS4_TOOLCHAIN` set to it:

```
make -C tools/hwtest/ngs2_api
```

Install `IV0000-PSPT01094_00-NGS2API000000000.pkg` on a jailbroken PS4 (firmware 11.00 is the one
psport follows), start "Ngs2 API test", wait for it to exit (the klog shows `ngs2_api: done`), and
copy `/data/psport-hwtest/ngs2_api/results.csv` off the console over FTP. To compare, run the same
build through psport as a title folder (`eboot.bin` and `sce_sys/`), once with `PS_NGS2=native` and
once with the firmware module (`PS_LLE=libSceNgs2`), and diff the three files: every row should
match, the `MISMATCH` rows of the console being corrections to the spec.
