/* Hardware test for libSceNgs2's pan, geom and FFT (kb #1099, phase F7 of #625): the checks and error codes
 * of the nine exports, the pan's volume matrices for every default layout, unit, angle grid and matrix
 * format, the geom's listener frames and source attributes for each rolloff model, cone and flag, and the
 * FFT of known signals in every size and order, so that a PS4, psport with the firmware module
 * (PS_LLE=libSceNgs2) and psport's native Ngs2 (PS_NGS2=native) can be compared word for word. The
 * checks, with the value rea-ngs2 spec 12 predicts, go to /data/psport-hwtest/ngs2_pan/results.csv; the
 * raw outputs, as IEEE-754 words, to pan.csv, geom.csv and fft.csv in the same folder. */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SZ_OUT_DIR "/data/psport-hwtest/ngs2_pan"
#define SYSMODULE_NGS2 0x000B
#define E(low) (0x804A0000u | (low))
#define RADIANS 6.28318548f
#define MAX_POINTS 4096

typedef struct {
    float a_speaker_angle[8];
    float unit_angle;
    uint32_t c_speakers;
} Ngs2PanWork;

typedef struct {
    float angle, distance, fbw_level, lfe_level;
} Ngs2PanParam;

typedef struct {
    float x, y, z;
} Ngs2Vector;

typedef struct {
    Ngs2Vector position, front, up, velocity;
    float sound_speed;
    uint32_t reserved[2];
} Ngs2ListenerParam;

typedef struct {
    float matrix[4][4];
    Ngs2Vector velocity;
    float sound_speed;
    uint32_t coordinate, reserved[3];
} Ngs2ListenerWork;

typedef struct {
    Ngs2Vector position, velocity, direction;
    float inner_level, inner_angle, outer_level, outer_angle;
    uint32_t model;
    float max_distance, rolloff_factor, reference_distance;
    float doppler_factor, fbw_level, lfe_level, max_level, min_level, radius;
    uint32_t c_speakers, matrix_format, reserved[2];
} Ngs2SourceParam;

typedef struct {
    float pitch_ratio;
    float a_level[64];
    Ngs2Vector a3d_position;
    float a3d_volume;
    uint32_t reserved[8];
} Ngs2Attribute;

_Static_assert(sizeof(Ngs2PanWork) == 0x28, "pan work");
_Static_assert(sizeof(Ngs2ListenerParam) == 0x3c, "listener param");
_Static_assert(sizeof(Ngs2ListenerWork) == 0x60, "listener work");
_Static_assert(sizeof(Ngs2SourceParam) == 0x6c, "source param");
_Static_assert(offsetof(Ngs2SourceParam, model) == 0x34, "rolloff");
_Static_assert(offsetof(Ngs2SourceParam, matrix_format) == 0x60, "matrix format");
_Static_assert(sizeof(Ngs2Attribute) == 0x134, "attribute");
_Static_assert(offsetof(Ngs2Attribute, a3d_position) == 0x104, "a3d");

int sceSysmoduleLoadModule(uint16_t id);
int sceNgs2PanInit(Ngs2PanWork *p_work, const float *p_angles, uint32_t c_speakers, float unit_angle);
int sceNgs2PanGetVolumeMatrix(const Ngs2PanWork *p_work, const Ngs2PanParam *p_params, uint32_t c_params,
                              uint32_t matrix_format, float *p_out_levels);
int sceNgs2GeomResetListenerParam(Ngs2ListenerParam *p_param);
int sceNgs2GeomResetSourceParam(Ngs2SourceParam *p_param);
int sceNgs2GeomCalcListener(const Ngs2ListenerParam *p_param, Ngs2ListenerWork *p_out_work, uint32_t coordinate);
int sceNgs2GeomApply(const Ngs2ListenerWork *p_listener, const Ngs2SourceParam *p_source, Ngs2Attribute *p_out,
                     uint32_t flags);
