// SPDX-License-Identifier: GPL-2.0-or-later
// sceNgs2Geom* as firmware 11.00 computes them (0x2920 to 0x3ae0; rea spec 12 §2): the listener's frame
// as a matrix, and in that frame a source's distance gain, cone, Doppler pitch, A3D position,
// spherical harmonics and speaker matrix (through the pan). Every float operation keeps the
// firmware's order, its fast inverse square roots and its table arctangent, so that the results
// match it bit for bit.
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

#include "Internal.h"
#include "Spatial.h"
#include "core/libraries/ngs2/ngs2_error.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kFlagCone = 0x1;
constexpr u32 kFlagDoppler = 0x2;
constexpr u32 kFlagMatrix = 0x4;
constexpr u32 kFlagA3d = 0x8;
constexpr u32 kFlagHarmonics = 0x10;
constexpr u32 kAllFlags = 0x1f;
constexpr u32 kMaxRolloffModel = 5;
constexpr u32 kLeftHanded = 1;

constexpr float kPi = std::bit_cast<float>(0x40490fdbu);
constexpr float kHalfPi = std::bit_cast<float>(0x3fc90fdbu);
constexpr float kRadians = std::bit_cast<float>(0x40c90fdbu);
constexpr float kHalfPower = std::bit_cast<float>(0x3f3504f3u);
constexpr float kSpeedOfSound = std::bit_cast<float>(0x43aa251fu);
constexpr float kMinSoundSpeed = std::bit_cast<float>(0x3dcccccdu);
constexpr float kMinExponentialDistance = std::bit_cast<float>(0x3089705fu);
constexpr float kConeDegreesPerRadian = std::bit_cast<float>(0x42e52ee0u);
constexpr float kMaxConeAngle = 360.0f;
constexpr float kMaxConeLevel = 2.0f;

constexpr u32 kAtanSegments = 26;
constexpr u32 kAtanPointsPerSegment = 24;
constexpr u32 kAtanPoints = kAtanSegments * kAtanPointsPerSegment + 1;
constexpr float kAtanBeyond = std::bit_cast<float>(0x3fc90fccu);

