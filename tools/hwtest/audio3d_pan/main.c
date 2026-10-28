/* Hardware test for libSceAudio3d's object rendering (kb #201, decision #194): one object plays a steady
 * 440 Hz tone at the front, 30 and 90 degrees to each side, behind, 45 degrees up, overhead and at the
 * origin, each with spreads 0, pi and 2pi and with the port's late reverb level (0x10001) at 0 and 0.5,
 * so that recordings of a PS4 and of psport (PS_AUDIO_DUMP) can be cut case by case and compared. Each
 * tone lasts 3 s between pauses of 1 s; three 1 kHz beeps open the run so that a recording can be aligned.
 * The output mode the console is set to (stereo or 7.1) is read from config.txt and only names the folder
 * and the expectations: the console renders to whatever its audio output is. The API checks go to
 * /data/psport-hwtest/audio3d_pan/<mode>/results.csv, each case's position and time to timeline.csv, and
 * the list of what to listen for to record.txt. */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SZ_OUT_ROOT "/data/psport-hwtest/audio3d_pan"
#define SYSMODULE_AUDIO3D 0x00A7
#define USER_SYSTEM 0xff
#define RATE_HZ 48000
#define GRAIN 256
#define TONE_HZ 440.0
#define BEEP_HZ 1000.0
#define LEVEL 0.25
#define FADE_SAMPLES 480
#define TONE_GRAINS 563
#define PAUSE_GRAINS 188
#define BEEP_GRAINS 19
#define BEEP_GAP_GRAINS 38
#define PI 3.14159265358979
#define TAU (2.0 * PI)

#define ATTRIBUTE_PCM 1
#define ATTRIBUTE_POSITION 3
#define ATTRIBUTE_SPREAD 4
#define ATTRIBUTE_GAIN 5
#define PORT_ATTRIBUTE_LATE_REVERB_LEVEL 0x10001
#define FORMAT_FLOAT 1
#define BUFFER_ADVANCE_AND_PUSH 2
#define BLOCKING_SYNC 1
#define AUDIO_OUT_ALREADY_INIT 0x8026000Eu
#define ERR_INVALID_PARAMETER 0x80EA0004u

/* Layouts as psport's src/audio/runtime_audio3d.c asserts them (shadPS4's audio3d.h has the same fields). */
typedef struct {
    uint64_t cb_this;
    uint32_t granularity, rate, max_objects, queue_depth, buffer_mode, pad, c_beds;
} Audio3dOpenParameters;

typedef struct {
    uint32_t format, pad;
    const void *p_samples;
    uint32_t c_samples;
} Audio3dPcm;

typedef struct {
    uint32_t id, pad;
    const void *p_value;
    uint64_t cb_value;
} Audio3dAttribute;

_Static_assert(sizeof(Audio3dOpenParameters) == 0x28, "open parameters");
_Static_assert(sizeof(Audio3dPcm) == 0x18, "pcm");
_Static_assert(sizeof(Audio3dAttribute) == 0x18, "attribute");

/* Declared here, not from the SDK's headers, as shadPS4's src/core/libraries/audio3d/audio3d.h (67dbc06)
 * and psport's src/audio/runtime_audio3d.c take them; README.md lists the ones to check on the SDK. */
int sceSysmoduleLoadModule(uint16_t id);
int sceAudioOutInit(void);
uint64_t sceKernelGetProcessTime(void);
int sceAudio3dInitialize(int64_t reserved);
int sceAudio3dTerminate(void);
int sceAudio3dGetDefaultOpenParameters(Audio3dOpenParameters *p_out_parameters);
int sceAudio3dPortOpen(int32_t user, const Audio3dOpenParameters *p_parameters, uint32_t *p_out_port);
int sceAudio3dPortClose(uint32_t h_port);
int sceAudio3dPortSetAttribute(uint32_t h_port, uint32_t attribute, const void *p_value, uint64_t cb_value);
int sceAudio3dPortAdvance(uint32_t h_port);
int sceAudio3dPortPush(uint32_t h_port, uint32_t blocking);
int sceAudio3dPortFlush(uint32_t h_port);
int sceAudio3dObjectReserve(uint32_t h_port, uint32_t *p_out_object);
int sceAudio3dObjectUnreserve(uint32_t h_port, uint32_t h_object);
int sceAudio3dObjectSetAttributes(uint32_t h_port, uint32_t h_object, uint64_t c_attributes,
                                  const Audio3dAttribute *p_attributes);