int sceNgs2FftQuerySize(uint32_t log2_points, uint32_t flags, size_t *p_out_cb);
int sceNgs2FftInit(void *p_buffer, size_t cb_buffer, uint32_t log2_points, uint32_t flags, void **pp_out_work);
int sceNgs2FftProcess(void *p_work, const float *p_in, float *p_out, float *p_scratch, uint64_t unused,
                      uint32_t flags);

static FILE *s_p_results;
static int s_c_mismatches;

static void record(const char *sz_case, uint32_t value, uint32_t expected) {
    const int is_match = value == expected;
    s_c_mismatches += !is_match;
    fprintf(s_p_results, "%s,%#x,%#x,%s\n", sz_case, value, expected, is_match ? "ok" : "MISMATCH");
}

static void record_near(const char *sz_case, double value, double expected, double tolerance) {
    const int is_match = fabs(value - expected) <= tolerance;
    s_c_mismatches += !is_match;
    fprintf(s_p_results, "%s,%.9g,%.9g,%s\n", sz_case, value, expected, is_match ? "ok" : "MISMATCH");
}

static uint32_t word_of(float value) {
    uint32_t word;
    memcpy(&word, &value, sizeof(word));
    return word;
}

/* One raw row: a case name, the call's answer and the words it wrote. */
static void dump(FILE *p_file, const char *sz_case, int rc, const void *p_words, size_t c_words) {
    const uint32_t *p = p_words;
    fprintf(p_file, "%s,%#x", sz_case, (unsigned)rc);
    for (size_t i = 0; i < c_words; ++i) fprintf(p_file, ",%08x", p[i]);
    fprintf(p_file, "\n");
}

static uint32_t s_lcg = 1099;
static float random_in(float lo, float hi) {
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(s_lcg >> 8) / 16777216.0f;
}

