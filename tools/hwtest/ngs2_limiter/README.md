# Ngs2 mastering limiter and LFE (kb #395)

Checks on a PS4 what psport's `gpu/ngs2_anyps5/Limiter.cpp` and the mastering setup's LFE low-pass
reproduce from firmware 11.00's `libSceNgs2.sprx`. Each case renders 40 grains of 256 frames at
48 kHz from a float PCM sampler voice through a mastering voice and writes, per case, one CSV to
`/data/psport-hwtest/ngs2_limiter/<case>.csv`: one row per frame and channel, with
`limiter_peak_level` and `limiter_press_level` from `sceNgs2VoiceGetState` on the last frame of
each grain. Its first line holds the return code of the 0x30000003 call.

| Case | Mastering | Input | Checks |
|---|---|---|---|
| `default` | stereo, setup only | ch0 sine 1 kHz at 2.0, ch1 step 0.25 -> 1.5 at frame 4800 | default limiter (threshold 1.0, slope 0, 10 ms attack and release) |
| `threshold_0.5` | + 0x30000003 {1, 0.5} | same | the parameter's threshold |
| `threshold_9_clamped` | + 0x30000003 {1, 9.0} | same | threshold clamped to 4, so nothing is limited |
| `disabled` | + 0x30000003 {0, 0.5} | same | limiter off, input passes untouched |
| `wrong_size` | + 0x30000003 with size 0x18 | same | expected 0x804a030a, limiter left as set up |
| `lfe_default` | 5.1, setup only | ch0 sine 50 Hz at 0.5, ch3 (LFE) sine 1 kHz at 0.5 | LFE low-pass on at 120 Hz after setup |
| `lfe_off` | + 0x30000002 {0, 120} | same | LFE unfiltered |

The console prints the title's compiled SDK version first: the limiter's soft knee only runs at
0x1700000 or later, so the result depends on what the toolchain wrote in the eboot's process
parameters.

## Build and run

With the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and
`OO_PS4_TOOLCHAIN` set to it:

```
make -C tools/hwtest/ngs2_limiter
```

Install `IV0000-PSPT00395_00-NGS2LIMITER00000.pkg` on a jailbroken PS4 (firmware 11.00 is the one
psport follows), start "Ngs2 limiter test", wait for it to exit (the klog shows
`ngs2_limiter: done`), and copy `/data/psport-hwtest/ngs2_limiter/` off the console over FTP.
To compare, run the same build through psport as a title folder (`eboot.bin` and `sce_sys/`)
and diff the two folders; the values should match to
float rounding (psport's RMS sums in another order than the firmware's SIMD code).
