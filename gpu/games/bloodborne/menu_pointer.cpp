// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (1.09): the mouse as a pointer in the game's Scaleform menus (menu_pointer.h).
//
// Reverse engineering (offsets in the 1.09 executable, image at 0x800000000; first published in a
// fork of bloodborne_pc, checked against the executable by bbport):
//  - 0x1acff30 updates one list component each frame it is shown (rdi the component, rsi its
//    input: the first byte is nonzero while the list has the input focus). It is reached through
//    the list class's vtable and five direct calls; one hook at its entry sees them all.
//  - The component: +0x30 its listener (vtable +0x10: cursor moved, with a pointer to the index),
//    +0x70 item count, +0x74 cursor, +0x78 cursor override (>= 0 wins), +0x7c items per line,
//    +0x80 lines, +0x84 scroll within a line, +0x24c line scroll, +0x258 the item provider
//    (vtable +0x18 makes the Scaleform value of item (cell | line << 32)), +0x2b0 wraps around,
//    +0xc8/+0xd0 the type and data of its root value.
//  - 0x1acf6b0 sets the cursor (component, index; 1 when it scrolled), 0x1ad0dd0 scrolls by
//    (lines, cells), 0x1acf0e0 hides every item's cursor.
//  - Scaleform values are 0x80 bytes: +0x30 the value, +0x40 the object interface (vtable +0x18
//    releases), +0x48 the type (& 0x40: managed; & 0x8f == 10: a display object), +0x50 its data
//    (+0x88: the native display object). 0x1adb220 finds a child by path ("Cursor" at 0x4934044,
//    "Widgets/Slider/LeftArrow", ...); a value's own vtable +8 shows or hides it.
//  - Display objects: +0x38 parent, +0x6b flags (0x40 visible), vtable +0x1f0 local bounds
//    (twips, x1 y1 x2 y2); 0x2040 their world matrix (2x4 floats, twips). The stage is 1920x1080
//    pixels, 20 twips each.
//  - 0x5580e38 the menu flow (+0xb11 zero: the title menu; +0xa88: a menu is open; +0xa78: input
//    blocked), 0x5562878 another input block (+0x2e4), 0x5128948 the region (5: Japan, circle
//    confirms), 0x557fc60 the menu sound request (+8 its label: 0x49344e4 the cursor's move).
#include "menu_pointer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../psport_gpu.h"
#include "../../../src/platform/ps_host_window.h"
#include "psport_guest_code.h"
#include "psport_overlay.h"
#include "psport_settings.h"
#include "psport_toggles.h"

namespace PsMenuPointer {
namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;

constexpr u64 ImageBase = 0x800000000ull;
constexpr u64 OffCodeEnd = 0x50d96dc;

constexpr u64 OffListUpdate = 0x1acff30;
constexpr u8 ListUpdateEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}; // push rbp; mov rbp, rsp; push r15
constexpr u64 OffSetCursor = 0x1acf6b0, OffScrollBy = 0x1ad0dd0, OffHideCursors = 0x1acf0e0;
constexpr u64 OffFindChild = 0x1adb220, OffWorldMatrix = 0x2040;
constexpr u64 OffCursorName = 0x4934044;
constexpr u64 OffMenuFlow = 0x5580e38, OffInputBlock = 0x5562878, OffRegion = 0x5128948;
constexpr u64 OffMenuSound = 0x557fc60, OffCursorMoveSound = 0x49344e4;

enum : std::uint32_t {
    Up = 0x10,
    Right = 0x20,
    Down = 0x40,
    Left = 0x80,
    Circle = 0x2000,
    Cross = 0x4000,
};

/// List component fields.
enum : std::size_t {
    OffListener = 0x30,
    OffCount = 0x70,
    OffCursor = 0x74,
    OffCursorOverride = 0x78,
    OffPerLine = 0x7c,
    OffLines = 0x80,
    OffLineScroll = 0x84,
    OffRootType = 0xc8,
    OffRootData = 0xd0,
    OffScroll = 0x24c,
    OffProvider = 0x258,
    OffWraps = 0x2b0,
};