static void test_pan(FILE *p_dump) {
    Ngs2PanWork work;
    const float stereo[2] = {330.0f, 30.0f};
    record("pan_init_null_work", sceNgs2PanInit(NULL, stereo, 2, 360.0f), E(0x053));
    record("pan_init_bad_unit", sceNgs2PanInit(&work, stereo, 2, 100.0f), E(0x910));
    record("pan_init_three_speakers", sceNgs2PanInit(&work, stereo, 3, 360.0f), E(0x911));
    record("pan_init_default_one_speaker", sceNgs2PanInit(&work, NULL, 1, 360.0f), E(0x911));
    record("pan_init_default_any_unit", sceNgs2PanInit(&work, NULL, 2, 100.0f), 0);
    record("pan_init_default_zero_is_seven", (sceNgs2PanInit(&work, NULL, 0, 360.0f), work.c_speakers), 7);
    record("pan_init_custom_stereo", sceNgs2PanInit(&work, stereo, 2, 360.0f), 0);
    record("pan_init_custom_stereo_order", word_of(work.a_speaker_angle[0]), word_of(30.0f));
    record("pan_init_custom_stereo_sentinel", word_of(work.a_speaker_angle[2]), word_of(390.0f));
    const float wrapped[2] = {-30.0f, 750.0f};
    sceNgs2PanInit(&work, wrapped, 2, 360.0f);
    record("pan_init_wraps_negative", word_of(work.a_speaker_angle[1]), word_of(330.0f));
    record("pan_init_wraps_over", word_of(work.a_speaker_angle[0]), word_of(30.0f));
    /* Unconfirmed (spec 12 §1.1 b): with radians the default layout's closing sentinel stays in degrees. */
    sceNgs2PanInit(&work, NULL, 2, RADIANS);
    record("pan_init_radians_sentinel_degrees", word_of(work.a_speaker_angle[2]), word_of(450.0f));
    sceNgs2PanInit(&work, NULL, 0, RADIANS);
    record("pan_init_radians_zero_left_in_degrees", word_of(work.a_speaker_angle[1]), word_of(40.0f));

    static const uint32_t layouts[] = {0, 2, 4, 5, 7};
    for (int i_layout = 0; i_layout < 5; ++i_layout) {
        for (int is_radians = 0; is_radians < 2; ++is_radians) {
            char sz_case[64];
            memset(&work, 0xcd, sizeof(work));
            const int rc = sceNgs2PanInit(&work, NULL, layouts[i_layout], is_radians ? RADIANS : 360.0f);
            snprintf(sz_case, sizeof(sz_case), "init_default_%u_%s", (unsigned)layouts[i_layout], is_radians ? "rad" : "deg");
            dump(p_dump, sz_case, rc, &work, sizeof(work) / 4);
        }
    }

    Ngs2PanParam param = {30.0f, 1.0f, 1.0f, 0.0f};
    float levels[64];
    sceNgs2PanInit(&work, NULL, 2, 360.0f);
    record("pan_matrix_null_work", sceNgs2PanGetVolumeMatrix(NULL, &param, 1, 2, levels), E(0x913));
    record("pan_matrix_null_param", sceNgs2PanGetVolumeMatrix(&work, NULL, 1, 2, levels), E(0x914));
    record("pan_matrix_null_out", sceNgs2PanGetVolumeMatrix(&work, &param, 1, 2, NULL), E(0x053));
    record("pan_matrix_format_3", sceNgs2PanGetVolumeMatrix(&work, &param, 1, 3, levels), E(0x912));
    record("pan_matrix_nine_params", sceNgs2PanGetVolumeMatrix(&work, &param, 9, 2, levels), E(0x052));
    record("pan_matrix_stereo_30", sceNgs2PanGetVolumeMatrix(&work, &param, 1, 2, levels), 0);
    record_near("pan_matrix_stereo_30_left", levels[0], 0.5, 1e-6);
    record_near("pan_matrix_stereo_30_right", levels[1], 0.866025404, 1e-6);
    param.angle = 90.0f;
    param.distance = 0.0f;
    sceNgs2PanGetVolumeMatrix(&work, &param, 1, 2, levels);
    record_near("pan_matrix_stereo_spread_left", levels[0], 0.707106781, 1e-6);
    record_near("pan_matrix_stereo_spread_right", levels[1], 0.707106781, 1e-6);

    static const uint32_t formats[] = {1, 2, 6, 8};
    static const float distances[] = {1.0f, 0.5f, 0.0f, -1.0f};
    for (int i_layout = 1; i_layout < 5; ++i_layout) {
        for (int is_radians = 0; is_radians < 2; ++is_radians) {
            const float unit = is_radians ? RADIANS : 360.0f;
            sceNgs2PanInit(&work, NULL, layouts[i_layout], unit);
            for (int i_format = 0; i_format < 4; ++i_format) {
                for (int i_distance = 0; i_distance < 4; ++i_distance) {
                    for (int degrees = -30; degrees < 360; degrees += 15) {
                        Ngs2PanParam p = {degrees * unit / 360.0f, distances[i_distance], 1.0f, 0.5f};
                        char sz_case[64];
                        memset(levels, 0xcd, sizeof(levels));
                        const int rc = sceNgs2PanGetVolumeMatrix(&work, &p, 1, formats[i_format], levels);
                        snprintf(sz_case, sizeof(sz_case), "matrix_%u_%s_f%u_d%d_a%d", (unsigned)layouts[i_layout],
                                 is_radians ? "rad" : "deg", (unsigned)formats[i_format], i_distance, degrees);
                        dump(p_dump, sz_case, rc, levels, formats[i_format]);
                    }
                }
            }
        }
    }
    /* Several parameters at once: the matrix is laid out by output channel, then by parameter. */
    Ngs2PanParam several[8];
    for (int i = 0; i < 8; ++i) several[i] = (Ngs2PanParam){i * 45.0f, random_in(-1.0f, 1.0f), 0.8f, 0.3f};
    sceNgs2PanInit(&work, NULL, 7, 360.0f);
    memset(levels, 0xcd, sizeof(levels));
    dump(p_dump, "matrix_7_deg_f8_eight_params", sceNgs2PanGetVolumeMatrix(&work, several, 8, 8, levels), levels, 64);
}

