// SPDX-License-Identifier: GPL-2.0-or-later
// psport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or L3+R3 (gamepad) opens it;
// Guide (gamepad) or Home (keyboard) asks whether to close the game. While either is open the
// game gets no pad/keyboard input. Settings live in psport_settings.h.

#pragma once

#include <span>
#include <string>
#include "../psport_gpu.h"
#include "../../src/platform/ps_host_window.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace PsOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 c_images);

/// Window thread, for every window event: true when the menu consumed it.
bool HandleEvent(const PsHostWindowEvent& event);
/// Turns the window's text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(PsHostWindow* p_window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal); `frame` is where the game
/// frame lies in it, under the game's cursors.
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent, vk::Rect2D frame);

/// VideoOut's hardware cursor as the game set it (game threads). The image is the game's
/// CursorImageSize x CursorImageSize A8R8G8B8 bitmap, read again at every present while enabled;
/// the position (where the hot spot goes) is in the video port's width x height.
struct GameCursor {
    const u32* p_image;
    s32 x, y;
    u32 hot_x, hot_y, width, height;
    bool is_enabled, is_magnified;
};
constexpr u32 CursorImageSize = 64;
constexpr u32 MaxGameCursors = 2;
void SetGameCursor(u32 i_cursor, const GameCursor& cursor);
/// A game cursor is enabled: the window leaves the mouse as a pointer and hides the system one.
bool GameCursorShown();

/// The menu, the text dialog or the close prompt is open: the game's input is held neutral.
bool CapturesInput();

/// Pad thread, scripted input (PS_PAD_FILE): Guide opens the close prompt, which cross confirms and
/// circle or Guide cancels. True when the prompt took the press, which then stays out of the game.
bool PadFilePress(bool is_guide_pressed, bool is_cross_pressed, bool is_circle_pressed);

/// The settings menu is open: the window shows the system cursor over it.
bool MenuOpen();
/// Guide's close prompt is open: Cross and Circle answer it.
bool ClosePromptOpen();

/// A line over the game near the bottom until called with nullptr or "" (any thread;
/// psport_gpu_overlay_banner).
void SetBanner(const char* sz_text);

/// Window thread: the game's text dialog (ImeDialog) state, drawn as a box over the frame.
void SetTextPrompt(bool is_active, const std::string& prompt, const std::string& text);

/// The game's message dialog (libSceMsgDialog, any thread): psport_gpu_message_begin, poll and end.
/// system_id >= 0 shows that system message instead of text. False when the overlay is not
/// initialized, since nobody could see or answer it.
bool OpenMessage(const std::string& text, int system_id, std::span<const PsMessageButton> buttons, int i_focus,
                 bool can_cancel);
int PollMessage();
void CloseMessage();

} // namespace PsOverlay
