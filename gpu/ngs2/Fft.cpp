// SPDX-License-Identifier: GPL-2.0-or-later
// sceNgs2Fft* (0x430, 0x480, 0x860; rea spec 12 §3). Firmware 11.00 runs PFFFT's real transform on
// 256 to 4096 points; psport computes the same DFT with its own radix-2 transform and keeps what the
// game can see of the firmware's: the sizes, the packed spectrum, the internal order an unordered
// transform leaves (zreorder 0x24f0, ported exactly), the 1/N scaling, and the complex mode, which
// 11.00 never transforms and only moves between the game's buffers.
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>

#include "Internal.h"
#include "Spatial.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kMinLog2Points = 8;
constexpr u32 kMaxLog2Points = 12;
constexpr u32 kMaxPoints = 1u << kMaxLog2Points;
constexpr u32 kFlagForward = 0x1;
constexpr u32 kFlagReal = 0x2;
constexpr u32 kFlagUnordered = 0x4;
constexpr u32 kFlagScale = 0x8;
constexpr std::size_t kCbHeader = 0x40;
constexpr std::size_t kTableAlignment = 16;
constexpr s32 kFftFailed = -1;

// The firmware's header at the start of the game's buffer (+0x00 and +0x08 its tables' addresses,
// +0x10 log2 of the points, +0x14 Init's flags); here +0x00 holds the twiddle table's address, which
// Process checks to refuse a buffer Init did not set up.
struct FftHeader {
    u64 va_table;
    u64 va_twiddles;
    u32 log2_points;
    u32 flags;
};
static_assert(sizeof(FftHeader) == 0x18);

struct Vec4 {
    float lanes[4];
};

// zreorder works on 4-float vectors exactly as the firmware's SSE does: interleave (unpcklps,
// unpckhps), uninterleave (shufps 0x88, 0xdd) and the half swap (blendps 3).
struct Interleaved {
    Vec4 low;
    Vec4 high;
};

Vec4 LoadVec(const float* p_floats, std::ptrdiff_t i_vec) {
    Vec4 v;
    std::memcpy(v.lanes, p_floats + i_vec * 4, sizeof(v.lanes));
    return v;
}

void StoreVec(float* p_floats, std::ptrdiff_t i_vec, const Vec4& v) {
    std::memcpy(p_floats + i_vec * 4, v.lanes, sizeof(v.lanes));
}

Interleaved Interleave(const Vec4& a, const Vec4& b) {
    return {{a.lanes[0], b.lanes[0], a.lanes[1], b.lanes[1]}, {a.lanes[2], b.lanes[2], a.lanes[3], b.lanes[3]}};
}

Interleaved Uninterleave(const Vec4& a, const Vec4& b) {
    return {{a.lanes[0], a.lanes[2], b.lanes[0], b.lanes[2]}, {a.lanes[1], a.lanes[3], b.lanes[1], b.lanes[3]}};
}

Vec4 SwapHalves(const Vec4& a, const Vec4& b) {
    return {b.lanes[0], b.lanes[1], a.lanes[2], a.lanes[3]};
}

// The two halves of the spectrum that the real forward reorder writes backwards.
void ReversedCopy(u32 c_blocks, const float* p_in, std::ptrdiff_t i_in, float* p_out, std::ptrdiff_t i_out_end) {
    const Interleaved first = Interleave(LoadVec(p_in, i_in), LoadVec(p_in, i_in + 1));
    std::ptrdiff_t i_out = i_out_end;
    StoreVec(p_out, --i_out, SwapHalves(first.low, first.high));
    Vec4 carry = first.high;
    for (u32 i_block = 1; i_block < c_blocks; ++i_block) {
        i_in += 8;
        const Interleaved next = Interleave(LoadVec(p_in, i_in), LoadVec(p_in, i_in + 1));
        StoreVec(p_out, --i_out, SwapHalves(carry, next.low));
        StoreVec(p_out, --i_out, SwapHalves(next.low, next.high));
        carry = next.high;
    }
    StoreVec(p_out, --i_out, SwapHalves(carry, first.low));
}