static void test_geom(FILE *p_dump) {
    Ngs2ListenerParam listener_param;
    Ngs2SourceParam source;
    Ngs2ListenerWork listener;
    Ngs2Attribute attr;
    memset(&listener_param, 0xcd, sizeof(listener_param));
    memset(&source, 0xcd, sizeof(source));
    record("geom_reset_listener", sceNgs2GeomResetListenerParam(&listener_param), 0);
    record("geom_reset_listener_speed", word_of(listener_param.sound_speed), 0x43aa251f);
    record("geom_reset_listener_front_z", word_of(listener_param.front.z), word_of(-1.0f));
    record("geom_reset_listener_reserved_kept", listener_param.reserved[0], 0xcdcdcdcd);
    record("geom_reset_source", sceNgs2GeomResetSourceParam(&source), 0);
    record("geom_reset_source_model", source.model, 5);
    record("geom_reset_source_outer_level", word_of(source.outer_level), 0x3e4ccccd);
    record("geom_reset_source_speakers", source.c_speakers, 7);
    record("geom_reset_source_format", source.matrix_format, 8);
    record("geom_reset_null", sceNgs2GeomResetSourceParam(NULL), E(0x053));
    dump(p_dump, "reset_listener", 0, &listener_param, sizeof(listener_param) / 4);
    dump(p_dump, "reset_source", 0, &source, sizeof(source) / 4);

    record("geom_listener_null_param", sceNgs2GeomCalcListener(NULL, &listener, 0), E(0x921));
    record("geom_listener_null_work", sceNgs2GeomCalcListener(&listener_param, NULL, 0), E(0x053));
    record("geom_listener_coordinate_2", sceNgs2GeomCalcListener(&listener_param, &listener, 2), E(0x923));
    record("geom_listener_identity", sceNgs2GeomCalcListener(&listener_param, &listener, 0), 0);
    /* The firmware's fast inverse square root leaves unit vectors about 0.17 % short. */
    record_near("geom_listener_identity_scale", listener.matrix[0][0], 1.0, 4e-3);
    for (int i = 0; i < 6; ++i) {
        Ngs2ListenerParam p = listener_param;
        p.position = (Ngs2Vector){random_in(-20, 20), random_in(-20, 20), random_in(-20, 20)};
        p.front = (Ngs2Vector){random_in(-1, 1), random_in(-1, 1), random_in(-1, 1)};
        p.up = (Ngs2Vector){random_in(-1, 1), random_in(-1, 1), random_in(-1, 1)};
        p.velocity = (Ngs2Vector){random_in(-10, 10), random_in(-10, 10), random_in(-10, 10)};
        p.sound_speed = i == 5 ? -1.0f : 343.0f;
        char sz_case[64];
        memset(&listener, 0xcd, sizeof(listener));
        snprintf(sz_case, sizeof(sz_case), "listener_%d_%s", i, i % 2 ? "left" : "right");
        dump(p_dump, sz_case, sceNgs2GeomCalcListener(&p, &listener, i % 2), &listener, sizeof(listener) / 4);
    }

    sceNgs2GeomResetListenerParam(&listener_param);
    listener_param.velocity = (Ngs2Vector){0.0f, 0.0f, -5.0f};
    sceNgs2GeomCalcListener(&listener_param, &listener, 0);
    sceNgs2GeomResetSourceParam(&source);
    record("geom_apply_null_listener", sceNgs2GeomApply(NULL, &source, &attr, 4), E(0x921));
    record("geom_apply_null_source", sceNgs2GeomApply(&listener, NULL, &attr, 4), E(0x922));
    record("geom_apply_null_attribute", sceNgs2GeomApply(&listener, &source, NULL, 4), E(0x053));
    record("geom_apply_flags_0", sceNgs2GeomApply(&listener, &source, &attr, 0), E(0x923));
    record("geom_apply_flags_matrix_and_harmonics", sceNgs2GeomApply(&listener, &source, &attr, 0x14), E(0x923));
    record("geom_apply_flags_0x20", sceNgs2GeomApply(&listener, &source, &attr, 0x20), E(0x923));
    source.model = 6;
    record("geom_apply_model_6", sceNgs2GeomApply(&listener, &source, &attr, 8), E(0x920));
    source.model = 3;
    source.reference_distance = 200.0f;
    record("geom_apply_reference_over_max", sceNgs2GeomApply(&listener, &source, &attr, 8), E(0x920));
    sceNgs2GeomResetSourceParam(&source);
    source.inner_angle = 400.0f;
    record("geom_apply_cone_400", sceNgs2GeomApply(&listener, &source, &attr, 1 | 8), E(0x924));
    source.inner_angle = 90.0f;
    source.outer_angle = 60.0f;
    record("geom_apply_cone_inner_over_outer", sceNgs2GeomApply(&listener, &source, &attr, 1 | 8), E(0x924));
    sceNgs2GeomResetSourceParam(&source);
    source.position = (Ngs2Vector){0.0f, 0.0f, -2.0f};
    source.model = 0;
    record("geom_apply_inverse_2m", sceNgs2GeomApply(&listener, &source, &attr, 8), 0);
    record_near("geom_apply_inverse_2m_volume", attr.a3d_volume, 0.5, 5e-3);
    record_near("geom_apply_inverse_2m_z", attr.a3d_position.z, 2.0, 5e-3);
    sceNgs2GeomApply(&listener, &source, &attr, 2);
    /* The reset listener's 340.29 m/s, the listener's 5 m/s toward the source shortened 0.17 % by its frame. */
    record_near("geom_apply_doppler_listener_approaching", attr.pitch_ratio, (340.29 + 5.0) / 340.29, 1e-4);

    static const uint32_t flag_sets[] = {1 | 2 | 4 | 8, 1 | 2 | 8 | 0x10, 4, 0x10, 2, 8};
    static const uint32_t speakers[] = {7, 2, 4, 5, 0};
    static const uint32_t formats[] = {8, 2, 6, 1};
    for (int i_case = 0; i_case < 240; ++i_case) {
        sceNgs2GeomResetSourceParam(&source);
        const float azimuth = (float)(i_case % 24) * 0.261799388f;
        const float range = 0.25f + (float)(i_case % 7) * 3.0f;
        source.position = (Ngs2Vector){range * sinf(azimuth), random_in(-2, 2), -range * cosf(azimuth)};
        source.velocity = (Ngs2Vector){random_in(-20, 20), random_in(-20, 20), random_in(-20, 20)};
        source.direction = (Ngs2Vector){random_in(-1, 1), random_in(-1, 1), random_in(-1, 1)};
        source.model = (uint32_t)(i_case % 6);
        source.max_distance = 15.0f;
        source.reference_distance = 1.5f;
        source.rolloff_factor = 0.5f + (float)(i_case % 3);
        source.radius = (float)(i_case % 5);
        source.lfe_level = 0.25f;
        source.min_level = 0.05f;
        source.c_speakers = speakers[i_case % 5];
        source.matrix_format = formats[i_case % 4];
        Ngs2ListenerParam p = listener_param;
        if (i_case % 8 == 7) {
            p.front = (Ngs2Vector){random_in(-1, 1), random_in(-1, 1), random_in(-1, 1)};
            p.position = (Ngs2Vector){random_in(-5, 5), random_in(-5, 5), random_in(-5, 5)};
        }
        sceNgs2GeomCalcListener(&p, &listener, (uint32_t)(i_case / 120));
        char sz_case[64];
        memset(&attr, 0xcd, sizeof(attr));
        const uint32_t flags = flag_sets[i_case % 6];
        snprintf(sz_case, sizeof(sz_case), "apply_%d_flags_%#x", i_case, (unsigned)flags);
        dump(p_dump, sz_case, sceNgs2GeomApply(&listener, &source, &attr, flags), &attr, sizeof(attr) / 4);
    }
}