// 0x30cc0, 0x30d30 and 0x30da0: the arctangent's breakpoints, the step of each segment's 24 points,
// and its values, as IEEE-754 words (43 of them are not the float nearest atan, so they are not
// recomputed). The last value is the word after the table, a NaN the firmware reads for an
// argument exactly on the last breakpoint.
constexpr std::array<u32, kAtanSegments + 1> kAtanBreaks{
    0x00000000, 0x3e8432a5, 0x3ec1848f, 0x3ef5f4f4, 0x3f13cd3a, 0x3f2c985d, 0x3f464bf8, 0x3f61c552,
    0x3f800000, 0x3f91236b, 0x3fa53f4e, 0x3fbddacc, 0x3fddb3d7, 0x40053a04, 0x402953fd, 0x4077def6,
    0x40968c54, 0x40b57b24, 0x41183ad6, 0x4198a62b, 0x41e5170c, 0x426528ec, 0x42e52d64, 0x440f3d43,
    0x45b30ca0, 0x475fcfc8, 0x490be1dd
};
constexpr std::array<u32, kAtanSegments> kAtanSteps{
    0x3c304387, 0x3ba3851c, 0x3b8bd662, 0x3b846402, 0x3b843b64, 0x3b89133a, 0x3b928736, 0x3ba138f7,
    0x3bb6cf23, 0x3bd67eca, 0x3c033d4b, 0x3c29dae5, 0x3c6eabaf, 0x3cc08a87, 0x3d517296, 0x3d0def33,
    0x3d24f9a8, 0x3e23f8b6, 0x3ecc1755, 0x3ecbd7ad, 0x3f98d1dd, 0x4018cbe8, 0x4198ca1e, 0x4356dbf4,
    0x45064978, 0x46a7dbd6
};
constexpr std::array<u32, kAtanPoints + 1> kAtanValues{
    0x00000000, 0x3c3041c9, 0x3cb03c91, 0x3d0426e7, 0x3d3027b4, 0x3d5c1e1a, 0x3d8403c2, 0x3d99f0b3,
    0x3dafd49a, 0x3dc5ae37, 0x3ddb7c4c, 0x3df13d9f, 0x3e03787d, 0x3e0e4a97, 0x3e191486, 0x3e23d5b9,
    0x3e2e8d9f, 0x3e393bab, 0x3e43df53, 0x3e4e7810, 0x3e590560, 0x3e6386c4, 0x3e6dfbbe, 0x3e7863d8,
    0x3e815f4e, 0x3e83c3c3, 0x3e8626b7, 0x3e888825, 0x3e8ae807, 0x3e8d465a, 0x3e8fa316, 0x3e91fe38,
    0x3e9457bb, 0x3e96af99, 0x3e9905cf, 0x3e9b5a57, 0x3e9dad2d, 0x3e9ffe4d, 0x3ea24db2, 0x3ea49b58,
    0x3ea6e73b, 0x3ea93157, 0x3eab79a9, 0x3eadc02b, 0x3eb004db, 0x3eb247b5, 0x3eb488b6, 0x3eb6c7d9,
    0x3eb9051d, 0x3ebaedda, 0x3ebcd533, 0x3ebebb27, 0x3ec09fb4, 0x3ec282d7, 0x3ec4648f, 0x3ec644db,
    0x3ec823b8, 0x3eca0125, 0x3ecbdd21, 0x3ecdb7a9, 0x3ecf90bd, 0x3ed1685b, 0x3ed33e81, 0x3ed5132f,
    0x3ed6e662, 0x3ed8b81a, 0x3eda8856, 0x3edc5714, 0x3ede2454, 0x3edff013, 0x3ee1ba52, 0x3ee3830f,
    0x3ee54a4a, 0x3ee6f7e1, 0x3ee8a41c, 0x3eea4efa, 0x3eebf87b, 0x3eeda09e, 0x3eef4762, 0x3ef0ecc7,
    0x3ef290cd, 0x3ef43372, 0x3ef5d4b6, 0x3ef7749a, 0x3ef9131c, 0x3efab03c, 0x3efc4bfb, 0x3efde656,
    0x3eff7f50, 0x3f008b73, 0x3f01568d, 0x3f0220f5, 0x3f02eaab, 0x3f03b3af, 0x3f047c02, 0x3f0543a3,
    0x3f060a92, 0x3f06d092, 0x3f0795e1, 0x3f085a7e, 0x3f091e6b, 0x3f09e1a5, 0x3f0aa42f, 0x3f0b6607,
    0x3f0c272e, 0x3f0ce7a4, 0x3f0da769, 0x3f0e667d, 0x3f0f24e1, 0x3f0fe294, 0x3f109f96, 0x3f115be9,
    0x3f12178b, 0x3f12d27d, 0x3f138cc0, 0x3f144653, 0x3f14ff36, 0x3f15b76b, 0x3f166ef1, 0x3f1725c8,
    0x3f17dbf1, 0x3f18980e, 0x3f195370, 0x3f1a0e17, 0x3f1ac805, 0x3f1b8138, 0x3f1c39b2, 0x3f1cf173,
    0x3f1da87b, 0x3f1e5ecb, 0x3f1f1462, 0x3f1fc942, 0x3f207d6a, 0x3f2130db, 0x3f21e396, 0x3f22959b,
    0x3f2346ea, 0x3f23f783, 0x3f24a768, 0x3f255698, 0x3f260515, 0x3f26b2dd, 0x3f275ff3, 0x3f280c56,
    0x3f28b807, 0x3f296ecb, 0x3f2a24c4, 0x3f2ad9f3, 0x3f2b8e59, 0x3f2c41f7, 0x3f2cf4cc, 0x3f2da6da,
    0x3f2e5822, 0x3f2f08a3, 0x3f2fb85f, 0x3f306756, 0x3f311589, 0x3f31c2f8, 0x3f326fa5, 0x3f331b8f,
    0x3f33c6b8, 0x3f347120, 0x3f351ac8, 0x3f35c3b1, 0x3f366bdb, 0x3f371347, 0x3f37b9f5, 0x3f385fe7,
    0x3f39051d, 0x3f39ba0b, 0x3f3a6e19, 0x3f3b2145, 0x3f3bd392, 0x3f3c8500, 0x3f3d3590, 0x3f3de543,
    0x3f3e941a, 0x3f3f4216, 0x3f3fef38, 0x3f409b81, 0x3f4146f3, 0x3f41f18d, 0x3f429b51, 0x3f434440,
    0x3f43ec5c, 0x3f4493a4, 0x3f453a1a, 0x3f45dfbf, 0x3f468494, 0x3f472899, 0x3f47cbd1, 0x3f486e3c,
    0x3f490fdb, 0x3f49c627, 0x3f4a7b71, 0x3f4b2fb8, 0x3f4be2fe, 0x3f4c9545, 0x3f4d468e, 0x3f4df6db,
    0x3f4ea62d, 0x3f4f5486, 0x3f5001e6, 0x3f50ae50, 0x3f5159c5, 0x3f520446, 0x3f52add5, 0x3f535673,
    0x3f53fe22, 0x3f54a4e3, 0x3f554ab7, 0x3f55efa0, 0x3f56939f, 0x3f5736b7, 0x3f57d8e7, 0x3f587a32,
    0x3f591a99, 0x3f59d5ac, 0x3f5a8f89, 0x3f5b4833, 0x3f5bffac, 0x3f5cb5f6, 0x3f5d6b12, 0x3f5e1f04,
    0x3f5ed1cd, 0x3f5f8370, 0x3f6033ef, 0x3f60e34c, 0x3f619189, 0x3f623ea9, 0x3f62eaac, 0x3f639597,
    0x3f643f69, 0x3f64e827, 0x3f658fd1, 0x3f66366a, 0x3f66dbf4, 0x3f678071, 0x3f6823e3, 0x3f68c64c,
    0x3f6967ae, 0x3f6a2bc7, 0x3f6aee5d, 0x3f6baf73, 0x3f6c6f0e, 0x3f6d2d2f, 0x3f6de9dc, 0x3f6ea517,
    0x3f6f5ee4, 0x3f701746, 0x3f70ce41, 0x3f7183d8, 0x3f72380e, 0x3f72eae7, 0x3f739c67, 0x3f744c8f,
    0x3f74fb64, 0x3f75a8e8, 0x3f765520, 0x3f77000d, 0x3f77a9b3, 0x3f785216, 0x3f78f938, 0x3f799f1b,
    0x3f7a43c4, 0x3f7b1712, 0x3f7be85b, 0x3f7cb7a7, 0x3f7d84fb, 0x3f7e505e, 0x3f7f19d5, 0x3f7fe167,
    0x3f80538d, 0x3f80b579, 0x3f81167b, 0x3f817696, 0x3f81d5cd, 0x3f823422, 0x3f829198, 0x3f82ee32,
    0x3f8349f3, 0x3f83a4dc, 0x3f83fef2, 0x3f845835, 0x3f84b0a9, 0x3f850850, 0x3f855f2d, 0x3f85b542,
    0x3f860a92, 0x3f868128, 0x3f86f643, 0x3f8769ea, 0x3f87dc22, 0x3f884cf1, 0x3f88bc5e, 0x3f892a6f,
    0x3f899728, 0x3f8a0291, 0x3f8a6cad, 0x3f8ad583, 0x3f8b3d18, 0x3f8ba371, 0x3f8c0893, 0x3f8c6c84,
    0x3f8ccf48, 0x3f8d30e4, 0x3f8d915c, 0x3f8df0b6, 0x3f8e4ef6, 0x3f8eac20, 0x3f8f0839, 0x3f8f6345,
    0x3f8fbd48, 0x3f904c60, 0x3f90d8e2, 0x3f9162df, 0x3f91ea68, 0x3f926f8b, 0x3f92f258, 0x3f9372dd,
    0x3f93f128, 0x3f946d47, 0x3f94e748, 0x3f955f37, 0x3f95d521, 0x3f964912, 0x3f96bb16, 0x3f972b38,
    0x3f979984, 0x3f980604, 0x3f9870c4, 0x3f98d9cc, 0x3f994128, 0x3f99a6e0, 0x3f9a0aff, 0x3f9a6d8d,
    0x3f9ace93, 0x3f9b9c8a, 0x3f9c63bf, 0x3f9d2483, 0x3f9ddf23, 0x3f9e93e3, 0x3f9f4309, 0x3f9fecd3,
    0x3fa0917c, 0x3fa1313e, 0x3fa1cc4e, 0x3fa262de, 0x3fa2f51e, 0x3fa3833c, 0x3fa40d62, 0x3fa493ba,
    0x3fa51669, 0x3fa59594, 0x3fa61160, 0x3fa689eb, 0x3fa6ff57, 0x3fa771c0, 0x3fa7e144, 0x3fa84dfe,
    0x3fa8b807, 0x3fa8fe67, 0x3fa9439f, 0x3fa987b3, 0x3fa9caad, 0x3faa0c92, 0x3faa4d68, 0x3faa8d37,
    0x3faacc04, 0x3fab09d6, 0x3fab46b1, 0x3fab829c, 0x3fabbd9c, 0x3fabf7b7, 0x3fac30f0, 0x3fac694e,
    0x3faca0d6, 0x3facd78b, 0x3fad0d72, 0x3fad4291, 0x3fad76ea, 0x3fadaa83, 0x3faddd5f, 0x3fae0f82,
    0x3fae40f1, 0x3fae7987, 0x3faeb134, 0x3faee7fd, 0x3faf1de7, 0x3faf52f8, 0x3faf8734, 0x3fafbaa0,
    0x3fafed42, 0x3fb01f1e, 0x3fb05037, 0x3fb08093, 0x3fb0b036, 0x3fb0df24, 0x3fb10d60, 0x3fb13aef,
    0x3fb167d4, 0x3fb19414, 0x3fb1bfb0, 0x3fb1eaae, 0x3fb21510, 0x3fb23eda, 0x3fb2680e, 0x3fb290b0,
    0x3fb2b8c2, 0x3fb352c3, 0x3fb3e4c2, 0x3fb46f5b, 0x3fb4f31a, 0x3fb5707b, 0x3fb5e7f1, 0x3fb659e5,
    0x3fb6c6b5, 0x3fb72eb7, 0x3fb7923c, 0x3fb7f18a, 0x3fb84ce5, 0x3fb8a48b, 0x3fb8f8b3, 0x3fb94991,
    0x3fb99756, 0x3fb9e22e, 0x3fba2a43, 0x3fba6fba, 0x3fbab2b8, 0x3fbaf35e, 0x3fbb31ca, 0x3fbb6e19,
    0x3fbba866, 0x3fbc316f, 0x3fbcaffb, 0x3fbd2530, 0x3fbd920e, 0x3fbdf771, 0x3fbe5616, 0x3fbeaea4,
    0x3fbf01ac, 0x3fbf4faf, 0x3fbf991d, 0x3fbfde5a, 0x3fc01fc0, 0x3fc05d9e, 0x3fc0983c, 0x3fc0cfd8,
    0x3fc104ae, 0x3fc136f0, 0x3fc166cd, 0x3fc19471, 0x3fc1c002, 0x3fc1e9a4, 0x3fc21176, 0x3fc23796,
    0x3fc25c20, 0x3fc27f21, 0x3fc2a0bd, 0x3fc2c107, 0x3fc2e013, 0x3fc2fdf4, 0x3fc31aba, 0x3fc33675,
    0x3fc35131, 0x3fc36afe, 0x3fc383e8, 0x3fc39bf9, 0x3fc3b33d, 0x3fc3c9be, 0x3fc3df85, 0x3fc3f49b,
    0x3fc40908, 0x3fc41cd5, 0x3fc43008, 0x3fc442a9, 0x3fc454bd, 0x3fc4664b, 0x3fc47759, 0x3fc487ec,
    0x3fc49809, 0x3fc4c5c8, 0x3fc4f002, 0x3fc5171c, 0x3fc53b6b, 0x3fc55d39, 0x3fc57cc7, 0x3fc59a4b,
    0x3fc5b5f8, 0x3fc5cff7, 0x3fc5e86f, 0x3fc5ff81, 0x3fc6154a, 0x3fc629e7, 0x3fc63d6e, 0x3fc64ff4,
    0x3fc6618d, 0x3fc6724b, 0x3fc6823c, 0x3fc69170, 0x3fc69ff3, 0x3fc6add0, 0x3fc6bb14, 0x3fc6c7c6,
    0x3fc6d3f2, 0x3fc6ead2, 0x3fc6fff0, 0x3fc7137d, 0x3fc725a5, 0x3fc7368c, 0x3fc74653, 0x3fc75515,
    0x3fc762eb, 0x3fc76feb, 0x3fc77c27, 0x3fc787af, 0x3fc79294, 0x3fc79ce2, 0x3fc7a6a5, 0x3fc7afe8,
    0x3fc7b8b5, 0x3fc7c114, 0x3fc7c90c, 0x3fc7d0a6, 0x3fc7d7e7, 0x3fc7ded6, 0x3fc7e577, 0x3fc7ebd1,
    0x3fc7f1e6, 0x3fc81ac0, 0x3fc83963, 0x3fc85138, 0x3fc86448, 0x3fc873e1, 0x3fc880e0, 0x3fc88be0,
    0x3fc8954d, 0x3fc89d79, 0x3fc8a49f, 0x3fc8aaee, 0x3fc8b089, 0x3fc8b58d, 0x3fc8ba11, 0x3fc8be27,
    0x3fc8c1de, 0x3fc8c542, 0x3fc8c85e, 0x3fc8cb3a, 0x3fc8cddd, 0x3fc8d04f, 0x3fc8d294, 0x3fc8d4b1,
    0x3fc8d6aa, 0x3fc8e643, 0x3fc8ef2c, 0x3fc8f4f1, 0x3fc8f8fa, 0x3fc8fbf6, 0x3fc8fe42, 0x3fc90014,
    0x3fc9018e, 0x3fc902c8, 0x3fc903d0, 0x3fc904b2, 0x3fc90575, 0x3fc9061f, 0x3fc906b4, 0x3fc90739,
    0x3fc907af, 0x3fc90819, 0x3fc90879, 0x3fc908d1, 0x3fc90920, 0x3fc90969, 0x3fc909ac, 0x3fc909ea,
    0x3fc90a23, 0x3fc90bb2, 0x3fc90c96, 0x3fc90d2a, 0x3fc90d91, 0x3fc90ddd, 0x3fc90e18, 0x3fc90e47,
    0x3fc90e6d, 0x3fc90e8c, 0x3fc90ea6, 0x3fc90ebd, 0x3fc90ed0, 0x3fc90ee1, 0x3fc90ef0, 0x3fc90efe,
    0x3fc90f09, 0x3fc90f14, 0x3fc90f1e, 0x3fc90f26, 0x3fc90f2e, 0x3fc90f36, 0x3fc90f3c, 0x3fc90f43,
    0x3fc90f48, 0x3fc90f70, 0x3fc90f87, 0x3fc90f96, 0x3fc90fa0, 0x3fc90fa8, 0x3fc90fae, 0x3fc90fb2,
    0x3fc90fb6, 0x3fc90fb9, 0x3fc90fbc, 0x3fc90fbe, 0x3fc90fc0, 0x3fc90fc2, 0x3fc90fc3, 0x3fc90fc5,
    0x3fc90fc6, 0x3fc90fc7, 0x3fc90fc8, 0x3fc90fc9, 0x3fc90fc9, 0x3fc90fca, 0x3fc90fcb, 0x3fc90fcb,
    0x3fc90fcc, 0xfffd2503
};

