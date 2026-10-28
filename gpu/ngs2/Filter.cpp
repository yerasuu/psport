// SPDX-License-Identifier: GPL-2.0-or-later
// The biquad filter, module 0x13 (0x1b880 parameters, 0x1bac0 process), and the mastering's LFE
// low-pass, module 0x14 (0x1c690), spec 06 §4, §5: RBJ cookbook designs with A = sqrt(gain), one
// direct-form-I section per channel whose feedback coefficients are stored negated.
#include <algorithm>
#include <cmath>
#include <numbers>

#include "Dsp.h"

namespace Libraries::Ngs2 {
namespace {

enum FilterType : u32 {
    FILTER_BYPASS = 0,
    FILTER_LOW_PASS = 1,
    FILTER_HIGH_PASS = 2,
    FILTER_BAND_PASS_SKIRT = 3,
    FILTER_BAND_PASS_PEAK = 4,
    FILTER_NOTCH = 5,
    FILTER_LOW_SHELF = 6,
    FILTER_HIGH_SHELF = 7,
    FILTER_PEAKING = 8,
    FILTER_LOW_PASS_FIRST_ORDER = 9,
    FILTER_HIGH_PASS_FIRST_ORDER = 10,
    FILTER_DIRECT = 0x20,
};

struct LfeCoefficients {
    float stages[2][5];
};

// Firmware 11.00's table at 0x34f90 (spec 06-tablas §B): two cascaded sections per pair of system
// rate and cut-off, the gain in the first one.
constexpr LfeCoefficients kLfeCoefficients[] = {
    {{{2.12305224e-07f, 4.24610448e-07f, 2.12305224e-07f, 1.94224191f, -0.943500161f},
      {1.0f, 2.0f, 1.0f, 1.97348094f, -0.976211846f}}},
    {{{1.0531561e-06f, 2.10631219e-06f, 1.0531561e-06f, 1.91365385f, -0.916445732f},
      {1.0f, 2.0f, 1.0f, 1.95844567f, -0.96455127f}}},
    {{{1.58618132e-05f, 3.17236263e-05f, 1.58618132e-05f, 1.8290211f, -0.839743078f},
      {1.0f, 2.0f, 1.0f, 1.90662777f, -0.930571795f}}},
    {{{1.51771118e-07f, 3.03542237e-07f, 1.51771118e-07f, 1.94690561f, -0.947970152f},
      {1.0f, 2.0f, 1.0f, 1.97581422f, -0.978121758f}}},
    {{{7.54104576e-07f, 1.50820915e-06f, 7.54104576e-07f, 1.92060459f, -0.922969162f},
      {1.0f, 2.0f, 1.0f, 1.96221697f, -0.967378676f}}},
    {{{1.14126015e-05f, 2.28252029e-05f, 1.14126015e-05f, 1.84266067f, -0.851769984f},
      {1.0f, 2.0f, 1.0f, 1.91570973f, -0.935987473f}}},
    {{{1.35429241e-08f, 2.70858482e-08f, 1.35429241e-08f, 1.97102308f, -0.971342146f},
      {1.0f, 2.0f, 1.0f, 1.98734319f, -0.988030136f}}},
    {{{6.78635814e-08f, 1.35727163e-07f, 6.78635814e-08f, 1.95660818f, -0.957320988f},
      {1.0f, 2.0f, 1.0f, 1.98056078f, -0.982101679f}}},
    {{{1.0531561e-06f, 2.10631219e-06f, 1.0531561e-06f, 1.91365385f, -0.916445732f},
      {1.0f, 2.0f, 1.0f, 1.95844567f, -0.96455127f}}},
    {{{9.66550129e-09f, 1.93310026e-08f, 9.66550129e-09f, 1.97337008f, -0.973639727f},
      {1.0f, 2.0f, 1.0f, 1.98841691f, -0.988997102f}}},
    {{{4.84738756e-08f, 9.69477512e-08f, 4.84738756e-08f, 1.96011734f, -0.960720003f},
      {1.0f, 2.0f, 1.0f, 1.98224151f, -0.983543158f}}},
    {{{7.54104576e-07f, 1.50820915e-06f, 7.54104576e-07f, 1.92060459f, -0.922969162f},
      {1.0f, 2.0f, 1.0f, 1.96221697f, -0.967378676f}}},
    {{{8.55150895e-10f, 1.71030168e-09f, 8.55150895e-10f, 1.98548687f, -0.985567272f},
      {1.0f, 2.0f, 1.0f, 1.99382424f, -0.993996501f}}},
    {{{4.30706537e-09f, 8.61413074e-09f, 4.30706537e-09f, 1.97824883f, -0.97842896f},
      {1.0f, 2.0f, 1.0f, 1.99062169f, -0.991008639f}}},
    {{{6.78635814e-08f, 1.35727163e-07f, 6.78635814e-08f, 1.95660818f, -0.957320988f},
      {1.0f, 2.0f, 1.0f, 1.98056078f, -0.982101679f}}},
    {{{6.09809425e-10f, 1.21961885e-09f, 6.09809425e-10f, 1.9866643f, -0.986732125f},
      {1.0f, 2.0f, 1.0f, 1.99433744f, -0.994482934f}}},
    {{{3.07265147e-09f, 6.14530293e-09f, 3.07265147e-09f, 1.98001206f, -0.98016417f},
      {1.0f, 2.0f, 1.0f, 1.9914093f, -0.991736054f}}},
    {{{4.84738756e-08f, 9.69477512e-08f, 4.84738756e-08f, 1.96011734f, -0.960720003f},
      {1.0f, 2.0f, 1.0f, 1.98224151f, -0.983543158f}}},
    {{{5.37218985e-11f, 1.07443797e-10f, 5.37218985e-11f, 1.99273729f, -0.99275744f},
      {1.0f, 2.0f, 1.0f, 1.99695051f, -0.996993661f}}},
    {{{2.71270006e-10f, 5.42540013e-10f, 2.71270006e-10f, 1.98911059f, -0.989155829f},
      {1.0f, 2.0f, 1.0f, 1.99539697f, -0.995493948f}}},
    {{{4.30706537e-09f, 8.61413074e-09f, 4.30706537e-09f, 1.97824883f, -0.97842896f},
      {1.0f, 2.0f, 1.0f, 1.99062169f, -0.991008639f}}},
    {{{3.82932991e-11f, 7.6586501e-11f, 3.82932991e-11f, 1.9933269f, -0.993343949f},
      {1.0f, 2.0f, 1.0f, 1.9972012f, -0.997237623f}}},
    {{{1.93402794e-10f, 3.86805699e-10f, 1.93402794e-10f, 1.98999429f, -0.990032554f},
      {1.0f, 2.0f, 1.0f, 1.99577737f, -0.995859325f}}},
    {{{3.07265147e-09f, 6.14530293e-09f, 3.07265147e-09f, 1.98001206f, -0.98016417f},
      {1.0f, 2.0f, 1.0f, 1.9914093f, -0.991736054f}}},
    {{{3.36630008e-12f, 6.73249997e-12f, 3.36630008e-12f, 1.9963671f, -0.996372163f},
      {1.0f, 2.0f, 1.0f, 1.99848485f, -0.998495698f}}},
    {{{1.70198005e-11f, 3.40395004e-11f, 1.70198005e-11f, 1.99455178f, -0.994563162f},
      {1.0f, 2.0f, 1.0f, 1.99772012f, -0.997744381f}}},
    {{{2.71270006e-10f, 5.42540013e-10f, 2.71270006e-10f, 1.98911059f, -0.989155829f},
      {1.0f, 2.0f, 1.0f, 1.99539697f, -0.995493948f}}},
    {{{2.39900005e-12f, 4.7980001e-12f, 2.39900005e-12f, 1.99666214f, -0.996666431f},
      {1.0f, 2.0f, 1.0f, 1.99860871f, -0.998617828f}}},
    {{{1.21304997e-11f, 2.42609995e-11f, 1.21304997e-11f, 1.99499416f, -0.995003819f},
      {1.0f, 2.0f, 1.0f, 1.99790704f, -0.997927487f}}},
    {{{1.93402794e-10f, 3.86805699e-10f, 1.93402794e-10f, 1.98999429f, -0.990032554f},
      {1.0f, 2.0f, 1.0f, 1.99577737f, -0.995859325f}}},
};

// The firmware's order, 96000 before 88200, so 88200 Hz takes the 96000 Hz coefficients.
constexpr u32 kLfeRates[] = {11025, 12000, 22050, 24000, 44100, 48000, 96000, 88200, 176400, 192000};
constexpr u32 kLfeCutoffs[] = {80, 120, 240};
static_assert(std::size(kLfeCoefficients) == std::size(kLfeRates) * std::size(kLfeCutoffs));

template <std::size_t N>
u32 FirstAtLeast(const u32 (&values)[N], u32 value) {
    for (u32 i = 0; i < N; ++i) {
        if (values[i] >= value) {
            return i;
        }
    }
    return 0;
}

float RunBiquad(const float* p_coefficients, BiquadState* p_state, float x) {
    const float y = p_coefficients[0] * x + p_coefficients[1] * p_state->x1 + p_coefficients[2] * p_state->x2 +
                    p_coefficients[3] * p_state->y1 + p_coefficients[4] * p_state->y2;
    *p_state = {x, p_state->x1, y, p_state->y1};
    return y;
}

} // namespace

void DesignFilter(FilterParams* p_filter, u32 rate_hz) {
    float* p_out = p_filter->coefficients;
    if (p_filter->type == FILTER_DIRECT) {
        std::copy(std::begin(p_filter->f), std::end(p_filter->f), p_out);
        return;
    }
    const float ratio = p_filter->f[0] / float(rate_hz);
    const float w = ratio < 0.001f ? 0.0062831859f : 2.0f * std::numbers::pi_v<float> * std::min(ratio, 0.499f);
    const float q = std::clamp(p_filter->f[1], 1e-4f, 32.0f);
    const float s = std::sin(w);
    const float c = std::cos(w);
    const float alpha = s / (2.0f * q);
    const float a = std::sqrt(std::max(p_filter->f[2], 1e-4f));
    const float shelf = 2.0f * std::sqrt(a) * alpha;
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
    switch (p_filter->type) {
    case FILTER_LOW_PASS:
        b0 = (1.0f - c) / 2.0f, b1 = 1.0f - c, b2 = (1.0f - c) / 2.0f;
        a0 = 1.0f + alpha, a1 = -2.0f * c, a2 = 1.0f - alpha;
        break;
    case FILTER_HIGH_PASS:
        b0 = (1.0f + c) / 2.0f, b1 = -(1.0f + c), b2 = (1.0f + c) / 2.0f;
        a0 = 1.0f + alpha, a1 = -2.0f * c, a2 = 1.0f - alpha;
        break;
    case FILTER_BAND_PASS_SKIRT:
        b0 = s / 2.0f, b1 = 0.0f, b2 = -s / 2.0f;
        a0 = 1.0f + alpha, a1 = -2.0f * c, a2 = 1.0f - alpha;
        break;
    case FILTER_BAND_PASS_PEAK:
        b0 = alpha, b1 = 0.0f, b2 = -alpha;
        a0 = 1.0f + alpha, a1 = -2.0f * c, a2 = 1.0f - alpha;
        break;
    case FILTER_NOTCH:
        b0 = 1.0f, b1 = -2.0f * c, b2 = 1.0f;
        a0 = 1.0f + alpha, a1 = -2.0f * c, a2 = 1.0f - alpha;
        break;
    case FILTER_LOW_SHELF:
        b0 = a * ((a + 1.0f) - (a - 1.0f) * c + shelf);
        b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * c);
        b2 = a * ((a + 1.0f) - (a - 1.0f) * c - shelf);
        a0 = (a + 1.0f) + (a - 1.0f) * c + shelf;
        a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * c);
        a2 = (a + 1.0f) + (a - 1.0f) * c - shelf;
        break;
    case FILTER_HIGH_SHELF:
        b0 = a * ((a + 1.0f) + (a - 1.0f) * c + shelf);
        b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * c);
        b2 = a * ((a + 1.0f) + (a - 1.0f) * c - shelf);
        a0 = (a + 1.0f) - (a - 1.0f) * c + shelf;
        a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * c);
        a2 = (a + 1.0f) - (a - 1.0f) * c - shelf;
        break;
    case FILTER_PEAKING:
        b0 = 1.0f + alpha * a, b1 = -2.0f * c, b2 = 1.0f - alpha * a;
        a0 = 1.0f + alpha / a, a1 = -2.0f * c, a2 = 1.0f - alpha / a;
        break;
    case FILTER_LOW_PASS_FIRST_ORDER:
        b0 = s, b1 = s, b2 = 0.0f;
        a0 = 1.0f + s + c, a1 = s - c - 1.0f, a2 = 0.0f;
        break;
    case FILTER_HIGH_PASS_FIRST_ORDER:
        b0 = 1.0f + c, b1 = -(1.0f + c), b2 = 0.0f;
        a0 = 1.0f + s + c, a1 = s - c - 1.0f, a2 = 0.0f;
        break;
    default:
        break;
    }
    p_out[0] = b0 / a0;
    p_out[1] = b1 / a0;
    p_out[2] = b2 / a0;
    p_out[3] = -a1 / a0;
    p_out[4] = -a2 / a0;
}