static float s_in[2 * MAX_POINTS], s_out[2 * MAX_POINTS], s_scratch[2 * MAX_POINTS];
static uint8_t s_work[8 * MAX_POINTS + 0x40] __attribute__((aligned(64)));

static void signal(int kind, uint32_t c_points) {
    for (uint32_t i = 0; i < c_points; ++i) {
        const double t = (double)i / c_points;
        s_in[i] = kind == 0   ? (i == 0 ? 1.0f : 0.0f)
                  : kind == 1 ? (float)cos(2.0 * M_PI * 3.0 * t)
                  : kind == 2 ? (float)sin(2.0 * M_PI * 5.0 * t)
                  : kind == 3 ? 1.0f
                  : kind == 4 ? (i % 2 ? -1.0f : 1.0f)
                              : random_in(-1.0f, 1.0f);
    }
}

static void test_fft(FILE *p_dump) {
    size_t cb = 0;
    record("fft_size_7", sceNgs2FftQuerySize(7, 2, &cb), 0xffffffffu);
    record("fft_size_13", sceNgs2FftQuerySize(13, 2, &cb), 0xffffffffu);
    for (uint32_t log2 = 8; log2 <= 12; ++log2) {
        char sz_case[64];
        snprintf(sz_case, sizeof(sz_case), "fft_size_%u_real", (unsigned)log2);
        sceNgs2FftQuerySize(log2, 2, &cb);
        record(sz_case, (uint32_t)cb, (4u << log2) + 0x40);
        snprintf(sz_case, sizeof(sz_case), "fft_size_%u_complex", (unsigned)log2);
        sceNgs2FftQuerySize(log2, 0, &cb);
        record(sz_case, (uint32_t)cb, (8u << log2) + 0x40);
    }
    void *h_work = NULL;
    sceNgs2FftQuerySize(10, 2, &cb);
    record("fft_init_short_buffer", sceNgs2FftInit(s_work, cb - 1, 10, 2, &h_work), 0xffffffffu);
    record("fft_init_bad_size", sceNgs2FftInit(s_work, sizeof(s_work), 14, 2, &h_work), 0xffffffffu);

    for (uint32_t log2 = 8; log2 <= 12; ++log2) {
        const uint32_t n = 1u << log2;
        sceNgs2FftQuerySize(log2, 2, &cb);
        record("fft_init", sceNgs2FftInit(s_work, cb, log2, 2, &h_work), 0);
        char sz_case[64];
        signal(0, n);
        sceNgs2FftProcess(h_work, s_in, s_out, s_scratch, 0, 1 | 2);
        snprintf(sz_case, sizeof(sz_case), "fft_%u_impulse_flat", (unsigned)n);
        record_near(sz_case, s_out[2 * 7], 1.0, 1e-6);
        signal(1, n);
        sceNgs2FftProcess(h_work, s_in, s_out, s_scratch, 0, 1 | 2);
        snprintf(sz_case, sizeof(sz_case), "fft_%u_cos3_bin3", (unsigned)n);
        record_near(sz_case, s_out[6], n / 2.0, n * 1e-6);
        signal(2, n);
        sceNgs2FftProcess(h_work, s_in, s_out, s_scratch, 0, 1 | 2);
        snprintf(sz_case, sizeof(sz_case), "fft_%u_sin5_bin5_imag", (unsigned)n);
        record_near(sz_case, s_out[11], -(n / 2.0), n * 1e-6);
        signal(4, n);
        sceNgs2FftProcess(h_work, s_in, s_out, s_scratch, 0, 1 | 2 | 8);
        snprintf(sz_case, sizeof(sz_case), "fft_%u_nyquist_scaled", (unsigned)n);
        record_near(sz_case, s_out[1], 1.0, 1e-6);
        signal(5, n);
        sceNgs2FftProcess(h_work, s_in, s_out, s_scratch, 0, 1 | 2 | 4);
        sceNgs2FftProcess(h_work, s_out, s_scratch + n, s_scratch, 0, 2 | 4 | 8);
        snprintf(sz_case, sizeof(sz_case), "fft_%u_round_trip", (unsigned)n);
        record_near(sz_case, s_scratch[n + 17], s_in[17], 1e-5);

        static const uint32_t process_flags[] = {1 | 2, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 8};
        for (int kind = 0; kind < 6; ++kind) {
            for (int i_flags = 0; i_flags < 6; ++i_flags) {
                signal(kind, n);
                memset(s_out, 0xcd, n * sizeof(float));
                const int rc = sceNgs2FftProcess(h_work, s_in, s_out, s_scratch, 0, process_flags[i_flags]);
                snprintf(sz_case, sizeof(sz_case), "fft_%u_signal_%d_flags_%#x", (unsigned)n, kind,
                         (unsigned)process_flags[i_flags]);
                dump(p_dump, sz_case, rc, s_out, n);
            }
        }
    }

    /* Unconfirmed (spec 12 §3.4): 11.00's complex mode transforms nothing. Forward, unordered, 256 points
     * (3 factors, odd): the result is the work buffer as it was. */
    sceNgs2FftQuerySize(8, 0, &cb);
    sceNgs2FftInit(s_work, cb, 8, 0, &h_work);
    for (uint32_t i = 0; i < 512; ++i) {
        s_in[i] = (float)i;
        s_out[i] = -1.0f;
        s_scratch[i] = 1000.0f + (float)i;
    }
    record("fft_complex_forward", sceNgs2FftProcess(h_work, s_in, s_out, s_scratch, 0, 1 | 4), 0);
    record("fft_complex_forward_is_work", word_of(s_out[5]), word_of(1005.0f));
    for (uint32_t flags = 0; flags < 16; ++flags) {
        char sz_case[64];
        for (uint32_t i = 0; i < 512; ++i) {
            s_out[i] = -(float)i;
            s_scratch[i] = 1000.0f + (float)i;
        }
        const int rc = sceNgs2FftProcess(h_work, s_in, s_out, s_scratch, 0, flags);
        snprintf(sz_case, sizeof(sz_case), "fft_complex_flags_%#x", (unsigned)flags);
        dump(p_dump, sz_case, rc, s_out, 512);
    }
}