void UnreversedCopy(u32 c_blocks, const float* p_in, std::ptrdiff_t i_in, float* p_out, std::ptrdiff_t i_out) {
    const Vec4 first = LoadVec(p_in, i_in++);
    Vec4 carry = first;
    for (u32 i_block = 1; i_block < c_blocks; ++i_block) {
        const Vec4 head = LoadVec(p_in, i_in++);
        const Vec4 tail = LoadVec(p_in, i_in++);
        const Vec4 joined = SwapHalves(carry, head);
        const Interleaved out = Uninterleave(SwapHalves(head, tail), joined);
        StoreVec(p_out, i_out, out.low);
        StoreVec(p_out, i_out + 1, out.high);
        i_out -= 8;
        carry = tail;
    }
    const Vec4 head = LoadVec(p_in, i_in);
    const Interleaved out = Uninterleave(SwapHalves(head, first), SwapHalves(carry, head));
    StoreVec(p_out, i_out, out.low);
    StoreVec(p_out, i_out + 1, out.high);
}

// 0x24f0 over c_points (the table's N): the real kind touches N floats, the complex one 2N.
// p_in and p_out must not overlap.
void Reorder(const float* p_in, float* p_out, u32 c_points, bool is_real_kind, bool is_forward) {
    if (!is_real_kind) {
        const u32 c_vectors = c_points / 4;
        for (u32 k = 0; k < c_vectors; ++k) {
            const u32 kk = k / 4 + (k % 4) * (c_vectors / 4);
            if (is_forward) {
                const Interleaved v = Interleave(LoadVec(p_in, 2 * k), LoadVec(p_in, 2 * k + 1));
                StoreVec(p_out, 2 * kk, v.low);
                StoreVec(p_out, 2 * kk + 1, v.high);
            } else {
                const Interleaved v = Uninterleave(LoadVec(p_in, 2 * kk), LoadVec(p_in, 2 * kk + 1));
                StoreVec(p_out, 2 * k, v.low);
                StoreVec(p_out, 2 * k + 1, v.high);
            }
        }
        return;
    }
    const u32 c_blocks = c_points / 32;
    const std::ptrdiff_t c_vectors = c_points / 4;
    if (is_forward) {
        for (u32 k = 0; k < c_blocks; ++k) {
            const Interleaved a = Interleave(LoadVec(p_in, 8 * k), LoadVec(p_in, 8 * k + 1));
            StoreVec(p_out, 2 * k, a.low);
            StoreVec(p_out, 2 * k + 1, a.high);
            const Interleaved b = Interleave(LoadVec(p_in, 8 * k + 4), LoadVec(p_in, 8 * k + 5));
            StoreVec(p_out, 2 * (2 * c_blocks + k), b.low);
            StoreVec(p_out, 2 * (2 * c_blocks + k) + 1, b.high);
        }
        ReversedCopy(c_blocks, p_in, 2, p_out, c_vectors / 2);
        ReversedCopy(c_blocks, p_in, 6, p_out, c_vectors);
        return;
    }
    for (u32 k = 0; k < c_blocks; ++k) {
        const Interleaved a = Uninterleave(LoadVec(p_in, 2 * k), LoadVec(p_in, 2 * k + 1));
        StoreVec(p_out, 8 * k, a.low);
        StoreVec(p_out, 8 * k + 1, a.high);
        const u32 i_high = 2 * (2 * c_blocks + k);
        const Interleaved b = Uninterleave(LoadVec(p_in, i_high), LoadVec(p_in, i_high + 1));
        StoreVec(p_out, 8 * k + 4, b.low);
        StoreVec(p_out, 8 * k + 5, b.high);
    }
    UnreversedCopy(c_blocks, p_in, c_vectors / 4, p_out, c_vectors - 6);
    UnreversedCopy(c_blocks, p_in, 3 * c_vectors / 4, p_out, c_vectors - 2);
}

bool IsLog2Points(u32 log2_points) {
    return log2_points - kMinLog2Points <= kMaxLog2Points - kMinLog2Points;
}

// 0x30b30's rows: the factor count of N/4 is odd for 256, 2048 and 4096 points.
bool HasOddFactorCount(u32 log2_points) {
    return log2_points != 9 && log2_points != 10;
}

// The floats a transform reads and writes: 2 * Ncvec vectors.
u32 TransformFloats(u32 log2_points, u32 setup_flags) {
    return (setup_flags & kFlagReal) != 0 ? 1u << log2_points : 2u << log2_points;
}

std::size_t WorkBytes(u32 log2_points, u32 flags) {
    return std::size_t(TransformFloats(log2_points, flags)) * sizeof(float) + kCbHeader;
}

std::size_t TableOffset(const void* p_buffer) {
    const auto va_buffer = reinterpret_cast<std::uintptr_t>(p_buffer);
    const std::uintptr_t va_table = (va_buffer + sizeof(FftHeader) + kTableAlignment - 1) & ~(kTableAlignment - 1);
    return va_table - va_buffer;
}