float MinSs(float a, float b) {
    return a < b ? a : b;
}

float MaxSs(float a, float b) {
    return a > b ? a : b;
}

u32 FastInverseSqrtSeed(float s) {
    return 0x5f3759dfu - (std::bit_cast<u32>(s) >> 1);
}

// The two spellings of one Newton step the firmware compiles; equal in value, kept apart so that the
// operations stay the firmware's.
float InverseSqrtNegHalf(float s) {
    const float y = std::bit_cast<float>(FastInverseSqrtSeed(s));
    return (((s * -0.5f) * y) * y + 1.5f) * y;
}

float InverseSqrtHalf(float s) {
    const float y = std::bit_cast<float>(FastInverseSqrtSeed(s));
    return (1.5f - ((s * 0.5f) * y) * y) * y;
}

// 0x3ae0: linear between the table's points, odd by recursion; past the last breakpoint, and for a
// NaN, just under π/2. The farthest point it reads is the word after the table (x on the last
// breakpoint gives position 624.0).
float TableArcTan(float x) {
    if (0.0f > x) {
        return -TableArcTan(-x);
    }
    if (x == 0.0f) {
        return 0.0f;
    }
    for (u32 i_segment = 0; i_segment < kAtanSegments; ++i_segment) {
        if (std::bit_cast<float>(kAtanBreaks[i_segment + 1]) >= x) {
            const float position = (x - std::bit_cast<float>(kAtanBreaks[i_segment])) /
                                       std::bit_cast<float>(kAtanSteps[i_segment]) +
                                   float(i_segment * kAtanPointsPerSegment);
            const s32 i_point = s32(position);
            const float low = std::bit_cast<float>(kAtanValues[i_point]);
            const float high = std::bit_cast<float>(kAtanValues[i_point + 1]);
            return low + (high - low) * (position - float(i_point));
        }
    }
    return kAtanBeyond;
}