std::atomic<bool> s_is_installed{false};
/// The 1.09 executable at its base, whether the pointer is on or not: IsInputBlocked reads its data.
std::atomic<bool> s_is_image_known{false};

u64 At(u64 off_image) {
    return ImageBase + off_image;
}

/// Copies guest memory; false where nothing is mapped (the runtime's fault recovery).
bool Read(u64 va_from, void* p_out, std::size_t cb_copy) {
    if (va_from < 0x10000) {
        return false;
    }
    PsHostRecoveryPoint recover;
    if (PS_HOST_RECOVERY_SET(recover)) {
        g_p_runtime_fault_recover = nullptr;
        return false;
    }
    g_p_runtime_fault_recover = &recover;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::memcpy(p_out, reinterpret_cast<const void*>(va_from), cb_copy);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    g_p_runtime_fault_recover = nullptr;
    return true;
}

template <typename T>
T Get(u64 va_from, T fallback = {}) {
    T value = fallback;
    return Read(va_from, &value, sizeof(value)) ? value : fallback;
}

template <typename T>
T Field(const u8* p_object, std::size_t off_field) {
    T value{};
    std::memcpy(&value, p_object + off_field, sizeof(value));
    return value;
}

/// A function of the game (its usual prologue), from a vtable or a field.
bool IsGameFunction(u64 va_function) {
    if (va_function < At(0) || va_function >= At(OffCodeEnd)) {
        return false;
    }
    u8 prologue[4] = {};
    return Read(va_function, prologue, sizeof(prologue)) && prologue[0] == 0x55 && prologue[1] == 0x48 &&
           prologue[2] == 0x89 && prologue[3] == 0xe5;
}