bool Overlaps(const float* p_a, const float* p_b, u32 c_floats) {
    return p_a < p_b + c_floats && p_b < p_a + c_floats;
}

// p_twiddles holds e^(2πij/N) for j < N/2 as (cos, sin) pairs; the forward transform uses their
// conjugates.
void ComplexTransform(float* p_data, u32 c_complex, const float* p_twiddles, u32 c_points, bool is_inverse) {
    for (u32 i = 1, j = 0; i < c_complex; ++i) {
        u32 bit = c_complex >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j |= bit;
        if (i < j) {
            std::swap(p_data[2 * i], p_data[2 * j]);
            std::swap(p_data[2 * i + 1], p_data[2 * j + 1]);
        }
    }
    const float sign = is_inverse ? 1.0f : -1.0f;
    for (u32 c_span = 2; c_span <= c_complex; c_span <<= 1) {
        const u32 c_half = c_span / 2;
        const u32 i_stride = c_points / c_span;
        for (u32 i_start = 0; i_start < c_complex; i_start += c_span) {
            for (u32 k = 0; k < c_half; ++k) {
                const float w_re = p_twiddles[2 * (k * i_stride)];
                const float w_im = sign * p_twiddles[2 * (k * i_stride) + 1];
                float* p_a = p_data + 2 * (i_start + k);
                float* p_b = p_data + 2 * (i_start + k + c_half);
                const float b_re = p_b[0] * w_re - p_b[1] * w_im;
                const float b_im = p_b[0] * w_im + p_b[1] * w_re;
                p_b[0] = p_a[0] - b_re;
                p_b[1] = p_a[1] - b_im;
                p_a[0] = p_a[0] + b_re;
                p_a[1] = p_a[1] + b_im;
            }
        }
    }
}

// The N real samples in p_data become the packed spectrum [Re X0, Re X(N/2), Re X1, Im X1, ...] of
// X[k] = sum x[n] e^(-2πikn/N), from the N/2-point transform of the even and odd samples as one
// complex sequence.
void ForwardReal(float* p_data, const float* p_twiddles, u32 c_points) {
    const u32 c_complex = c_points / 2;
    ComplexTransform(p_data, c_complex, p_twiddles, c_points, false);
    const float z0_re = p_data[0];
    const float z0_im = p_data[1];
    p_data[0] = z0_re + z0_im;
    p_data[1] = z0_re - z0_im;
    for (u32 k = 1; k <= c_complex / 2; ++k) {
        const u32 k_mirror = c_complex - k;
        const float a_re = p_data[2 * k];
        const float a_im = p_data[2 * k + 1];
        const float b_re = p_data[2 * k_mirror];
        const float b_im = -p_data[2 * k_mirror + 1];
        const float even_re = 0.5f * (a_re + b_re);
        const float even_im = 0.5f * (a_im + b_im);
        // (a - b) / 2i
        const float odd_re = 0.5f * (a_im - b_im);
        const float odd_im = -0.5f * (a_re - b_re);
        const float w_re = p_twiddles[2 * k];
        const float w_im = -p_twiddles[2 * k + 1];
        const float t_re = w_re * odd_re - w_im * odd_im;
        const float t_im = w_re * odd_im + w_im * odd_re;
        p_data[2 * k] = even_re + t_re;
        p_data[2 * k + 1] = even_im + t_im;
        p_data[2 * k_mirror] = even_re - t_re;
        p_data[2 * k_mirror + 1] = -(even_im - t_im);
    }
}

// The packed spectrum in p_data becomes x[n] = sum X[k] e^(2πikn/N) over the whole Hermitian
// spectrum: N times the inverse, as the firmware leaves it unscaled.
void BackwardReal(float* p_data, const float* p_twiddles, u32 c_points) {
    const u32 c_complex = c_points / 2;
    const float x0 = p_data[0];
    const float x_half = p_data[1];
    p_data[0] = x0 + x_half;
    p_data[1] = x0 - x_half;
    for (u32 k = 1; k <= c_complex / 2; ++k) {
        const u32 k_mirror = c_complex - k;
        const float a_re = p_data[2 * k];
        const float a_im = p_data[2 * k + 1];
        const float b_re = p_data[2 * k_mirror];
        const float b_im = -p_data[2 * k_mirror + 1];
        const float even_re = a_re + b_re;
        const float even_im = a_im + b_im;
        const float diff_re = a_re - b_re;
        const float diff_im = a_im - b_im;
        const float w_re = p_twiddles[2 * k];
        const float w_im = p_twiddles[2 * k + 1];
        // odd = diff * e^(+2πik/N); z = even + i * odd
        const float odd_re = diff_re * w_re - diff_im * w_im;
        const float odd_im = diff_re * w_im + diff_im * w_re;
        p_data[2 * k] = even_re - odd_im;
        p_data[2 * k + 1] = even_im + odd_re;
        // z[M - k] = conj(even) + i * conj(odd)
        p_data[2 * k_mirror] = even_re + odd_im;
        p_data[2 * k_mirror + 1] = -even_im + odd_re;
    }
    ComplexTransform(p_data, c_complex, p_twiddles, c_points, true);
}