typedef struct {
    const char *sz_name;
    double azimuth_deg, elevation_deg;
    int is_origin;
    const char *sz_stereo, *sz_surround;
} Position;

/* What psport renders with spread 0 (decision #194): the 7.1 ring C 0, R 30, SR 90, BR 150, BL 210, SL 270,
 * L 330 degrees, and the fold to stereo with C, SL and BL at -3 dB into L. Elevated positions and the origin
 * have none since psport turns their elevation into an arc (play_case). */
static const Position s_positions[] = {
    {"front", 0, 0, 0, "L = R", "C"},
    {"right_30", 30, 0, 0, "R only", "R"},
    {"left_30", -30, 0, 0, "L only", "L"},
    {"right_90", 90, 0, 0, "R only, 3 dB under right_30", "SR"},
    {"left_90", -90, 0, 0, "L only, 3 dB under left_30", "SL"},
    {"back", 180, 0, 0, "L = R, 3 dB under front", "BL = BR"},
    {"above_45", 0, 45, 0, NULL, NULL},
    {"overhead", 0, 90, 0, NULL, NULL},
    {"origin", 0, 0, 1, NULL, NULL},
};
#define C_POSITIONS (sizeof(s_positions) / sizeof(s_positions[0]))
static const double s_spreads[] = {0.0, PI, TAU};
static const float s_late_reverb_levels[] = {0.0f, 0.5f};

static FILE *s_p_results, *s_p_timeline, *s_p_record;
static int s_c_mismatches;
static uint32_t s_h_port, s_h_object;
static uint64_t s_c_samples, s_start_us;
static float s_grain[GRAIN];

typedef struct {
    const char *sz_call;
    uint32_t c_errors, first_rc;
} CallErrors;
static CallErrors s_pcm_errors = {"sceAudio3dObjectSetAttributes(PCM)", 0, 0};
static CallErrors s_advance_errors = {"sceAudio3dPortAdvance", 0, 0};
static CallErrors s_push_errors = {"sceAudio3dPortPush", 0, 0};

static void record(const char *sz_case, uint32_t value, uint32_t expected) {
    const int is_match = value == expected;
    s_c_mismatches += !is_match;
    fprintf(s_p_results, "%s,%#x,%#x,%s\n", sz_case, value, expected, is_match ? "ok" : "MISMATCH");
}

static void count_error(CallErrors *p_errors, int rc) {
    if (rc >= 0) return;
    if (!p_errors->c_errors++) {
        p_errors->first_rc = (uint32_t)rc;
        printf("audio3d_pan: %s failed %#x at sample %llu\n", p_errors->sz_call, (unsigned)rc,
               (unsigned long long)s_c_samples);
    }
}

/* Push with SYNC waits for room in the port's queue, so the console's output clock paces the loop and
 * s_c_samples is the time the recording sees. */
static void play_grain(void) {
    const Audio3dPcm pcm = {FORMAT_FLOAT, 0, s_grain, GRAIN};
    const Audio3dAttribute attribute = {ATTRIBUTE_PCM, 0, &pcm, sizeof(pcm)};
    count_error(&s_pcm_errors, sceAudio3dObjectSetAttributes(s_h_port, s_h_object, 1, &attribute));
    count_error(&s_advance_errors, sceAudio3dPortAdvance(s_h_port));
    count_error(&s_push_errors, sceAudio3dPortPush(s_h_port, BLOCKING_SYNC));
    s_c_samples += GRAIN;
}

static void play_silence(int c_grains) {
    memset(s_grain, 0, sizeof(s_grain));
    for (int i = 0; i < c_grains; ++i) play_grain();
}