// The elevation: atan2(y, x) over the table arctangent.
float TableArcTan2(float y, float x) {
    if (x > 0.0f) {
        return TableArcTan(y / x);
    }
    if (y >= 0.0f && 0.0f > x) {
        return TableArcTan(y / x) + kPi;
    }
    if (0.0f > y && 0.0f > x) {
        return TableArcTan(y / x) + -kPi;
    }
    if (y > 0.0f && x == 0.0f) {
        return kHalfPi;
    }
    return y < 0.0f && x == 0.0f ? -kHalfPi : 0.0f;
}

// The azimuth with the front on -z: atan2(-x, -z), spelled as the firmware tests it.
float TableAzimuth(float x, float z) {
    if (0.0f > z) {
        return TableArcTan(x / z);
    }
    if (0.0f >= x && z > 0.0f) {
        return TableArcTan(x / z) + kPi;
    }
    if (x > 0.0f && z > 0.0f) {
        return TableArcTan(x / z) + -kPi;
    }
    if (0.0f > x && z == 0.0f) {
        return kHalfPi;
    }
    return 0.0f < x && z == 0.0f ? -kHalfPi : 0.0f;
}

struct Vector {
    float x;
    float y;
    float z;
};

// A point or direction into the listener's frame: rows of m are its basis, row 3 its origin. The
// firmware adds the origin times 0 to directions, which keeps a NaN or infinite origin visible.
Vector Transform(const float* m, float x, float y, float z, float w) {
    return {((x * m[0] + y * m[4]) + z * m[8]) + m[12] * w, ((x * m[1] + y * m[5]) + z * m[9]) + m[13] * w,
            ((x * m[2] + y * m[6]) + z * m[10]) + m[14] * w};
}

