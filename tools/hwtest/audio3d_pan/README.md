# Audio3d pan (kb #201)

Plays on a PS4 what psport's libSceAudio3d (`src/audio/runtime_audio3d.c`) renders for an object, so that
the console's output and psport's can be recorded and compared case by case. psport's panner is its own
(decision #194): objects pan in pairs at constant power over a horizontal 7.1 ring (C 0°, R 30°, SR 90°,
BR 150°, BL 210°, SL 270°, L 330°, LFE never), the spread spreads the power over an arc, elevation becomes
an extra arc of 2π·(1 − horizontal distance / distance), and without a 5.1 or wider device the port folds
7.1 to stereo (C, SL and BL at -3 dB into L, their right twins into R, no LFE). None of it has been
checked against a console; this test is how.

One Audio3d port, one object, a 440 Hz sine at 0.25 (-12 dBFS) in float, 48 kHz, granularity 256. Three
1 kHz beeps of 0.1 s open the run (t = 0 is the first one's onset), then 54 cases, each a 3 s tone between
pauses of 1 s (5 s per case, 4 min 31 s in all):

- positions (azimuth, elevation): `front` (0, 0), `right_30` (30, 0), `left_30` (-30, 0), `right_90`
  (90, 0), `left_90` (-90, 0), `back` (180, 0), `above_45` (0, 45), `overhead` (0, 90) and `origin`
  (the vector (0, 0, 0));
- each with spread 0, π and 2π (attribute 4, radians);
- all of them first with the port's late reverb level (`sceAudio3dPortSetAttribute` 0x10001) at 0, then
  at 0.5.

The position, spread and gain (1.0) are set in the pause before each tone, so the console never moves a
sounding object. Tones fade in and out over 10 ms so that no click carries its own position.

## Conventions

Attribute ids as games pass them (note #193): 1 PCM, 3 POSITION (three floats), 4 SPREAD (float,
radians), 5 GAIN (float); 0x10001 the port's late reverb level (float, 0 to 1). shadPS4's enum differs
and is wrong.

Positions are Cartesian, relative to the listener: +x right, +y up, +z front, at a distance of 1 (no unit:
psport applies no distance attenuation). The azimuth turns clockwise seen from above, from the front
toward the right:

    x = sin(azimuth) cos(elevation),  y = sin(elevation),  z = cos(azimuth) cos(elevation)

This is the convention FMOD passes (note #193) and the one psport reads (`object_gains`: azimuth
`atan2f(x, z)`, 30° is R). If the console mirrors an axis, the test shows it: `right_30` coming out on the
left means x is the other way, `front` and `back` swapped means z is.

## What it writes

To `/data/psport-hwtest/audio3d_pan/<mode>/`, `<mode>` being `stereo` or `7.1` from `config.txt`:

- `results.csv`, `case,value,expected,match`: the answers of the API calls, `expected` being what psport
  answers. Rows that ask the console something psport decided alone: `default_parameters_*` (psport's
  defaults: size 0x20, granularity 0x100, 512 objects, queue depth 2, advance and push),
  `late_reverb_1_5_rejected` (**unconfirmed**: psport answers `0x80EA0004` for a level outside 0..1 because
  Koei Tecmo's engine only passes 0..1), and `grain_*_errors`, the count of failed PCM, advance and push
  calls (0; the first failure is also in the klog).
- `timeline.csv`, one row per case: `index,case,azimuth_deg,elevation_deg,x,y,z,spread_rad,late_reverb,
  tone_start_s,tone_end_s,wall_start_s`. The tone times count samples from the first beep, which is the
  time a recording sees, since a synchronous push waits for the console's output; `wall_start_s` is the
  process clock at the same point, to show the push did pace the loop.
- `record.txt`, the plain list to record by: each case's start and end (m:ss.s), what it is, and what
  psport renders for it in that mode.

## Build and run

With the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and
`OO_PS4_TOOLCHAIN` set to it:

```
make -C tools/hwtest/audio3d_pan
```

The program declares the calls itself (no SDK header was at hand when it was written), from shadPS4's
`src/core/libraries/audio3d/audio3d.h` at `67dbc06` and from what psport's `runtime_audio3d.c` takes.
Check them against the SDK before trusting a failure:

- `sceAudio3dPortOpen(user, parameters, &port)` with user 255 (the system user, the only one psport
  accepts) and a 0x28-byte `OrbisAudio3dOpenParameters` with 2 beds;
- `sceAudio3dObjectSetAttributes(port, object, count, attributes)` with 24-byte attributes
  `{id, pad, value, size}` and a 24-byte PCM `{format, pad, samples, count}`, format 1 float;
- `sceAudio3dPortSetAttribute(port, 0x10001, &level, 4)`;
- `sceAudio3dPortPush(port, 1)` as the synchronous push;
- `sceSysmoduleLoadModule(0x00A7)` for libSceAudio3d (shadPS4's `sysmodule_table.h`; Ngs2's 0x000B in
  the other homebrews comes from the same table);
- that the toolchain has stubs for `libSceAudio3d` and `libSceAudioOut` (`$OO_PS4_TOOLCHAIN/lib`);
  without the first, the link fails and a stub has to be made for it.

On the console: set the audio output, write `/data/psport-hwtest/audio3d_pan/config.txt` with one line,
`output=stereo` or `output=7.1`, to match it (without the file the run is taken as stereo), install
`IV0000-PSPT00201_00-AUDIO3DPAN000000.pkg` on a jailbroken PS4 (firmware 11.00 is the one psport follows),
start recording, start "Audio3d pan test" and wait for it to exit (the klog shows `audio3d_pan: done`).
Copy the folder off the console over FTP. Run it once per mode.

The console renders Audio3d to whatever its own output is: under Settings, Sound and Screen, Audio Output
Settings, the primary output port (HDMI) and its audio format (Linear PCM; a bitstream format leaves the
decoding to the receiver). 7.1 Linear PCM needs a TV or receiver that reports 7.1 over HDMI; stereo is a
TV or a receiver set to stereo, or headphones on the controller ("Output to Headphones": All Audio). The
menu names are from the PS4's settings as remembered, not checked on firmware 11.00.

Recording: stereo with any HDMI capture or the TV's line out; 7.1 needs a capture that keeps eight
channels (or a receiver and an ear at each speaker). Cut each case from the onset of the first beep plus
`tone_start_s` to `tone_end_s`.

## psport's side

Run the same build through psport as a title folder (`eboot.bin` and `sce_sys/`), with
`PS_AUDIO_DUMP=<prefix>`: every AudioOut port writes what reaches the host, after psport's volume, channel
order and downmix, as raw interleaved float at 48 kHz to `<prefix>.port<id>.<ch>ch.f32`. The Audio3d port
is AudioOut type 126, port 21 when nothing else opened one, so the file is `<prefix>.port21.2ch.f32` or
`<prefix>.port21.8ch.f32`. Its sample 0 is the first beep's onset, so `timeline.csv` cuts it directly.
psport writes its folder to `/data` in its user folder (`PS_USER_DIR`, else `user/` in `PS_DATA_DIR`).

- Stereo: the Audio3d port is stereo when the host's default device has fewer than 6 channels, and with
  `PS_AUDIO=none`, which also keeps the run silent (the dump is still written).
- 7.1: the default device must report 6 channels or more (an 8-channel dump needs 8; a 5.1 device gets
  psport's 7.1 downmixed, `.6ch.f32`); on PipeWire or PulseAudio a 7.1 null sink set as default does it ([module-null-sink](https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/User/Modules/#module-null-sink),
  `channel_map` of eight). The 8-channel dump is in SDL's 7.1 order, FL FR FC LFE BL BR SL SR (psport
  remaps the PS4's L R C LFE SL SR BL BR on the way). The log says `Audio3d port 0 opened (... 8 ch out)`.
- `config.txt` names the folder only: psport's output mode is its host device's, so set both alike.
- psport renders a late reverb level of 0.5 dry and logs `Audio3d port attribute 0x10001 is accepted but
  not rendered` once: the second half of the run should sound like the first on psport, and whatever
  differs on the console is its reverb.

To compare, turn both into WAV (`ffmpeg -f f32le -ar 48000 -ac 8 -i a3d.port21.8ch.f32 psport.wav`, `-ac 2`
for stereo), cut each case, and compare the level of each channel per case (`ffmpeg -ss <start> -to <end>
-i psport.wav -af astats -f null -`), or listen to both in turn. What should match: which channels carry
each case and their level ratios (the console's absolute level, after the TV or receiver, will not). A
difference in where `right_30`, `right_90`, `back` or the elevated cases land, or in how wide spread π
sounds, is what decision #194 has to be adjusted to.