/* Raised-cosine ends, since a tone cut at full level clicks and the click would localize differently. */
static void play_tone(double frequency_hz, int c_grains) {
    const uint64_t c_tone = (uint64_t)c_grains * GRAIN;
    for (uint64_t i_sample = 0; i_sample < c_tone; i_sample += GRAIN) {
        for (int i = 0; i < GRAIN; ++i) {
            const uint64_t n = i_sample + (uint64_t)i;
            const uint64_t from_edge = n < c_tone - 1 - n ? n : c_tone - 1 - n;
            const double envelope =
                from_edge >= FADE_SAMPLES ? 1.0 : 0.5 - 0.5 * cos(PI * (double)from_edge / FADE_SAMPLES);
            s_grain[i] = (float)(LEVEL * envelope * sin(TAU * frequency_hz * (double)n / RATE_HZ));
        }
        play_grain();
    }
}

static int set_placement(const float *p_position, float spread) {
    const float gain = 1.0f;
    const Audio3dAttribute attributes[] = {
        {ATTRIBUTE_POSITION, 0, p_position, 3 * sizeof(float)},
        {ATTRIBUTE_SPREAD, 0, &spread, sizeof(spread)},
        {ATTRIBUTE_GAIN, 0, &gain, sizeof(gain)},
    };
    return sceAudio3dObjectSetAttributes(s_h_port, s_h_object, 3, attributes);
}

static void format_time(char *sz_out, size_t cb_out, uint64_t c_samples) {
    const double seconds = (double)c_samples / RATE_HZ;
    const int minutes = (int)(seconds / 60.0);
    snprintf(sz_out, cb_out, "%d:%04.1f", minutes, seconds - 60.0 * minutes);
}

static void play_sync(void) {
    const float front[3] = {0.0f, 0.0f, 1.0f};
    const float level = 0.0f;
    record("sync_late_reverb_0", sceAudio3dPortSetAttribute(s_h_port, PORT_ATTRIBUTE_LATE_REVERB_LEVEL, &level,
                                                            sizeof(level)), 0);
    record("sync_placement", set_placement(front, 0.0f), 0);
    for (int i = 0; i < 3; ++i) {
        play_tone(BEEP_HZ, BEEP_GRAINS);
        play_silence(BEEP_GAP_GRAINS);
    }
}

static void play_case(int i_case, const Position *p_position, double spread, float late_reverb, int is_surround) {
    const double azimuth = p_position->azimuth_deg * PI / 180.0, elevation = p_position->elevation_deg * PI / 180.0;
    float xyz[3] = {0.0f, 0.0f, 0.0f};
    if (!p_position->is_origin) {
        xyz[0] = (float)(sin(azimuth) * cos(elevation));
        xyz[1] = (float)sin(elevation);
        xyz[2] = (float)(cos(azimuth) * cos(elevation));
    }
    char sz_case[96];
    snprintf(sz_case, sizeof(sz_case), "case_%02d_%s_spread_%.2f_reverb_%.1f_placement", i_case, p_position->sz_name,
             spread, late_reverb);
    record(sz_case, (uint32_t)set_placement(xyz, (float)spread), 0);
    snprintf(sz_case, sizeof(sz_case), "case_%02d_%s_spread_%.2f_reverb_%.1f_late_reverb", i_case,
             p_position->sz_name, spread, late_reverb);
    record(sz_case, (uint32_t)sceAudio3dPortSetAttribute(s_h_port, PORT_ATTRIBUTE_LATE_REVERB_LEVEL, &late_reverb,
                                                         sizeof(late_reverb)), 0);
    play_silence(PAUSE_GRAINS);

    const uint64_t start = s_c_samples;
    const double wall_start_s = (double)(sceKernelGetProcessTime() - s_start_us) / 1e6;
    play_tone(TONE_HZ, TONE_GRAINS);
    const uint64_t end = s_c_samples;
    play_silence(PAUSE_GRAINS);

    fprintf(s_p_timeline, "%d,%s,%g,%g,%.6f,%.6f,%.6f,%.6f,%.1f,%.6f,%.6f,%.3f\n", i_case, p_position->sz_name,
            p_position->azimuth_deg, p_position->elevation_deg, xyz[0], xyz[1], xyz[2], spread, late_reverb,
            (double)start / RATE_HZ, (double)end / RATE_HZ, wall_start_s);

    /* psport widens the arc to the spread, or to 2pi(1 - cos elevation) when that is wider (decision #194). */
    const double elevation_arc = p_position->is_origin ? TAU : TAU * (1.0 - cos(elevation));
    const double arc_deg = (spread > elevation_arc ? spread : elevation_arc) * 180.0 / PI;
    char sz_start[16], sz_end[16], sz_expected[96];
    format_time(sz_start, sizeof(sz_start), start);
    format_time(sz_end, sizeof(sz_end), end);
    if (arc_deg < 1.0)
        snprintf(sz_expected, sizeof(sz_expected), "%s", is_surround ? p_position->sz_surround : p_position->sz_stereo);
    else if (arc_deg > 359.0)
        snprintf(sz_expected, sizeof(sz_expected), "the whole ring%s", is_surround ? "" : ", folded: L = R");
    else
        snprintf(sz_expected, sizeof(sz_expected), "an arc of %.0f degrees around azimuth %g%s", arc_deg,
                 p_position->azimuth_deg, is_surround ? "" : ", folded");
    fprintf(s_p_record, "%3d  %8s  %8s  %-9s az %4g el %3g  spread %4.2f  late reverb %.1f  psport: %s\n", i_case,
            sz_start, sz_end, p_position->sz_name, p_position->azimuth_deg, p_position->elevation_deg, spread,
            late_reverb, sz_expected);
}