bool ProcessFilter(const FilterParams& filter, u32 location, BiquadState* p_states, const Planes& planes,
                   u32 c_channels, u32 c_samples, bool is_first_grain) {
    if (filter.location != location) {
        return false;
    }
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        BiquadState& state = p_states[i_channel];
        if (is_first_grain) {
            state = {};
        }
        float* p_samples = planes.p_channel[i_channel];
        if (filter.bypass_mask & (1u << i_channel)) {
            state = {};
            continue;
        }
        if (filter.type == FILTER_BYPASS) {
            // Keeps the history of the unfiltered signal, so a filter switched on later starts
            // without a click.
            const float x1 = p_samples[c_samples - 1];
            const float x2 = p_samples[c_samples - 2];
            state = {x1, x2, x1, x2};
            continue;
        }
        for (u32 i = 0; i < c_samples; ++i) {
            p_samples[i] = RunBiquad(filter.coefficients, &state, p_samples[i]);
        }
    }
    return true;
}

void ProcessLfe(const LfeParams& lfe, LfeState* p_state, float* p_lfe, u32 c_samples, u32 rate_hz) {
    ProcessLfe(lfe, p_state, p_lfe, p_lfe, c_samples, rate_hz);
}

// Both stages read p_in (0x1c690, spec 06 §5): the cascade only happens in place.
void ProcessLfe(const LfeParams& lfe, LfeState* p_state, const float* p_in, float* p_out, u32 c_samples, u32 rate_hz) {
    if (!lfe.is_enabled) {
        *p_state = {};
        return;
    }
    const u32 i_rate = FirstAtLeast(kLfeRates, rate_hz);
    const u32 i_cutoff = FirstAtLeast(kLfeCutoffs, lfe.cutoff_hz);
    const LfeCoefficients& coefficients = kLfeCoefficients[i_rate * std::size(kLfeCutoffs) + i_cutoff];
    for (u32 i_stage = 0; i_stage < 2; ++i_stage) {
        for (u32 i = 0; i < c_samples; ++i) {
            p_out[i] = RunBiquad(coefficients.stages[i_stage], &p_state->stages[i_stage], p_in[i]);
        }
    }
}

} // namespace Libraries::Ngs2
