#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ngs2_compat.hpp"
#include "Ngs2Internal.hpp"

static void SetCoefficients(Ngs2Filter& filter, const Ngs2FilterParam& param, std::uint32_t sampleRate) {
    const auto type = param.type & 0xff;
    if (type == SCE_NGS2_FILTER_TYPE_COEFFICIENTS) {
        filter.b0 = param.values[0];
        filter.b1 = param.values[1];
        filter.b2 = param.values[2];
        filter.a1 = -param.values[3];
        filter.a2 = -param.values[4];
        return;
    }
    if (type < SCE_NGS2_FILTER_TYPE_LOW_PASS || type > SCE_NGS2_FILTER_TYPE_FIRST_ORDER_HIGH_PASS) {
        filter.b0 = 1.0;
        filter.b1 = filter.b2 = filter.a1 = filter.a2 = 0.0;
        return;
    }
    const float ratio = param.values[0] / static_cast<float>(sampleRate);
    const float omega = ratio < 0.001f ? 0.0062831859f : std::min(0.499f, ratio) * 6.2831855f;
    const float q = param.values[1] < 1e-4f ? 1e-4f : std::min(32.0f, param.values[1]);
    const float sine = std::sin(omega);
    const float cosine = std::cos(omega);
    const float alpha = sine / (q + q);
    const float amplitude = std::sqrt(std::max(param.values[2], 1e-4f));
    const float shelf = std::sqrt(amplitude) / q * sine;
    float b0, b1, b2, a0, a1, a2;
    switch (type) {
        case SCE_NGS2_FILTER_TYPE_LOW_PASS:
            b1 = 1.0f - cosine;
            b0 = b2 = b1 * 0.5f;
            a0 = 1.0f + alpha, a1 = -2.0f * cosine, a2 = 1.0f - alpha;
            break;
        case SCE_NGS2_FILTER_TYPE_HIGH_PASS:
            b1 = -(1.0f + cosine);
            b0 = b2 = (1.0f + cosine) * 0.5f;
            a0 = 1.0f + alpha, a1 = -2.0f * cosine, a2 = 1.0f - alpha;
            break;
        case SCE_NGS2_FILTER_TYPE_BAND_PASS:
            b0 = sine * 0.5f, b1 = 0.0f, b2 = -b0;
            a0 = 1.0f + alpha, a1 = -2.0f * cosine, a2 = 1.0f - alpha;
            break;
        case SCE_NGS2_FILTER_TYPE_BAND_PASS_PEAK:
            b0 = alpha, b1 = 0.0f, b2 = -alpha;
            a0 = 1.0f + alpha, a1 = -2.0f * cosine, a2 = 1.0f - alpha;
            break;
        case SCE_NGS2_FILTER_TYPE_NOTCH:
            b0 = 1.0f, b1 = -2.0f * cosine, b2 = 1.0f;
            a0 = 1.0f + alpha, a1 = b1, a2 = 1.0f - alpha;
            break;
        case SCE_NGS2_FILTER_TYPE_HIGH_SHELF:
            b0 = amplitude * ((amplitude + 1.0f) + (amplitude - 1.0f) * cosine + shelf);
            b1 = -2.0f * amplitude * ((amplitude - 1.0f) + (amplitude + 1.0f) * cosine);
            b2 = amplitude * ((amplitude + 1.0f) + (amplitude - 1.0f) * cosine - shelf);
            a0 = (amplitude + 1.0f) - (amplitude - 1.0f) * cosine + shelf;
            a1 = 2.0f * ((amplitude - 1.0f) - (amplitude + 1.0f) * cosine);
            a2 = (amplitude + 1.0f) - (amplitude - 1.0f) * cosine - shelf;
            break;
        case SCE_NGS2_FILTER_TYPE_LOW_SHELF:
            b0 = amplitude * ((amplitude + 1.0f) - (amplitude - 1.0f) * cosine + shelf);
            b1 = 2.0f * amplitude * ((amplitude - 1.0f) - (amplitude + 1.0f) * cosine);
            b2 = amplitude * ((amplitude + 1.0f) - (amplitude - 1.0f) * cosine - shelf);
            a0 = (amplitude + 1.0f) + (amplitude - 1.0f) * cosine + shelf;
            a1 = -2.0f * ((amplitude - 1.0f) + (amplitude + 1.0f) * cosine);
            a2 = (amplitude + 1.0f) + (amplitude - 1.0f) * cosine - shelf;
            break;
        case SCE_NGS2_FILTER_TYPE_PEAKING:
            b0 = 1.0f + alpha * amplitude, b1 = -2.0f * cosine, b2 = 1.0f - alpha * amplitude;
            a0 = 1.0f + alpha / amplitude, a1 = b1, a2 = 1.0f - alpha / amplitude;
            break;
        case SCE_NGS2_FILTER_TYPE_FIRST_ORDER_LOW_PASS:
            b0 = b1 = sine, b2 = 0.0f;
            a0 = sine + cosine + 1.0f, a1 = sine - cosine - 1.0f, a2 = 0.0f;
            break;
        default:
            b0 = 1.0f + cosine, b1 = -b0, b2 = 0.0f;
            a0 = sine + b0, a1 = sine - b0, a2 = 0.0f;
            break;
    }
    const float inverse = 1.0f / a0;
    filter.b0 = b0 * inverse;
    filter.b1 = b1 * inverse;
    filter.b2 = b2 * inverse;
    filter.a1 = a1 * inverse;
    filter.a2 = a2 * inverse;
}

void Ngs2SetFilter(Ngs2Filter& filter, const Ngs2FilterParam& param) {
    filter.param = param;
    filter.enabled = true;
    filter.bypassMask = param.channel_mask & 0xff;
    filter.sampleRate = 0;
}

void Ngs2UpdateFilter(Ngs2Filter& filter, std::uint32_t sampleRate) {
    if (filter.sampleRate == sampleRate) return;
    SetCoefficients(filter, filter.param, sampleRate);
    filter.sampleRate = sampleRate;
}