void Scale(float* p_data, u32 c_floats, u32 c_points) {
    const float scale = 1.0f / float(c_points);
    for (u32 i = 0; i < c_floats; ++i) {
        p_data[i] = scale * p_data[i];
    }
}

struct Transform {
    FftHeader header;
    const float* p_twiddles;
    const float* p_in;
    float* p_out;
    u32 c_points;
    u32 flags;
};

// The real transform; p_scratch holds N floats that overlap neither p_in nor p_out.
void ProcessReal(const Transform& t, float* p_scratch) {
    const u32 c_points = t.c_points;
    const u32 setup_flags = t.header.flags;
    const bool is_unordered = (t.flags & kFlagUnordered) != 0;
    if ((t.flags & kFlagForward) != 0) {
        std::memmove(t.p_out, t.p_in, c_points * sizeof(float));
        ForwardReal(t.p_out, t.p_twiddles, c_points);
        if (is_unordered) {
            Reorder(t.p_out, p_scratch, c_points, true, false);
            std::memcpy(t.p_out, p_scratch, c_points * sizeof(float));
        } else if ((t.flags & kFlagReal) == 0) {
            static std::uint64_t s_c_overruns;
            NoteDeparture(&s_c_overruns, "sceNgs2FftProcess real forward without 0x2 (2N-float complex reorder)");
        }
        return;
    }
    // The firmware puts an ordered input into its internal order with the direction bit of Init's
    // and Process's flags together; the spectrum is that internal order put back forward.
    if (is_unordered) {
        Reorder(t.p_in, p_scratch, c_points, true, true);
        std::memcpy(t.p_out, p_scratch, c_points * sizeof(float));
    } else if (((setup_flags | t.flags) & kFlagForward) != 0) {
        Reorder(t.p_in, p_scratch, c_points, true, true);
        Reorder(p_scratch, t.p_out, c_points, true, true);
    } else {
        std::memmove(t.p_out, t.p_in, c_points * sizeof(float));
    }
    BackwardReal(t.p_out, t.p_twiddles, c_points);
}

// 11.00's complex mode: no transform, only the buffer moves of 0x860 (spec 12 §3.4). p_scratch is the
// game's work buffer, or zeros standing in for the firmware's uninitialised stack.
void ProcessComplex(const Transform& t, float* p_scratch) {
    const u32 c_points = t.c_points;
    const u32 c_floats = 2 * c_points;
    float* buffers[2] = {t.p_out, p_scratch};
    u32 i_buffer =
        ((t.flags & kFlagUnordered) != 0 ? 1u : 0u) ^ (HasOddFactorCount(t.header.log2_points) ? 1u : 0u);
    const bool is_ordered = (t.flags & kFlagUnordered) == 0;
    const auto reorder = [&](const float* p_from, float* p_to, u32 kind_flags) {
        if (Overlaps(p_from, p_to, c_floats)) {
            static std::uint64_t s_c_in_place;
            NoteDeparture(&s_c_in_place, "sceNgs2FftProcess complex reorder onto its own source");
            return;
        }
        Reorder(p_from, p_to, c_points, (kind_flags & kFlagReal) != 0, (kind_flags & kFlagForward) != 0);
    };
    if ((t.flags & kFlagForward) == 0) {
        if (buffers[i_buffer ^ 1] != t.p_in) {
            i_buffer ^= 1;
        }
        if (is_ordered) {
            reorder(t.p_in, buffers[i_buffer], t.header.flags | t.flags);
            i_buffer ^= 1;
        }
    } else if (is_ordered) {
        reorder(buffers[i_buffer ^ 1], buffers[i_buffer], t.flags);
    } else {
        i_buffer ^= 1;
    }
    const float* p_final = buffers[i_buffer];
    if (p_final != t.p_out) {
        std::memmove(t.p_out, p_final, c_floats * sizeof(float));
    }
}

bool IsReal(const Transform& t) {
    return (t.header.flags & kFlagReal) != 0;
}