/* config.txt holds `output=stereo` or `output=7.1`, as the console's audio output is set; without it the
 * run is taken as stereo. */
static int read_output_mode(const char **psz_out_mode) {
    *psz_out_mode = "stereo";
    FILE *p_config = fopen(SZ_OUT_ROOT "/config.txt", "r");
    if (!p_config) {
        printf("audio3d_pan: no %s/config.txt, taking the output as stereo\n", SZ_OUT_ROOT);
        return 0;
    }
    char sz_line[64] = "";
    const int is_read = fgets(sz_line, sizeof(sz_line), p_config) != NULL;
    fclose(p_config);
    sz_line[strcspn(sz_line, "\r\n")] = '\0';
    if (is_read && !strcmp(sz_line, "output=7.1")) *psz_out_mode = "7.1";
    else if (!is_read || strcmp(sz_line, "output=stereo")) {
        printf("audio3d_pan: config.txt says \"%s\", expected output=stereo or output=7.1\n", sz_line);
        return -1;
    }
    return 0;
}

static int open_outputs(const char *sz_mode) {
    char sz_dir[256], sz_path[288];
    snprintf(sz_dir, sizeof(sz_dir), "%s/%s", SZ_OUT_ROOT, sz_mode);
    mkdir(sz_dir, 0777);
    snprintf(sz_path, sizeof(sz_path), "%s/results.csv", sz_dir);
    s_p_results = fopen(sz_path, "w");
    snprintf(sz_path, sizeof(sz_path), "%s/timeline.csv", sz_dir);
    s_p_timeline = fopen(sz_path, "w");
    snprintf(sz_path, sizeof(sz_path), "%s/record.txt", sz_dir);
    s_p_record = fopen(sz_path, "w");
    if (!s_p_results || !s_p_timeline || !s_p_record) {
        printf("audio3d_pan: cannot write to %s (errno %d)\n", sz_dir, errno);
        return -1;
    }
    fprintf(s_p_results, "case,value,expected,match\n");
    fprintf(s_p_timeline,
            "index,case,azimuth_deg,elevation_deg,x,y,z,spread_rad,late_reverb,tone_start_s,tone_end_s,wall_start_s\n");
    fprintf(s_p_record,
            "Audio3d pan test (kb #201), console output %s. t = 0 is the onset of the first of three 1 kHz beeps;\n"
            "each case is a 440 Hz tone between pauses of 1 s. Listen for, or cut, each tone and compare it with\n"
            "what psport renders (decision #194). Positions: +x right, +y up, +z front, azimuth clockwise from the\n"
            "front seen from above. psport renders late reverb 0.5 dry, as 0: any reverb is the console's.\n\n",
            sz_mode);
    return 0;
}