// 0x2d90's distance attenuation by rolloff model, already validated.
float RolloffGain(const OrbisNgs2GeomRolloff& rolloff, float distance) {
    const float reference = rolloff.referenceDistance;
    const float max_distance = rolloff.maxDistance;
    const float factor = rolloff.rolloffFactor;
    float clamped = distance;
    if (rolloff.model >= 3) {
        clamped = distance < reference ? reference : MinSs(max_distance, distance);
    }
    float gain = 1.0f;
    switch (rolloff.model) {
    case 0:
    case 3:
        if (reference > 0.0f) {
            const float scaled = reference + factor * (clamped - reference);
            if (scaled > 0.0f) {
                gain = reference / scaled;
            }
        }
        break;
    case 1:
    case 4:
        if (!(max_distance == reference)) {
            gain = MaxSs(1.0f - (factor * (clamped - reference)) / (max_distance - reference), 0.0f);
        }
        break;
    default:
        clamped = MaxSs(kMinExponentialDistance, clamped);
        if (clamped > 0.0f && reference > 0.0f) {
            gain = std::pow(clamped / reference, -factor);
        }
        break;
    }
    return gain;
}

bool IsConeAngle(float angle) {
    return angle >= 0.0f && kMaxConeAngle >= angle;
}

float ConeLevel(float level) {
    return level < 0.0f ? 0.0f : MinSs(kMaxConeLevel, level);
}

} // namespace

