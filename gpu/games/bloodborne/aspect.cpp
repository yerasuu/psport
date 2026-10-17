// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (1.09): any output aspect ratio, live, through hooks (aspect.h). Nothing of the game's
// own code changes but the jumps to the stubs.
#include "aspect.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "psport_guest_code.h"

namespace PsAspect {
namespace {
using PsGuestCode::Emitter;
using PsGuestCode::Error;
using u64 = std::uint64_t;
using u8 = std::uint8_t;

u64 s_va_image = 0;
std::atomic<bool> s_is_camera_hooked{false};

// ---- The camera ----
// The camera manager (0x1436b..) copies the active camera's projection parameters into the
// current camera every frame (+0x50 vertical field of view, +0x54 aspect ratio, +0x58 near, +0x5c
// far: four plain copies, and a blend of two cameras, one of them per update); the projection
// (0x1d16830), the culling and the HUD's markers read them there. After the copy the output's
// aspect ratio replaces the game's 1.777 when one is set: a wider output sees more to the sides at
// the same height; a taller one keeps the width and sees more above and below (the field of view
// widened: 2 atan(tan(fov / 2) * 16:9 / aspect), x87).

/// Float bits; 0: the game's.
alignas(8) std::atomic<std::uint32_t> s_camera_aspect_bits{0};
struct alignas(8) FovScale {
    /// Float: the tangent's factor; 0: the game's field of view.
    std::atomic<std::uint32_t> bits{0};
    float half = 0.5f;
};
FovScale s_camera_fov_scale;
struct ParamStore {
    u64 off_site;
    /// The store the jump covers (no RIP-relative operand).
    std::array<u8, 5> code;
};
constexpr std::array<ParamStore, 5> CameraParamStores = {{
    {0x1436c2c, {0xc5, 0xfa, 0x11, 0x43, 0x5c}}, // vmovss [rbx + 0x5c], xmm0: the follow camera
    {0x1436d8a, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
    {0x1436e68, {0xc5, 0xf8, 0x11, 0x4b, 0x50}}, // vmovups [rbx + 0x50], xmm1: a blend of two
    {0x1436eec, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
    {0x1436f4b, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
}};

int InstallCameraHooks() {
    int c_installed = 0;
    for (const ParamStore& site : CameraParamStores) {
        const u64 va_site = s_va_image + site.off_site;
        Emitter e;
        e.Copy(site.code);
        e.Bytes({0x9c, 0x50}); // pushfq; push rax
        e.Bytes({0x48, 0xb8}); // movabs rax, &s_camera_aspect_bits
        e.U64(reinterpret_cast<u64>(&s_camera_aspect_bits));
        e.Bytes({0x8b, 0x00});             // mov eax, [rax]
        e.Bytes({0x85, 0xc0, 0x74, 0x03}); // test eax, eax; je over the store
        e.Bytes({0x89, 0x43, 0x54});       // mov [rbx + 0x54], eax
        e.Bytes({0x48, 0xb8});             // movabs rax, &s_camera_fov_scale
        e.U64(reinterpret_cast<u64>(&s_camera_fov_scale));
        e.Bytes({0x83, 0x38, 0x00, 0x74, 0x15}); // cmp dword [rax], 0; je over the x87 part
        e.Bytes({0xd9, 0x43, 0x50});             // fld dword [rbx + 0x50]: fov
        e.Bytes({0xd8, 0x48, 0x04});             // fmul dword [rax + 4]: fov / 2
        e.Bytes({0xd9, 0xf2, 0xdd, 0xd8});       // fptan; fstp st0 (its 1.0)
        e.Bytes({0xd8, 0x08});                   // fmul dword [rax]: tan * factor
        e.Bytes({0xd9, 0xe8, 0xd9, 0xf3});       // fld1; fpatan: atan(tan * factor)
        e.Bytes({0xd8, 0xc0});                   // fadd st0, st0
        e.Bytes({0xd9, 0x5b, 0x50});             // fstp dword [rbx + 0x50]
        e.Bytes({0x58, 0x9d});                   // pop rax; popfq
        e.JmpAbs(va_site + site.code.size());
        c_installed += PsGuestCode::InstallDetour("Bloodborne camera projection store", va_site, site.code, e) ==
                       Error::None;
    }
    return c_installed;
}

// ---- The HUD's markers ----
// The markers over characters (lock-on, enemies' health bars, damage numbers): projected with the
// current camera, then placed in the game's 1920x1080 UI by `* (0.5, -0.5, ..) + (0.5, 0.5, ..)`
// (three places), and drawn with the UI. The UI fills the output, its movies' 16:9 screen in the
// middle of their wider (taller) view: there a marker's 0.5 becomes 0.5 * output aspect / 16:9
// (16:9 / aspect for y), so it lands where its character is. The detour multiplies by the port's
// vector instead of the game's constant; a marker past the UI's edges is hidden by the game
// itself, as off screen.
struct MarkerSite {
    u64 off_site;
    /// The constants the two instructions read.
    u64 off_scale, off_bias;
};
constexpr std::array<MarkerSite, 3> MarkerSites = {{
    {0x1644379, 0x48f9a00, 0x48f9a10},
    {0x1644c45, 0x48f9a60, 0x48f9a70},
    {0x16452b7, 0x48f9ad0, 0x48f9ae0},
}};
struct alignas(16) MarkerScale {
    /// The game's constant.
    float game[4];
    /// What the detour multiplies by.
    std::atomic<std::uint32_t> now[4];
};
std::array<MarkerScale, MarkerSites.size()> s_marker_scales{};
std::atomic<int> s_c_marker_sites{0};
/// SetDisplayAspect's, kept for the scales.
std::atomic<float> s_marker_kx{1.0f}, s_marker_ky{1.0f};

void UpdateMarkerScales() {
    const float kx = s_marker_kx.load(std::memory_order_relaxed), ky = s_marker_ky.load(std::memory_order_relaxed);
    for (MarkerScale& scale : s_marker_scales) {
        const float now[4] = {scale.game[0] * kx, scale.game[1] * ky, scale.game[2], scale.game[3]};
        for (int c = 0; c < 4; ++c) {
            std::uint32_t bits;
            std::memcpy(&bits, &now[c], 4);
            scale.now[c].store(bits, std::memory_order_relaxed);
        }
    }
}

/// `vmulps xmm0, xmm0, [rip + disp]; vaddps xmm0, xmm0, [rip + disp]` reading the scale and the bias.
bool IsMarkerCode(const MarkerSite& site) {
    const auto* p_code = reinterpret_cast<const u8*>(s_va_image + site.off_site);
    const auto reads = [&](int off_instruction, u8 opcode, u64 off_target) {
        std::int32_t disp;
        std::memcpy(&disp, p_code + off_instruction + 4, 4);
        return p_code[off_instruction] == 0xc5 && p_code[off_instruction + 1] == 0xf8 &&
               p_code[off_instruction + 2] == opcode && p_code[off_instruction + 3] == 0x05 &&
               site.off_site + off_instruction + 8 + disp == off_target;
    };
    return reads(0, 0x59, site.off_scale) && reads(8, 0x58, site.off_bias);
}

int InstallMarkerHooks() {
    int c_installed = 0;
    for (std::size_t i = 0; i < MarkerSites.size(); ++i) {
        const MarkerSite& site = MarkerSites[i];
        const u64 va_site = s_va_image + site.off_site;
        if (!IsMarkerCode(site)) {
            std::printf("Guest code: Bloodborne HUD marker placement at %#llx not detoured: the code differs\n",
                        static_cast<unsigned long long>(va_site));
            continue;
        }
        MarkerScale& scale = s_marker_scales[i];
        std::memcpy(scale.game, reinterpret_cast<const void*>(s_va_image + site.off_scale), sizeof(scale.game));
        UpdateMarkerScales();
        u8 code[16];
        std::memcpy(code, reinterpret_cast<const void*>(va_site), sizeof(code));
        Emitter e;
        e.Bytes({0x50});       // push rax
        e.Bytes({0x48, 0xb8}); // movabs rax, &now
        e.U64(reinterpret_cast<u64>(&scale.now));
        e.Bytes({0xc5, 0xf8, 0x59, 0x00}); // vmulps xmm0, xmm0, [rax]
        e.Bytes({0x48, 0xb8});             // movabs rax, the game's bias
        e.U64(s_va_image + site.off_bias);
        e.Bytes({0xc5, 0xf8, 0x58, 0x00}); // vaddps xmm0, xmm0, [rax]
        e.Bytes({0x58});                   // pop rax
        e.JmpAbs(va_site + sizeof(code));
        c_installed += PsGuestCode::InstallDetour("Bloodborne HUD marker placement", va_site, code, e) == Error::None;
    }
    s_c_marker_sites.store(c_installed, std::memory_order_relaxed);
    return c_installed;
}

// ---- The HUD at the output's edges ----
// The game lays its HUD (menu/fe.gfx, Scaleform) out for 1920x1080: on a wider output its movie
// shows that screen in the middle, and its edges would stay there. The HUD's groups are moved
// instead, as the game moves its own clips: through the movie's members (GFx::Value,
// ObjectInterface::SetMember "x"/"y") from the thread the HUD lives on. The bars, items and gauges
// (PlayerHUD) go to the left edge, blood echoes and insight (PlayerHUD.Soul, .HeroPoint) and the
// autosave icon to the right; on a taller output the top ones go up and the boss bar down.
// PS_HUD_ANCHOR=0 keeps them in the 16:9 middle.

/// GFx::Value as this build has it (the game's own copies: 0x1adb830, its path lookup 0x1f68aa5).
/// A managed value is linked into its ObjectInterface's list (AddRef 0x2f15b0, Release 0x2f1640).
struct GfxValue {
    u64 links[2] = {~u64(0), ~u64(0)};
    void* p_object_interface = nullptr;
    std::uint32_t type = 0, pad = 0;
    union {
        double number;
        void* p_data;
    } value{};
    u64 aux = 0;
};
static_assert(sizeof(GfxValue) == 0x30);
constexpr std::uint32_t GfxManaged = 0x40, GfxNumber = 5, GfxDisplayObject = 10;
/// ObjectInterface's methods in Scaleform 4's order: AddRef, Release, HasMember, GetMember (the slot
/// the game's path lookup calls), SetMember.
constexpr std::size_t OffSlotRelease = 0x18, OffSlotGetMember = 0x28, OffSlotSetMember = 0x30;
using PfnGfxRef = void (*)(void* p_interface, GfxValue* p_value, void* p_data);
using PfnGfxGet = bool (*)(void* p_interface, void* p_data, const char* sz_name, GfxValue* p_out, bool is_dobj);
using PfnGfxSet = bool (*)(void* p_interface, void* p_data, const char* sz_name, const GfxValue* p_value, bool is_dobj);
template <typename F>
F GfxMethod(void* p_interface, std::size_t off_slot) {
    return *reinterpret_cast<F*>(*reinterpret_cast<u8**>(p_interface) + off_slot);
}
bool IsGfxObject(const GfxValue& value) {
    return value.p_object_interface && (value.type & 0x8c) == 8;
}
void GfxRelease(GfxValue& value) {
    if (value.p_object_interface && (value.type & GfxManaged)) {
        GfxMethod<PfnGfxRef>(value.p_object_interface, OffSlotRelease)(value.p_object_interface, &value,
                                                                        value.value.p_data);
    }
    value = GfxValue{};
}
/// object's member sz_name into out (released by the caller), false if none.
bool GfxGet(const GfxValue& object, const char* sz_name, GfxValue& out) {
    out = GfxValue{};
    if (!IsGfxObject(object)) {
        return false;
    }
    const bool is_dobj = (object.type & 0x8f) == GfxDisplayObject;
    return GfxMethod<PfnGfxGet>(object.p_object_interface, OffSlotGetMember)(object.p_object_interface,
                                                                             object.value.p_data, sz_name, &out,
                                                                             is_dobj);
}
bool GfxGetNumber(const GfxValue& object, const char* sz_name, double& out_number) {
    GfxValue value;
    const bool is_number = GfxGet(object, sz_name, value) && (value.type & 0x8f) == GfxNumber;
    if (is_number) {
        out_number = value.value.number;
    }
    GfxRelease(value);
    return is_number;
}
bool GfxSetNumber(const GfxValue& object, const char* sz_name, double number) {
    if (!IsGfxObject(object)) {
        return false;
    }
    GfxValue value;
    value.type = GfxNumber;
    value.value.number = number;
    const bool is_dobj = (object.type & 0x8f) == GfxDisplayObject;
    return GfxMethod<PfnGfxSet>(object.p_object_interface, OffSlotSetMember)(object.p_object_interface,
                                                                             object.value.p_data, sz_name, &value,
                                                                             is_dobj);
}

/// The game's in-game screen (vtable 0x539c988) updates once a frame (0x1bf9e10: rdi the screen,
/// xmm0 the frame time) and makes its HUD there on first use: the HUD (a value wrapper, its movie's
/// root GFx::Value at +0x30) at screen + 0xa80. The groups are moved from that update, through
/// values got and released within it: nothing of a movie is kept past its frame (the HUD movie is
/// unloaded with areas, and its ObjectInterface with it).
constexpr u64 OffHudFrameSite = 0x1bf9e10;
constexpr u8 FunctionEntryCode[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}; // push rbp; mov rbp, rsp; push r15
constexpr u64 OffScreenHud = 0xa80;
constexpr u64 OffHudWrapperVtable = 0x539a070; ///< the value wrapper's (0x539a060 + 0x10)
/// The shift for an edge, in the HUD's pixels.
std::atomic<float> s_hud_dx{0.0f}, s_hud_dy{0.0f};
std::atomic<bool> s_is_hud_anchor_on{true};
/// A HUD group: its parent (0 root, 1 PlayerHUD), name and how it follows the edges (x by dx, y by
/// dy), and where the game put it. The game's thread only.
struct HudItem {
    int parent;
    const char* sz_name;
    float fx, fy;
    double base_x = 0, base_y = 0;
    double set_x = 0, set_y = 0; ///< what the port set last
    bool is_known = false;
};
std::array<HudItem, 5> s_hud_items = {{
    {0, "PlayerHUD", -1.0f, -1.0f},
    {1, "Soul", 2.0f, 0.0f}, // in PlayerHUD: twice, for its move to the left
    {1, "HeroPoint", 2.0f, 0.0f},
    {0, "AutoSaveIcon", 1.0f, -1.0f},
    {0, "BossTop", 0.0f, 1.0f},
}};

/// Each frame, from the game's screen update: the HUD groups where the output's edges are.
void HudFrame(u64 va_screen) {
    const u64 va_hud = va_screen ? *reinterpret_cast<const u64*>(va_screen + OffScreenHud) : 0;
    if (!va_hud) {
        return;
    }
    if (*reinterpret_cast<const u64*>(va_hud) != s_va_image + OffHudWrapperVtable) {
        static bool s_has_told = false;
        if (!s_has_told) {
            s_has_told = true;
            std::printf("HUD: unexpected object at the screen's HUD: not anchored\n");
        }
        return;
    }
    const GfxValue& root = *reinterpret_cast<const GfxValue*>(va_hud + 0x30);
    static u64 s_va_last_hud = 0;
    static unsigned s_c_frames = 0;
    static float s_applied_dx = 0.0f, s_applied_dy = 0.0f;
    if (va_hud != s_va_last_hud) {
        s_va_last_hud = va_hud; // a new HUD (another area): where the game put its groups is read again
        for (HudItem& item : s_hud_items) {
            item.is_known = false;
        }
        static bool s_has_told = false;
        if (!s_has_told) {
            s_has_told = true;
            std::printf("HUD: anchored to the output's edges%s\n", s_is_hud_anchor_on ? "" : " off (PS_HUD_ANCHOR=0)");
        }
    }
    const float dx = s_is_hud_anchor_on ? s_hud_dx.load(std::memory_order_relaxed) : 0.0f;
    const float dy = s_is_hud_anchor_on ? s_hud_dy.load(std::memory_order_relaxed) : 0.0f;
    bool are_all_known = true;
    for (const HudItem& item : s_hud_items) {
        are_all_known = are_all_known && item.is_known;
    }
    // Rechecked every half second: the game may put a group back itself.
    if (dx == s_applied_dx && dy == s_applied_dy && are_all_known && ++s_c_frames % 32 != 0) {
        return;
    }
    if (!IsGfxObject(root)) {
        return;
    }
    GfxValue player;
    GfxGet(root, "PlayerHUD", player);
    for (HudItem& item : s_hud_items) {
        GfxValue clip;
        if (!GfxGet(item.parent ? player : root, item.sz_name, clip) || !IsGfxObject(clip)) {
            GfxRelease(clip);
            continue;
        }
        double x = 0, y = 0;
        if (GfxGetNumber(clip, "x", x) && GfxGetNumber(clip, "y", y)) {
            // Positions are in twips (1/20 pixel): a read back is that close.
            const auto has_moved = [](double a, double b) { return std::abs(a - b) > 0.06; };
            if (!item.is_known || has_moved(x, item.set_x) || has_moved(y, item.set_y)) {
                item.base_x = x; // first seen, or moved by the game since
                item.base_y = y;
                item.is_known = true;
            }
            const double new_x = item.base_x + item.fx * dx, new_y = item.base_y + item.fy * dy;
            if (has_moved(new_x, x)) {
                GfxSetNumber(clip, "x", new_x);
            }
            if (has_moved(new_y, y)) {
                GfxSetNumber(clip, "y", new_y);
            }
            double check_x = new_x, check_y = new_y;
            GfxGetNumber(clip, "x", check_x);
            GfxGetNumber(clip, "y", check_y);
            item.set_x = check_x;
            item.set_y = check_y;
            if (has_moved(check_x, new_x) || has_moved(check_y, new_y)) {
                static bool s_has_told = false;
                if (!s_has_told) {
                    s_has_told = true;
                    std::printf("HUD: %s did not take its place (%.1f, %.1f instead of %.1f, %.1f)\n", item.sz_name,
                                check_x, check_y, new_x, new_y);
                }
            }
        }
        GfxRelease(clip);
    }
    GfxRelease(player);
    s_applied_dx = dx;
    s_applied_dy = dy;
}

// ---- The UI's movies ----
// The menu system sets each movie's viewport every frame (0x1f59100 -> MovieImpl::SetViewport
// 0x1f510, a GFx::Viewport on its stack: buffer 1920x1080, the movie's view mode ShowAll, centred).
// Given the output's pixel aspect ratio (Viewport::AspectRatio: (output width / height) / 16:9), a
// movie shows its 16:9 screen in the middle of a wider (taller) view, squeezed into the 1920x1080
// frame, which the output stretches back (UiComposition::Scale): what lies past the game's screen
// (the HUD's groups at the edges) is drawn, not culled. PS_RE_VIEWPORT=1 logs the calls.
constexpr u64 OffSetViewportSite = 0x1f510;
std::atomic<float> s_gfx_pixel_aspect{1.0f};

void ViewportAdjust(u64 va_movie, u64 va_viewport, u64 va_caller) {
    auto* p_viewport = reinterpret_cast<std::int32_t*>(va_viewport);
    static const bool s_should_trace = [] {
        const char* sz_env = std::getenv("PS_RE_VIEWPORT");
        return sz_env && sz_env[0] == '1';
    }();
    if (s_should_trace) {
        static std::atomic<int> s_c_calls{0};
        float scale, aspect;
        std::memcpy(&scale, p_viewport + 11, 4);
        std::memcpy(&aspect, p_viewport + 12, 4);
        if (++s_c_calls <= 24) {
            std::printf("Viewport: movie %#llx from %#llx: buffer %dx%d at %d,%d %dx%d scissor %d,%d %dx%d "
                        "flags %#x scale %.3f aspect %.3f; mode %d align %d\n",
                        static_cast<unsigned long long>(va_movie),
                        static_cast<unsigned long long>(va_caller - s_va_image), p_viewport[0], p_viewport[1],
                        p_viewport[2], p_viewport[3], p_viewport[4], p_viewport[5], p_viewport[6], p_viewport[7],
                        p_viewport[8], p_viewport[9], p_viewport[10], double(scale), double(aspect),
                        *reinterpret_cast<const int*>(va_movie + 0xe8), *reinterpret_cast<const int*>(va_movie + 0xec));
        }
    }
    // The game's full-screen movies (1920x1080 in a 1920x1080 frame) only.
    if (p_viewport[0] == 1920 && p_viewport[1] == 1080 && p_viewport[2] == 0 && p_viewport[3] == 0 &&
        p_viewport[4] == 1920 && p_viewport[5] == 1080) {
        const float aspect = s_gfx_pixel_aspect.load(std::memory_order_relaxed);
        std::memcpy(p_viewport + 12, &aspect, 4);
    }
}

/// A host function called at a function's entry (its arguments kept around the call: the general
/// registers and xmm0..15), then the entry's first instructions and a jump back.
bool InstallEntryCall(const char* sz_name, u64 off_site, const void* p_function, bool should_pass_return) {
    const u64 va_site = s_va_image + off_site;
    Emitter e;
    e.SaveVolatile();
    e.SaveXmm();
    if (should_pass_return) {
        e.Bytes({0x49, 0x8b, 0x54, 0x24, 0x50}); // mov rdx, [r12 + 80]: the return address
    }
    e.CallAbs(p_function);
    e.RestoreXmm();
    e.RestoreVolatile();
    e.Copy(FunctionEntryCode);
    e.JmpAbs(va_site + sizeof(FunctionEntryCode));
    return PsGuestCode::InstallDetour(sz_name, va_site, FunctionEntryCode, e) == Error::None;
}
} // namespace

void PatchImage(unsigned char* p_image, std::uint64_t) {
    s_va_image = reinterpret_cast<u64>(p_image);
    const int c_camera = InstallCameraHooks();
    const int c_markers = InstallMarkerHooks();
    if (const char* sz_env = std::getenv("PS_HUD_ANCHOR"); sz_env && sz_env[0] == '0') {
        s_is_hud_anchor_on = false;
    }
    const bool has_viewport = InstallEntryCall("Bloodborne UI movie viewport", OffSetViewportSite,
                                               reinterpret_cast<const void*>(&ViewportAdjust), true);
    const bool has_hud = has_viewport && InstallEntryCall("Bloodborne HUD screen update", OffHudFrameSite,
                                                          reinterpret_cast<const void*>(&HudFrame), false);
    // Without every camera store the projection would switch between two aspect ratios.
    s_is_camera_hooked.store(c_camera == int(CameraParamStores.size()) && has_viewport, std::memory_order_release);
    std::printf("Aspect ratio: %s: the camera at %d of %zu places, the HUD's markers at %d of %zu, its groups %s\n",
                s_is_camera_hooked ? "follows the output" : "16:9 only (not every hook is in)", c_camera,
                CameraParamStores.size(), c_markers, MarkerSites.size(),
                has_hud ? "at the edges" : "in the middle (no hook)");
}

bool SetDisplayAspect(float aspect) {
    if (!s_is_camera_hooked.load(std::memory_order_acquire)) {
        return false;
    }
    constexpr float GameAspect = 16.0f / 9.0f;
    const bool is_other = aspect > 0.5f && aspect < 8.0f && std::abs(aspect - GameAspect) > 0.001f;
    std::uint32_t bits = 0, fov_bits = 0;
    float kx = 1.0f, ky = 1.0f; // the markers' scale: the UI is as high (wide) as the output
    if (is_other) {
        std::memcpy(&bits, &aspect, 4);
        if (aspect < GameAspect) {
            ky = GameAspect / aspect;
            std::memcpy(&fov_bits, &ky, 4);
        } else {
            kx = aspect / GameAspect;
        }
    }
    s_camera_aspect_bits.store(bits, std::memory_order_relaxed);
    s_camera_fov_scale.bits.store(fov_bits, std::memory_order_relaxed);
    // The HUD's groups: as far as the output's edge is from the 16:9 middle (the HUD's pixels).
    s_hud_dx.store(960.0f * (kx - 1.0f), std::memory_order_relaxed);
    s_hud_dy.store(540.0f * (ky - 1.0f), std::memory_order_relaxed);
    if (const char* sz_env = std::getenv("PS_HUD_DX")) { // diagnostics: another shift
        s_hud_dx.store(std::strtof(sz_env, nullptr), std::memory_order_relaxed);
    }
    s_gfx_pixel_aspect.store(is_other ? aspect / GameAspect : 1.0f, std::memory_order_relaxed);
    s_marker_kx.store(kx, std::memory_order_relaxed);
    s_marker_ky.store(ky, std::memory_order_relaxed);
    UpdateMarkerScales();
    std::printf("Display: aspect ratio %.3f (%s): camera %s, HUD markers at %d places\n", double(aspect),
                is_other ? (aspect > GameAspect ? "wider than 16:9" : "taller than 16:9") : "the game's",
                is_other ? (aspect > GameAspect ? "sees more to the sides" : "sees more above and below") : "as the game has it",
                s_c_marker_sites.load(std::memory_order_relaxed));
    return true;
}
} // namespace PsAspect