static int open_port(void) {
    Audio3dOpenParameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    record("default_parameters", (uint32_t)sceAudio3dGetDefaultOpenParameters(&parameters), 0);
    record("default_parameters_size", (uint32_t)parameters.cb_this, 0x20);
    record("default_parameters_granularity", parameters.granularity, 0x100);
    record("default_parameters_max_objects", parameters.max_objects, 512);
    record("default_parameters_queue_depth", parameters.queue_depth, 2);
    record("default_parameters_buffer_mode", parameters.buffer_mode, BUFFER_ADVANCE_AND_PUSH);
    parameters = (Audio3dOpenParameters){sizeof(parameters), GRAIN, 0, 8, 2, BUFFER_ADVANCE_AND_PUSH, 0, 2};
    const int rc = sceAudio3dPortOpen(USER_SYSTEM, &parameters, &s_h_port);
    record("port_open", (uint32_t)rc, 0);
    if (rc < 0) return rc;
    const int rc_object = sceAudio3dObjectReserve(s_h_port, &s_h_object);
    record("object_reserve", (uint32_t)rc_object, 0);
    if (rc_object < 0) return rc_object;
    const float too_high = 1.5f;
    const int rc_too_high =
        sceAudio3dPortSetAttribute(s_h_port, PORT_ATTRIBUTE_LATE_REVERB_LEVEL, &too_high, sizeof(too_high));
    record("late_reverb_1_5_rejected", (uint32_t)rc_too_high, ERR_INVALID_PARAMETER);
    return 0;
}

static void record_errors(const CallErrors *p_errors, const char *sz_case) {
    record(sz_case, p_errors->c_errors, 0);
    if (p_errors->c_errors)
        printf("audio3d_pan: %s failed %u times, first %#x\n", p_errors->sz_call, (unsigned)p_errors->c_errors,
               (unsigned)p_errors->first_rc);
}

int main(void) {
    const int rc_module = sceSysmoduleLoadModule(SYSMODULE_AUDIO3D);
    if (rc_module != 0) {
        printf("audio3d_pan: loading libSceAudio3d failed: %#x\n", (unsigned)rc_module);
        return 1;
    }
    mkdir("/data/psport-hwtest", 0777);
    mkdir(SZ_OUT_ROOT, 0777);
    const char *sz_mode;
    if (read_output_mode(&sz_mode) || open_outputs(sz_mode)) return 1;
    const int is_surround = !strcmp(sz_mode, "7.1");

    const int rc_audio_out = sceAudioOutInit();
    record("audio_out_init", (uint32_t)rc_audio_out == AUDIO_OUT_ALREADY_INIT ? 0 : (uint32_t)rc_audio_out, 0);
    const int rc_init = sceAudio3dInitialize(0);
    record("initialize", (uint32_t)rc_init, 0);
    if (rc_init < 0 || open_port() < 0) {
        printf("audio3d_pan: no Audio3d port, see results.csv\n");
        fclose(s_p_record);
        fclose(s_p_timeline);
        fclose(s_p_results);
        return 1;
    }

    s_start_us = sceKernelGetProcessTime();
    play_sync();
    int i_case = 0;
    for (size_t i_reverb = 0; i_reverb < 2; ++i_reverb)
        for (size_t i_spread = 0; i_spread < 3; ++i_spread)
            for (size_t i_position = 0; i_position < C_POSITIONS; ++i_position)
                play_case(++i_case, &s_positions[i_position], s_spreads[i_spread], s_late_reverb_levels[i_reverb],
                          is_surround);
    play_silence(PAUSE_GRAINS);

    record("port_flush", (uint32_t)sceAudio3dPortFlush(s_h_port), 0);
    record_errors(&s_pcm_errors, "grain_pcm_errors");
    record_errors(&s_advance_errors, "grain_advance_errors");
    record_errors(&s_push_errors, "grain_push_errors");
    record("object_unreserve", (uint32_t)sceAudio3dObjectUnreserve(s_h_port, s_h_object), 0);
    record("port_close", (uint32_t)sceAudio3dPortClose(s_h_port), 0);
    record("terminate", (uint32_t)sceAudio3dTerminate(), 0);
    fclose(s_p_record);
    fclose(s_p_timeline);
    fclose(s_p_results);
    printf("audio3d_pan: done, %d cases, %d mismatch(es) against what psport answers\n", i_case, s_c_mismatches);
    return s_c_mismatches != 0;
}