// Whether the transform needs a work buffer: the real one only to reorder.
bool NeedsScratch(const Transform& t) {
    return !IsReal(t) || (t.flags & kFlagUnordered) != 0 ||
           ((t.flags & kFlagForward) == 0 && ((t.header.flags | t.flags) & kFlagForward) != 0);
}

void Run(const Transform& t, float* p_scratch) {
    if (IsReal(t)) {
        ProcessReal(t, p_scratch);
    } else {
        ProcessComplex(t, p_scratch);
    }
    if ((t.flags & kFlagScale) != 0) {
        Scale(t.p_out, TransformFloats(t.header.log2_points, t.header.flags), t.c_points);
    }
}

// The firmware's stack array when the game gives no work buffer. The complex mode can hand it back to
// the game as it is, so it starts as zeros where the firmware's holds whatever the stack held.
void RunOnStack(const Transform& t) {
    std::array<float, 2 * kMaxPoints> stack_scratch;
    if (!IsReal(t)) {
        std::memset(stack_scratch.data(), 0, sizeof(stack_scratch));
    }
    Run(t, stack_scratch.data());
}

} // namespace

s32 PS4_SYSV_ABI sceNgs2FftQuerySize(u32 log2_points, u32 flags, std::size_t* p_out_cb_work) {
    if (!IsLog2Points(log2_points)) {
        return kFftFailed;
    }
    if (p_out_cb_work == nullptr) {
        static std::uint64_t s_c_null;
        NoteDeparture(&s_c_null, "sceNgs2FftQuerySize with a null size");
        return kFftFailed;
    }
    *p_out_cb_work = WorkBytes(log2_points, flags);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2FftInit(void* p_buffer, std::size_t cb_buffer, u32 log2_points, u32 flags,
                                void** pp_out_work) {
    if (!IsLog2Points(log2_points) || cb_buffer < WorkBytes(log2_points, flags)) {
        return kFftFailed;
    }
    if (p_buffer == nullptr || pp_out_work == nullptr) {
        static std::uint64_t s_c_null;
        NoteDeparture(&s_c_null, "sceNgs2FftInit with a null buffer or handle");
        return kFftFailed;
    }
    auto* p_bytes = static_cast<u8*>(p_buffer);
    float* p_twiddles = reinterpret_cast<float*>(p_bytes + TableOffset(p_buffer));
    const u32 c_points = 1u << log2_points;
    for (u32 j = 0; j < c_points / 2; ++j) {
        const double angle = 2.0 * std::numbers::pi * double(j) / double(c_points);
        p_twiddles[2 * j] = float(std::cos(angle));
        p_twiddles[2 * j + 1] = float(std::sin(angle));
    }
    FftHeader header{};
    header.va_table = reinterpret_cast<std::uintptr_t>(p_twiddles);
    header.va_twiddles = header.va_table;
    header.log2_points = log2_points;
    header.flags = flags;
    std::memcpy(p_buffer, &header, sizeof(header));
    *pp_out_work = p_buffer;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2FftProcess(void* p_work, const float* p_in, float* p_out, float* p_scratch, u64,
                                   u32 flags) {
    FftHeader header{};
    if (p_work != nullptr) {
        std::memcpy(&header, p_work, sizeof(header));
    }
    const auto* p_bytes = static_cast<const u8*>(p_work);
    if (p_work == nullptr || p_in == nullptr || p_out == nullptr || !IsLog2Points(header.log2_points) ||
        header.va_table != reinterpret_cast<std::uintptr_t>(p_bytes + TableOffset(p_work))) {
        static std::uint64_t s_c_invalid;
        NoteDeparture(&s_c_invalid, "sceNgs2FftProcess without a work FftInit set up or a buffer");
        return kFftFailed;
    }
    const Transform transform{header,
                              reinterpret_cast<const float*>(p_bytes + TableOffset(p_work)),
                              p_in,
                              p_out,
                              1u << header.log2_points,
                              flags};
    const u32 c_floats = TransformFloats(header.log2_points, header.flags);
    if (IsReal(transform) && p_scratch != nullptr &&
        (Overlaps(p_scratch, p_in, c_floats) || Overlaps(p_scratch, p_out, c_floats))) {
        static std::uint64_t s_c_overlaps;
        NoteDeparture(&s_c_overlaps, "sceNgs2FftProcess with a work buffer over its input or output");
        p_scratch = nullptr;
    }
    if (p_scratch == nullptr && NeedsScratch(transform)) {
        RunOnStack(transform);
    } else {
        Run(transform, p_scratch);
    }
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2