u64 VirtualFunction(u64 va_object, std::size_t off_slot) {
    const u64 va_vtable = Get<u64>(va_object);
    const u64 va_function = va_vtable ? Get<u64>(va_vtable + off_slot) : 0;
    return IsGameFunction(va_function) ? va_function : 0;
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// A Scaleform value the game fills in (released when it goes out of scope).
class Value {
public:
    Value() = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    ~Value() {
        if (!(Field<std::uint32_t>(m_bytes, 0x48) & 0x40)) {
            return;
        }
        const u64 va_interface = Field<u64>(m_bytes, 0x40);
        if (const u64 va_release = va_interface ? VirtualFunction(va_interface, 0x18) : 0) {
            using PfnRelease = void (*)(void*, void*, u64);
            reinterpret_cast<PfnRelease>(va_release)(reinterpret_cast<void*>(va_interface), m_bytes + 0x30,
                                                     Field<u64>(m_bytes, 0x50));
        }
    }
    void* Raw() {
        return m_bytes;
    }
    /// The native display object, or 0 (not a display object).
    u64 DisplayObject() const {
        if ((Field<std::uint32_t>(m_bytes, 0x48) & 0x8f) != 10) {
            return 0;
        }
        const u64 va_data = Field<u64>(m_bytes, 0x50);
        return va_data ? Get<u64>(va_data + 0x88) : 0;
    }
    /// Its child at sz_path ("Cursor", "Widgets/Slider/LeftArrow").
    void Child(const char* sz_path, Value& out) {
        using PfnFind = void (*)(void*, const void*, const char*, int);
        reinterpret_cast<PfnFind>(At(OffFindChild))(out.Raw(), m_bytes, sz_path, 0);
    }
    void SetVisible(bool is_visible) {
        const u64 va_vtable = Field<u64>(m_bytes, 0);
        const u64 va_set = va_vtable ? Get<u64>(va_vtable + 8) : 0;
        if (DisplayObject() && IsGameFunction(va_set)) {
            using PfnSet = void (*)(void*, int);
            reinterpret_cast<PfnSet>(va_set)(m_bytes, is_visible ? 1 : 0);
        }
    }

private:
    alignas(16) u8 m_bytes[0x80]{};
};

struct Rect {
    float x1, y1, x2, y2;
    float CenterX() const {
        return (x1 + x2) * 0.5f;
    }
};

/// Shown: it and every parent visible.
bool IsShown(u64 va_object) {
    for (int depth = 0; va_object && depth < 32; ++depth) {
        if (!(Get<u8>(va_object + 0x6b) & 0x40)) {
            return false;
        }
        va_object = Get<u64>(va_object + 0x38);
    }
    return true;
}

/// Whether stage point (x, y) is in the display object (its bounds grown by grow pixels);
/// p_out_stage gets its bounds on the stage.
bool Contains(u64 va_object, float x, float y, Rect* p_out_stage, float grow = 0.0f) {
    const u64 va_get_bounds = VirtualFunction(va_object, 0x1f0);
    if (!va_get_bounds) {
        return false;
    }
    alignas(16) const float identity[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    alignas(16) float local[4] = {};
    using PfnBounds = void (*)(float*, void*, const float*);
    reinterpret_cast<PfnBounds>(va_get_bounds)(local, reinterpret_cast<void*>(va_object), identity);
    if (!(local[2] > local[0]) || !(local[3] > local[1])) {
        return false;
    }
    alignas(16) float m[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    using PfnMatrix = void (*)(void*, float*);
    reinterpret_cast<PfnMatrix>(At(OffWorldMatrix))(reinterpret_cast<void*>(va_object), m);
    constexpr float Twips = 20.0f;
    if (p_out_stage) {
        *p_out_stage = {1e9f, 1e9f, -1e9f, -1e9f};
        for (const float lx : {local[0], local[2]}) {
            for (const float ly : {local[1], local[3]}) {
                const float sx = (m[0] * lx + m[1] * ly + m[3]) / Twips;
                const float sy = (m[4] * lx + m[5] * ly + m[7]) / Twips;
                *p_out_stage = {std::min(p_out_stage->x1, sx), std::min(p_out_stage->y1, sy),
                                std::max(p_out_stage->x2, sx), std::max(p_out_stage->y2, sy)};
            }
        }
    }
    // The point in the object's own space (the inverse of the matrix's 2x2 part).
    const float det = m[0] * m[5] - m[1] * m[4];
    if (std::fabs(det) < 1e-12f) {
        return false;
    }
    const float px = x * Twips - m[3], py = y * Twips - m[7];
    const float lx = (m[5] * px - m[1] * py) / det;
    const float ly = (m[0] * py - m[4] * px) / det;
    const float g = grow * Twips / std::sqrt(std::fabs(det));
    return lx >= local[0] - g && lx <= local[2] + g && ly >= local[1] - g && ly <= local[3] + g;
}

/// A list component of the game.
class List {
public:
    explicit List(u8* p_component) : m_p_component{p_component} {}

    int CursorIndex() const {
        const int forced = Field<std::int32_t>(m_p_component, OffCursorOverride);
        return forced >= 0 ? forced : Field<std::int32_t>(m_p_component, OffCursor);
    }
    /// A one-line spinner (one item shown of several: left and right change it).
    bool IsSpinner() const {
        return Field<std::int32_t>(m_p_component, OffLines) == 1 && Field<std::int32_t>(m_p_component, OffPerLine) == 1 &&
               Field<std::int32_t>(m_p_component, OffCount) > 1;
    }
    /// Its root display object (spinners).
    u64 Root() const {
        if ((Field<std::uint32_t>(m_p_component, OffRootType) & 0x8f) != 10) {
            return 0;
        }
        const u64 va_data = Field<u64>(m_p_component, OffRootData);
        return va_data ? Get<u64>(va_data + 0x88) : 0;
    }

    /// The shown item under stage point (x, y): its index, or -1; out_slot gets its cell and line.
    int ItemAt(float x, float y, u64& out_slot) {
        const int c_items = Field<std::int32_t>(m_p_component, OffCount);
        const int c_per_line = Field<std::int32_t>(m_p_component, OffPerLine);
        const int c_lines = Field<std::int32_t>(m_p_component, OffLines);
        if (c_items <= 0 || c_per_line <= 0 || c_lines <= 0 || c_per_line > 64 || c_lines > 64 || !IsMakerReady()) {
            return -1;
        }
        const int stride = c_lines != 1 ? c_per_line : std::max(c_per_line, c_items);
        const int line_scroll = Field<std::int32_t>(m_p_component, OffScroll);
        const int cell_scroll = Field<std::int32_t>(m_p_component, OffLineScroll);
        const bool is_wrapping = Field<u8>(m_p_component, OffWraps) != 0;
        int i_best = -1;
        float best_distance = 0.0f;
        for (int line = 0; line < c_lines; ++line) {
            for (int cell = 0; cell < c_per_line; ++cell) {
                int i_item = (line_scroll + line) * stride + cell_scroll + cell;
                if (is_wrapping) {
                    i_item %= c_items;
                }
                if (i_item < 0 || i_item >= c_items) {
                    continue;
                }
                const u64 slot = std::uint32_t(cell) | (u64(std::uint32_t(line)) << 32);
                Value item;
                MakeItem(slot, item);
                const u64 va_object = item.DisplayObject();
                if (!va_object || !IsShown(va_object)) {
                    continue;
                }
                Rect bounds{};
                if (!ItemContains(item, va_object, x, y, bounds)) {
                    continue;
                }
                const float dx = bounds.CenterX() - x, dy = (bounds.y1 + bounds.y2) * 0.5f - y;
                if (i_best < 0 || dx * dx + dy * dy < best_distance) {
                    i_best = i_item;
                    best_distance = dx * dx + dy * dy;
                    out_slot = slot;
                }
            }
        }
        return i_best;
    }

    /// The button a click at (x, y) on item slot presses: a slider's arrow or a combo box's side,
    /// else confirm.
    std::uint32_t ClickButton(u64 slot, float x, float y, std::uint32_t confirm) {
        if (!IsMakerReady()) {
            return confirm;
        }
        Value item;
        MakeItem(slot, item);
        if (!item.DisplayObject()) {
            return confirm;
        }
        const auto is_on = [&](const char* sz_path, float grow, Rect* p_bounds) {
            Value part;
            item.Child(sz_path, part);
            const u64 va_object = part.DisplayObject();
            return va_object && IsShown(va_object) && Contains(va_object, x, y, p_bounds, grow);
        };
        Rect bounds{};
        if (is_on("Widgets/Slider/LeftArrow", 14.0f, &bounds)) {
            return Left;
        }
        if (is_on("Widgets/Slider/RightArrow", 14.0f, &bounds)) {
            return Right;
        }
        if (is_on("Widgets/ComboBox", 0.0f, &bounds)) {
            return x < bounds.CenterX() ? Left : Right;
        }
        return confirm;
    }

    /// Moves the game's cursor to i_item (item slot) as its own input would: the scroll, the cursor
    /// shown, the listener told, the sound. False: it did not move there.
    bool MoveTo(int i_item, u64 slot) {
        const int i_before = CursorIndex();
        if (i_item == i_before) {
            return true;
        }
        const int line_scroll = Field<std::int32_t>(m_p_component, OffLineScroll);
        const int scroll = Field<std::int32_t>(m_p_component, OffScroll);
        using PfnSetCursor = std::int32_t (*)(u8*, int);
        const int scrolled = reinterpret_cast<PfnSetCursor>(At(OffSetCursor))(m_p_component, i_item);
        const int d_line = line_scroll - Field<std::int32_t>(m_p_component, OffLineScroll);
        const int d_scroll = scroll - Field<std::int32_t>(m_p_component, OffScroll);
        if (scrolled == 1 && (d_line || d_scroll)) {
            // The list scrolled to it: back where the pointer is, with the cursor on that item.
            using PfnScrollBy = void (*)(u8*, int, int);
            using PfnHideCursors = void (*)(u8*);
            reinterpret_cast<PfnScrollBy>(At(OffScrollBy))(m_p_component, d_line, d_scroll);
            reinterpret_cast<PfnHideCursors>(At(OffHideCursors))(m_p_component);
            if (IsMakerReady()) {
                Value item, cursor;
                MakeItem(slot, item);
                item.Child(reinterpret_cast<const char*>(At(OffCursorName)), cursor);
                cursor.SetVisible(true);
            }
        }
        const int i_now = CursorIndex();
        if (i_now != i_before) {
            CursorMoved(i_now);
        }
        return i_now == i_item;
    }

private:
    bool IsMakerReady() {
        m_va_provider = Field<u64>(m_p_component, OffProvider);
        m_va_make_item = m_va_provider ? VirtualFunction(m_va_provider, 0x18) : 0;
        return m_va_make_item != 0;
    }
    void MakeItem(u64 slot, Value& out) {
        using PfnMake = void (*)(void*, void*, const u64*);
        reinterpret_cast<PfnMake>(m_va_make_item)(out.Raw(), reinterpret_cast<void*>(m_va_provider), &slot);
    }
    /// The item's own cursor area if it has one, else the item; its combo box and slider too.
    bool ItemContains(Value& item, u64 va_object, float x, float y, Rect& out_bounds) {
        Value cursor;
        item.Child(reinterpret_cast<const char*>(At(OffCursorName)), cursor);
        const u64 va_area = cursor.DisplayObject();
        if (Contains(va_area ? va_area : va_object, x, y, &out_bounds)) {
            return true;
        }
        for (const char* sz_path : {"Widgets/ComboBox", "Widgets/Slider"}) {
            Value widget;
            item.Child(sz_path, widget);
            const u64 va_part = widget.DisplayObject();
            if (va_part && IsShown(va_part) && Contains(va_part, x, y, &out_bounds)) {
                return true;
            }
        }
        return false;
    }
    void CursorMoved(int i_item) {
        const u64 va_listener = Field<u64>(m_p_component, OffListener);
        if (const u64 va_moved = va_listener ? VirtualFunction(va_listener, 0x10) : 0) {
            std::int32_t at = i_item;
            using PfnMoved = void (*)(void*, std::int32_t*);
            reinterpret_cast<PfnMoved>(va_moved)(reinterpret_cast<void*>(va_listener), &at);
        }
        // The cursor's sound, unless one is already queued.
        auto* p_sound = reinterpret_cast<std::int32_t*>(At(OffMenuSound));
        if (p_sound[1] <= 0) {
            p_sound[0] = 0;
            p_sound[1] = 0;
            *reinterpret_cast<u64*>(At(OffMenuSound) + 8) = At(OffCursorMoveSound);
        }
    }

    u8* m_p_component;
    u64 m_va_provider = 0, m_va_make_item = 0;
};

// The pointer (window thread) and its presses (window and pad threads), for the game's thread.
std::atomic<float> s_pointer_x{0.0f}, s_pointer_y{0.0f};
std::atomic<bool> s_is_pointer_in{false};
std::atomic<bool> s_is_click_pending{false}; ///< a left click not yet used
std::atomic<std::int64_t> s_click_ms{0};
std::atomic<bool> s_is_left_held{false};
std::atomic<std::uint32_t> s_presses{0};       ///< buttons to press once
std::atomic<std::uint32_t> s_repeat_press{0};  ///< an arrow held under the pointer
std::atomic<bool> s_is_pad_steering{false};    ///< the pad's directions are in use: hover waits
std::atomic<std::int64_t> s_list_focus_ms{0};

constexpr std::int64_t ClickLifeMs = 250;
} // namespace

bool IsInputBlocked() {
    if (!s_is_image_known.load(std::memory_order_acquire)) {
        return false;
    }
    const u64 va_flow = Get<u64>(At(OffMenuFlow));
    if (va_flow && Get<u64>(va_flow + 0xa78)) {
        return true;
    }
    const u64 va_block = Get<u64>(At(OffInputBlock));
    return va_block && Get<std::uint32_t>(va_block + 0x2e4);
}

namespace {
bool IsInMenu() {
    const u64 va_flow = Get<u64>(At(OffMenuFlow));
    const bool is_title = va_flow && !Get<u8>(va_flow + 0xb11, 1);
    const bool is_menu = va_flow && Get<u64>(va_flow + 0xa88);
    return is_title || is_menu || NowMs() - s_list_focus_ms.load(std::memory_order_relaxed) < 400;
}

std::uint32_t ConfirmButton() {
    return Get<std::int32_t>(At(OffRegion), -1) == 5 ? Circle : Cross;
}

std::uint32_t BackButton() {
    return ConfirmButton() == Circle ? Cross : Circle;
}

bool HasClick() {
    if (!s_is_click_pending.load(std::memory_order_relaxed)) {
        return false;
    }
    if (NowMs() - s_click_ms.load(std::memory_order_relaxed) > ClickLifeMs) {
        s_is_click_pending.store(false, std::memory_order_relaxed);
        return false;
    }
    return true;
}

/// Where the pointer was over each list the last time it was seen: hovering acts on a move only, so
/// a pointer resting somewhere does not fight the keyboard or the pad. The game's thread only.
struct Seen {
    const u8* p_list = nullptr;
    float x = 0.0f, y = 0.0f;
    std::int64_t ms = 0;
};
Seen s_seen[32];
unsigned s_i_seen_next = 0;

/// Whether the pointer moved over p_list since it was last seen (a list not seen for a while counts
/// as not moved: it just opened under the pointer).
bool HasPointerMoved(const u8* p_list, float x, float y) {
    const std::int64_t now_ms = NowMs();
    for (Seen& seen : s_seen) {
        if (seen.p_list == p_list) {
            const bool has_moved = now_ms - seen.ms <= 250 && (seen.x != x || seen.y != y);
            seen = {p_list, x, y, now_ms};
            return has_moved;
        }
    }
    s_seen[s_i_seen_next++ % 32] = {p_list, x, y, now_ms};
    return false;
}

/// The game's list update (0x1acff30) is about to run for its component (rdi; rsi its input).
void PS4_SYSV_ABI OnListUpdate(std::uint64_t, PsGuestCode::Registers* p_registers) {
    auto* p_component = reinterpret_cast<u8*>(p_registers->rdi);
    const auto* p_input = reinterpret_cast<const u8*>(p_registers->rsi);
    if (!p_component || !p_input || !*p_input || IsInputBlocked()) {
        return;
    }
    s_list_focus_ms.store(NowMs(), std::memory_order_relaxed);
    if (PsOverlay::CapturesInput() || !s_is_pointer_in.load(std::memory_order_relaxed)) {
        return;
    }
    const float x = s_pointer_x.load(std::memory_order_relaxed);
    const float y = s_pointer_y.load(std::memory_order_relaxed);
    const bool has_moved = HasPointerMoved(p_component, x, y);
    const bool has_click = HasClick();
    List list{p_component};

    if (list.IsSpinner()) {
        // One value shown: a click on its left half goes left, on the right half right.
        Rect bounds{};
        const u64 va_root = list.Root();
        if (has_click && va_root && IsShown(va_root) && Contains(va_root, x, y, &bounds) &&
            s_is_click_pending.exchange(false, std::memory_order_relaxed)) {
            s_presses.fetch_or(x < bounds.CenterX() ? Left : Right, std::memory_order_relaxed);
        }
        return;
    }
    if ((!has_moved && !has_click) || s_is_pad_steering.load(std::memory_order_relaxed)) {
        return;
    }
    u64 slot = 0;
    const int i_item = list.ItemAt(x, y, slot);
    if (i_item < 0) {
        return;
    }
    if (has_click) {
        s_is_click_pending.store(false, std::memory_order_relaxed);
    }
    static const bool s_should_trace = std::getenv("PS_MENU_POINTER_FILE") != nullptr;
    if (s_should_trace) {
        std::printf("Menu pointer: list %p item %d (cursor %d)%s\n", static_cast<void*>(p_component), i_item,
                    list.CursorIndex(), has_click ? " click" : "");
    }
    if (list.MoveTo(i_item, slot) && has_click) {
        const std::uint32_t button = list.ClickButton(slot, x, y, ConfirmButton());
        s_presses.fetch_or(button, std::memory_order_relaxed);
        // A held click on a slider's arrow keeps moving it.
        if (button == Left || button == Right) {
            s_repeat_press.store(button, std::memory_order_relaxed);
        }
    }
}

/// Window coordinates to the stage's (1920x1080, centred in the shown frame, scaled by height).
void SetPointer(PsHostWindow* p_window, float window_x, float window_y) {
    int width = 0, height = 0;
    ps_host_window_size(p_window, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    const float aspect = std::max(PsSettings::Get().shown_frame_aspect.load(std::memory_order_relaxed), 0.1f);
    const float frame_height = std::min(float(height), float(width) / aspect);
    const float scale = frame_height / 1080.0f;
    s_pointer_x.store((window_x - width * 0.5f) / scale + 960.0f, std::memory_order_relaxed);
    s_pointer_y.store((window_y - height * 0.5f) / scale + 540.0f, std::memory_order_relaxed);
    s_is_pointer_in.store(true, std::memory_order_relaxed);
}

/// PS_MENU_POINTER_FILE=<file> (tests): "x y [click|right|up|down]" in stage pixels, applied when
/// the file changes, as the mouse would.
void ReadTestFile() {
    static const char* s_sz_path = std::getenv("PS_MENU_POINTER_FILE");
    static std::int64_t s_next_ms = 0;
    static char s_last[128] = {};
    const std::int64_t now_ms = NowMs();
    if (!s_sz_path || now_ms < s_next_ms) {
        return;
    }
    s_next_ms = now_ms + 200;
    char text[128] = {};
    if (FILE* p_file = std::fopen(s_sz_path, "r")) {
        const std::size_t cb_read = std::fread(text, 1, sizeof(text) - 1, p_file);
        text[cb_read] = 0;
        std::fclose(p_file);
    }
    if (!text[0] || std::strcmp(text, s_last) == 0) {
        return;
    }
    std::memcpy(s_last, text, sizeof(s_last));
    float x = 0.0f, y = 0.0f;
    char action[16] = {};
    if (std::sscanf(text, "%f %f %15s", &x, &y, action) < 2) {
        return;
    }
    s_pointer_x.store(x, std::memory_order_relaxed);
    s_pointer_y.store(y, std::memory_order_relaxed);
    s_is_pointer_in.store(true, std::memory_order_relaxed);
    if (!std::strcmp(action, "click")) {
        s_click_ms.store(now_ms, std::memory_order_relaxed);
        s_is_click_pending.store(true, std::memory_order_relaxed);
    } else if (!std::strcmp(action, "right")) {
        s_presses.fetch_or(BackButton(), std::memory_order_relaxed);
    } else if (!std::strcmp(action, "up") || !std::strcmp(action, "down")) {
        s_presses.fetch_or(action[0] == 'u' ? Up : Down, std::memory_order_relaxed);
    }
    std::printf("Menu pointer: test %.0f %.0f %s (in a menu: %d)\n", x, y, action, IsInMenu() ? 1 : 0);
}
} // namespace

void PatchImage(unsigned char* p_image, std::uint64_t cb_image) {
    if (reinterpret_cast<u64>(p_image) != ImageBase) {
        std::printf("Menu pointer: the image is not at %#llx: off\n", static_cast<unsigned long long>(ImageBase));
        return;
    }
    s_is_image_known.store(cb_image >= OffListUpdate + sizeof(ListUpdateEntry) &&
                               std::memcmp(p_image + OffListUpdate, ListUpdateEntry, sizeof(ListUpdateEntry)) == 0,
                           std::memory_order_release);
    if (const char* sz_env = std::getenv("PS_MENU_POINTER"); sz_env && sz_env[0] == '0') {
        std::printf("Menu pointer: off (PS_MENU_POINTER=0)\n");
        return;
    }
    if (PsGuestCode::InstallHook("Bloodborne menu list update", At(OffListUpdate), ListUpdateEntry, 0, OnListUpdate) !=
        PsGuestCode::Error::None) {
        std::printf("Menu pointer: off (the game's list update is not the expected code)\n");
        return;
    }
    s_is_installed.store(true, std::memory_order_release);
    std::printf("Menu pointer: on (the mouse points and clicks in the game's menus)\n");
}

bool WantsPointer() {
    if (!s_is_installed.load(std::memory_order_acquire)) {
        return false;
    }
    const bool is_menu = IsInMenu();
    static bool s_was_menu = false;
    if (is_menu != s_was_menu) {
        s_was_menu = is_menu;
        std::printf("Menu pointer: %s\n", is_menu ? "a game menu is open (pointer)" : "menus closed");
    }
    return is_menu;
}

bool WindowEvent(const PsHostWindowEvent* p_event, PsHostWindow* p_window) {
    if (!s_is_installed.load(std::memory_order_acquire)) {
        return false;
    }
    const PsHostWindowEvent& event = *p_event;
    switch (event.kind) {
    case PS_HOST_WINDOW_EVENT_MOUSE_MOTION:
        if (!psport_gpu_mouse_captured()) {
            SetPointer(p_window, event.motion.x, event.motion.y);
        }
        return false;
    case PS_HOST_WINDOW_EVENT_MOUSE_LEAVE:
        s_is_pointer_in.store(false, std::memory_order_relaxed);
        return false;
    default:
        break;
    }
    const bool is_ours = !psport_gpu_mouse_captured() && !PsOverlay::MenuOpen() && IsInMenu();
    if (event.kind == PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_UP && event.button.button == PS_HOST_INPUT_MOUSE_LEFT) {
        s_is_left_held.store(false, std::memory_order_relaxed);
        s_repeat_press.store(0, std::memory_order_relaxed);
        return is_ours;
    }
    if (!is_ours) {
        return false;
    }
    if (event.kind == PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_DOWN) {
        SetPointer(p_window, event.button.x, event.button.y);
        if (event.button.button == PS_HOST_INPUT_MOUSE_LEFT) {
            s_is_left_held.store(true, std::memory_order_relaxed);
            s_click_ms.store(NowMs(), std::memory_order_relaxed);
            s_is_click_pending.store(true, std::memory_order_relaxed);
        } else if (event.button.button == PS_HOST_INPUT_MOUSE_RIGHT) {
            s_presses.fetch_or(BackButton(), std::memory_order_relaxed);
        }
        return true;
    }
    if (event.kind == PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_UP) {
        return true;
    }
    if (event.kind == PS_HOST_WINDOW_EVENT_MOUSE_WHEEL) {
        const float notches = event.wheel.is_flipped ? -event.wheel.y : event.wheel.y;
        if (notches != 0.0f) {
            s_presses.fetch_or(notches > 0.0f ? Up : Down, std::memory_order_relaxed);
        }
        return true;
    }
    return false;
}

void FilterPad(std::uint32_t* p_buttons, const std::uint8_t* p_sticks) {
    if (!s_is_installed.load(std::memory_order_acquire)) {
        return;
    }
    const auto is_off_centre = [](u8 value) { return value < 64 || value > 192; };
    s_is_pad_steering.store((*p_buttons & (Up | Right | Down | Left)) || is_off_centre(p_sticks[0]) ||
                                is_off_centre(p_sticks[1]),
                            std::memory_order_relaxed);
    ReadTestFile();
    // The pointer's presses, each held long enough for the game to see it, with a gap after.
    static std::uint32_t s_holding = 0;
    static std::int64_t s_until_ms = 0, s_repeat_ms = 0;
    const std::int64_t now_ms = NowMs();
    if (!IsInMenu() || PsOverlay::CapturesInput()) {
        s_presses.store(0, std::memory_order_relaxed);
        s_repeat_press.store(0, std::memory_order_relaxed);
        s_holding = 0;
        return;
    }
    constexpr std::int64_t PressMs = 50, GapMs = 40, RepeatDelayMs = 400, RepeatMs = 120;
    if (s_holding && now_ms >= s_until_ms) {
        s_holding = 0;
        s_until_ms = now_ms + GapMs;
    }
    if (!s_holding && now_ms >= s_until_ms) {
        if (const std::uint32_t next = s_presses.exchange(0, std::memory_order_relaxed)) {
            s_holding = next;
            s_until_ms = now_ms + PressMs;
            s_repeat_ms = now_ms + RepeatDelayMs;
        } else if (const std::uint32_t arrow = s_repeat_press.load(std::memory_order_relaxed);
                   arrow && s_is_left_held.load(std::memory_order_relaxed) && now_ms >= s_repeat_ms) {
            s_holding = arrow;
            s_until_ms = now_ms + PressMs;
            s_repeat_ms = now_ms + RepeatMs;
        }
    }
    *p_buttons |= s_holding;
}
} // namespace PsMenuPointer