int main(void) {
    int rc = sceSysmoduleLoadModule(SYSMODULE_NGS2);
    if (rc != 0) {
        printf("ngs2_pan: loading libSceNgs2 failed: %#x\n", (unsigned)rc);
        return 1;
    }
    mkdir("/data/psport-hwtest", 0777);
    mkdir(SZ_OUT_DIR, 0777);
    s_p_results = fopen(SZ_OUT_DIR "/results.csv", "w");
    FILE *p_pan = fopen(SZ_OUT_DIR "/pan.csv", "w");
    FILE *p_geom = fopen(SZ_OUT_DIR "/geom.csv", "w");
    FILE *p_fft = fopen(SZ_OUT_DIR "/fft.csv", "w");
    if (!s_p_results || !p_pan || !p_geom || !p_fft) {
        printf("ngs2_pan: cannot write to %s (errno %d)\n", SZ_OUT_DIR, errno);
        return 1;
    }
    fprintf(s_p_results, "case,value,expected,match\n");
    test_pan(p_pan);
    test_geom(p_geom);
    test_fft(p_fft);
    fclose(p_fft);
    fclose(p_geom);
    fclose(p_pan);
    fclose(s_p_results);
    printf("ngs2_pan: done, %d mismatch(es) against the values the spec predicts\n", s_c_mismatches);
    return s_c_mismatches != 0;
}
