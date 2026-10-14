// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or L3+R3 (gamepad) opens it;
// Guide (gamepad) or Home (keyboard) asks whether to close the game. While either is open the
// game gets no pad/keyboard input. Settings live in bbport_settings.h.

#pragma once

#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal); `frame` is where the game
/// frame lies in it, under the game's cursors.
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent, vk::Rect2D frame);

/// VideoOut's hardware cursor as the game set it (game threads). The image is the game's
/// CursorImageSize x CursorImageSize A8R8G8B8 bitmap, read again at every present while enabled;
/// the position (where the hot spot goes) is in the video port's width x height.
struct GameCursor {
    const u32* image;
    s32 x, y;
    u32 hot_x, hot_y, width, height;
    bool enabled, magnified;
};
constexpr u32 CursorImageSize = 64;
constexpr u32 MaxGameCursors = 2;
void SetGameCursor(u32 index, const GameCursor& cursor);
/// A game cursor is enabled: the window leaves the mouse as a pointer and hides the system one.
bool GameCursorShown();

/// The menu, the text dialog or the close prompt is open: the game's input is held neutral.
bool CapturesInput();

/// Pad thread, scripted input (PS_PAD_FILE): Guide opens the close prompt, which cross confirms and
/// circle or Guide cancels. True when the prompt took the press, which then stays out of the game.
bool PadFilePress(bool guide, bool cross, bool circle);

/// The settings menu is open: the window shows the system cursor over it.
bool MenuOpen();

/// Window thread: the game's text dialog (ImeDialog) state, drawn as a box over the frame.
void SetTextPrompt(bool active, const std::string& prompt, const std::string& text);

} // namespace BbOverlay