s32 PS4_SYSV_ABI sceNgs2GeomResetListenerParam(OrbisNgs2GeomListenerParam* p_out_param) {
    if (p_out_param == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    p_out_param->position = {0.0f, 0.0f, 0.0f};
    p_out_param->orientFront = {0.0f, 0.0f, -1.0f};
    p_out_param->orientUp = {0.0f, 1.0f, 0.0f};
    p_out_param->velocity = {0.0f, 0.0f, 0.0f};
    p_out_param->soundSpeed = kSpeedOfSound;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2GeomResetSourceParam(OrbisNgs2GeomSourceParam* p_out_param) {
    if (p_out_param == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    p_out_param->position = {0.0f, 0.0f, 0.0f};
    p_out_param->velocity = {0.0f, 0.0f, 0.0f};
    p_out_param->direction = {0.0f, 0.0f, 0.0f};
    p_out_param->cone = {1.0f, 30.0f, std::bit_cast<float>(0x3e4ccccdu), 60.0f};
    p_out_param->rolloff = {5, 100.0f, 1.0f, 1.0f};
    p_out_param->dopplerFactor = 1.0f;
    p_out_param->fbwLevel = 1.0f;
    p_out_param->lfeLevel = 0.0f;
    p_out_param->maxLevel = 1.0f;
    p_out_param->minLevel = 0.0f;
    p_out_param->radius = 1.0f;
    p_out_param->numSpeakers = 7;
    p_out_param->matrixFormat = 8;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2GeomCalcListener(const OrbisNgs2GeomListenerParam* p_param,
                                         OrbisNgs2GeomListenerWork* p_out_work, u32 coordinate) {
    if (p_param == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_GEOM_LISTENER_ADDRESS;
    }
    if (p_out_work == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    if (coordinate > kLeftHanded) {
        return ORBIS_NGS2_ERROR_INVALID_GEOM_FLAG;
    }
    const bool is_left_handed = coordinate != 0;
    const OrbisNgs2GeomVector& front = p_param->orientFront;
    const float front_z = is_left_handed ? -front.z : front.z;
    const float front_scale = InverseSqrtNegHalf((front.x * front.x + front.y * front.y) + front.z * front.z);
    const Vector forward{front.x * front_scale, front.y * front_scale, front_z * front_scale};

    const OrbisNgs2GeomVector& up_param = p_param->orientUp;
    const float up_z = is_left_handed ? -up_param.z : up_param.z;
    const float up_scale = InverseSqrtHalf((up_param.x * up_param.x + up_param.y * up_param.y) + up_z * up_z);
    const Vector up{up_param.x * up_scale, up_param.y * up_scale, up_z * up_scale};

    const Vector cross{forward.y * up.z - forward.z * up.y, forward.z * up.x - forward.x * up.z,
                       forward.x * up.y - up.x * forward.y};
    const float right_scale = InverseSqrtHalf((cross.x * cross.x + cross.y * cross.y) + cross.z * cross.z);
    const Vector right{cross.x * right_scale, cross.y * right_scale, cross.z * right_scale};

    float(&m)[4][4] = p_out_work->matrix;
    m[0][0] = right.x;
    m[0][1] = up.x;
    m[0][2] = -forward.x;
    m[0][3] = 0.0f;
    m[1][0] = right.y;
    m[1][1] = up.y;
    m[1][2] = -forward.y;
    m[1][3] = 0.0f;
    m[2][0] = right.z;
    m[2][1] = up.z;
    m[2][2] = -forward.z;
    m[2][3] = 0.0f;
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = 0.0f;
    m[3][3] = 1.0f;

    // The origin, then the velocity, through the basis just written; the firmware's loop over the
    // fourth coordinate adds row 3 times 1 (still zero, turning -0 into +0) and times 0.
    const OrbisNgs2GeomVector& position = p_param->position;
    const float position_z = is_left_handed ? -position.z : position.z;
    const float origin_x = ((right.x * position.x + right.y * position.y) + position_z * right.z) + 1.0f * m[3][0];
    const float origin_y = ((up.x * position.x + up.y * position.y) + position_z * up.z) + 1.0f * m[3][1];
    const float origin_z =
        ((-forward.y * position.y - forward.x * position.x) - position_z * forward.z) + 1.0f * m[3][2];
    m[3][0] = -origin_x;
    m[3][1] = -origin_y;
    m[3][2] = -origin_z;

    const OrbisNgs2GeomVector& velocity = p_param->velocity;
    p_out_work->velocity = {((right.x * velocity.x + right.y * velocity.y) + velocity.z * right.z) + 0.0f * m[3][0],
                            ((up.x * velocity.x + up.y * velocity.y) + velocity.z * up.z) + 0.0f * m[3][1],
                            ((-forward.y * velocity.y - forward.x * velocity.x) + velocity.z * -forward.z) +
                                0.0f * m[3][2]};
    p_out_work->coordinate = is_left_handed ? 1 : 0;
    p_out_work->soundSpeed = p_param->soundSpeed < 0.0f ? kMinSoundSpeed : p_param->soundSpeed;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2GeomApply(const OrbisNgs2GeomListenerWork* p_listener, const OrbisNgs2GeomSourceParam* p_source,
                                  OrbisNgs2GeomAttribute* p_out_attribute, u32 flags) {
    if (p_listener == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_GEOM_LISTENER_ADDRESS;
    }
    if (p_source == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_GEOM_SOURCE_ADDRESS;
    }
    if (p_out_attribute == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    if (flags > kAllFlags || flags == 0 || (flags & (kFlagMatrix | kFlagHarmonics)) == (kFlagMatrix | kFlagHarmonics)) {
        return ORBIS_NGS2_ERROR_INVALID_GEOM_FLAG;
    }
    const OrbisNgs2GeomSourceParam& source = *p_source;
    const float* m = &p_listener->matrix[0][0];
    const bool is_left_handed = p_listener->coordinate == kLeftHanded;
    const float source_z = is_left_handed ? -source.position.z : source.position.z;
    const Vector position = Transform(m, source.position.x, source.position.y, source_z, 1.0f);
    const float direction_z = is_left_handed ? -source.direction.z : source.direction.z;
    const Vector direction = Transform(m, source.direction.x, source.direction.y, direction_z, 0.0f);
    const float direction_scale =
        InverseSqrtNegHalf((direction.x * direction.x + direction.y * direction.y) + direction.z * direction.z);
    const Vector facing{direction.x * direction_scale, direction.y * direction_scale, direction.z * direction_scale};
    const float velocity_z = is_left_handed ? -source.velocity.z : source.velocity.z;

    const float min_level = MaxSs(0.0f, source.minLevel);
    const float max_level = MaxSs(min_level, source.maxLevel);
    const float lfe_level = MaxSs(0.0f, source.lfeLevel);
    const float fbw_level = MaxSs(0.0f, source.fbwLevel);
    const float square_x = position.x * position.x;
    const float square_z = position.z * position.z;
    const float horizontal = 1.0f / InverseSqrtHalf(square_x + square_z);
    const float elevation = TableArcTan2(position.y, horizontal);
    const float azimuth = TableAzimuth(position.x, position.z);

    const OrbisNgs2GeomRolloff& rolloff = source.rolloff;
    if (0.0f > rolloff.referenceDistance || 0.0f > rolloff.maxDistance || 0.0f > rolloff.rolloffFactor ||
        rolloff.model > kMaxRolloffModel) {
        return ORBIS_NGS2_ERROR_INVALID_GEOM_DISTANCE;
    }
    const float inverse_distance = InverseSqrtHalf((position.y * position.y + square_x) + square_z);
    const float distance = 1.0f / inverse_distance;
    if (rolloff.model >= 3 && rolloff.referenceDistance > rolloff.maxDistance) {
        return ORBIS_NGS2_ERROR_INVALID_GEOM_DISTANCE;
    }
    float gain = RolloffGain(rolloff, distance);

    const Vector toward{inverse_distance * -position.x, inverse_distance * -position.y,
                        inverse_distance * -position.z};
    if ((flags & kFlagCone) != 0) {
        const OrbisNgs2GeomCone& cone = source.cone;
        if (!IsConeAngle(cone.innerAngle) || !IsConeAngle(cone.outerAngle) || cone.innerAngle > cone.outerAngle) {
            return ORBIS_NGS2_ERROR_INVALID_GEOM_CONE;
        }
        const float inner_level = ConeLevel(cone.innerLevel);
        const float angle =
            std::acos((toward.y * facing.y + toward.x * facing.x) + toward.z * facing.z) * kConeDegreesPerRadian;
        float cone_gain = inner_level;
        if (!(cone.innerAngle > angle)) {
            const float outer_level = ConeLevel(cone.outerLevel);
            const float span = cone.outerAngle - cone.innerAngle;
            cone_gain = angle > cone.outerAngle
                            ? outer_level
                            : inner_level + ((outer_level - inner_level) * (angle - cone.innerAngle)) / span;
        }
        gain = gain * cone_gain;
    }
    const float level = gain < min_level ? min_level : MinSs(max_level, gain);

    if ((flags & kFlagA3d) != 0) {
        p_out_attribute->a3dAttrib.position = {position.x, position.y, -position.z};
        p_out_attribute->a3dAttrib.volume = level * fbw_level;
    }
    if ((flags & kFlagDoppler) != 0) {
        const float doppler = source.dopplerFactor;
        const float sound_speed = p_listener->soundSpeed;
        float pitch = 1.0f;
        if (!(0.0f >= doppler) && !(0.0f > sound_speed)) {
            const Vector velocity = Transform(m, source.velocity.x, source.velocity.y, velocity_z, 0.0f);
            const float source_speed = (toward.x * velocity.x + toward.y * velocity.y) + toward.z * velocity.z;
            const OrbisNgs2GeomVector& listener_velocity = p_listener->velocity;
            const float listener_speed = (toward.x * listener_velocity.x + toward.y * listener_velocity.y) +
                                         toward.z * listener_velocity.z;
            pitch = (sound_speed - doppler * listener_speed) / (sound_speed - source_speed * doppler);
        }
        p_out_attribute->pitchRatio = pitch;
    }
    if ((flags & kFlagHarmonics) != 0) {
        const float sin_azimuth = std::sin(azimuth);
        const float cos_azimuth = std::cos(azimuth);
        const float sin_elevation = std::sin(elevation);
        const float cos_elevation = std::cos(elevation);
        const float sin_azimuth2 = std::sin(azimuth + azimuth);
        const float cos_azimuth2 = std::cos(azimuth + azimuth);
        const float sin_elevation2 = std::sin(elevation + elevation);
        const float cos_elevation_squared = cos_elevation * cos_elevation;
        float* p_levels = p_out_attribute->aLevel;
        p_levels[0] = level * kHalfPower;
        p_levels[1] = level * (cos_elevation * cos_azimuth);
        p_levels[2] = level * (cos_elevation * sin_azimuth);
        p_levels[3] = level * sin_elevation;
        p_levels[4] = level * (sin_elevation2 * cos_azimuth);
        p_levels[5] = level * (sin_elevation2 * sin_azimuth);
        p_levels[6] = level * (cos_elevation_squared * cos_azimuth2);
        p_levels[7] = level * (cos_elevation_squared * sin_azimuth2);
    }
    if ((flags & kFlagMatrix) == 0) {
        return ORBIS_OK;
    }
    std::memset(p_out_attribute->aLevel, 0, sizeof(p_out_attribute->aLevel));
    const float radius = source.radius < 0.0f ? 0.0f : MinSs(rolloff.maxDistance, source.radius);
    const float horizontal_scale = InverseSqrtNegHalf((0.0f + square_x) + square_z);
    const float side = position.x * horizontal_scale;
    const float ahead = horizontal_scale * position.z;
    float pan_angle = std::acos((side * 0.0f + (horizontal_scale * 0.0f) * 0.0f) - ahead);
    if (side < 0.0f) {
        pan_angle = -pan_angle;
    }
    OrbisNgs2PanParam pan{};
    pan.angle = pan_angle;
    pan.distance = radius == 0.0f || horizontal > radius ? 1.0f : horizontal / radius;
    pan.fbwLevel = level * fbw_level;
    pan.lfeLevel = level * lfe_level;
    OrbisNgs2PanWork work{};
    const s32 result = sceNgs2PanInit(&work, nullptr, source.numSpeakers, kRadians);
    if (result < 0) {
        return result;
    }
    return sceNgs2PanGetVolumeMatrix(&work, &pan, 1, source.matrixFormat, p_out_attribute->aLevel);
}

} // namespace Libraries::Ngs2
