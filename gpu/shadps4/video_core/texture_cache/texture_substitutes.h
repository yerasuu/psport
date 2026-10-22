// SPDX-License-Identifier: GPL-2.0-or-later
// psport: textures of the game sampled from pictures the port draws instead, known by their bytes
// (Game::TextureSubstitution; from bbport's vk_button_prompts, 72e5c8c). The pictures live in one
// image array, a layer each, uploaded when the profile's generation changes; a texture replaced
// whole samples its picture's layer, and one with cells samples an uncompressed copy of itself
// with the pictures in them, composed in stream order. Bindings that sample a picture are never
// remembered in a texture set, and a set whose image has come to be replaced misses, so a change
// of pictures needs no memo to be invalidated.
#pragma once

#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "game_profile.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

class BufferCache;
struct ImageViewInfo;

class TextureSubstitutes {
public:
    TextureSubstitutes(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, Vulkan::Runtime& runtime,
                       BufferCache& buffer_cache);
    ~TextureSubstitutes();

    TextureSubstitutes(const TextureSubstitutes&) = delete;
    TextureSubstitutes& operator=(const TextureSubstitutes&) = delete;

    /// Before an upload of `image` from the game's memory (TextureCache::RefreshImage).
    void Identify(Image& image) const;

    /// The draw recording thread, before a draw's or a dispatch's bindings: the pictures, uploaded
    /// when the profile's generation changed (ending the render pass then).
    void Update();

    /// Whether the texture helper must leave `image` to the recording thread (Replaces may
    /// identify it). Read only, valid between Update calls.
    [[nodiscard]] bool MayReplace(const Image& image) const noexcept {
        return m_is_active && (image.p_substitute || image.is_substitute_rescanned);
    }

    /// Whether `image` samples the port's pictures now (a rescanned shape is identified first).
    bool Replaces(Image& image);

    /// What to sample instead of `image` viewed as `info`, once Replaces said so; null keeps the
    /// game's own. Composing a copy with cells ends the render pass.
    vk::ImageView View(Image& image, const ImageViewInfo& info);

private:
    struct Composite {
        UniqueImage image;
        u32 width = 0, height = 0;
        bool is_srgb = false;
        bool is_current = false;
        u32 generation = 0;
        std::map<u32, vk::UniqueImageView> views; ///< by component mapping, sRGB in bit 31
    };

    bool Upload(Game::SubstitutePictures&& pictures);
    bool IsDrawn(const Game::TextureSubstitute& substitute) const;
    vk::ImageView PictureView(u32 i_picture, const ImageViewInfo& info);
    vk::ImageView CompositeView(Image& image, const Game::TextureSubstitute& substitute,
                                const ImageViewInfo& info);
    bool Compose(Composite& composite, Image& image, const Game::TextureSubstitute& substitute);
    /// Picture i_picture at side x side texels (straight alpha, averaged by coverage), its rows
    /// reversed when is_upside_down.
    void ScalePicture(u32 i_picture, u32 side, bool is_upside_down, std::vector<u32>& out) const;

    const Vulkan::Instance& m_instance;
    Vulkan::Scheduler& m_scheduler;
    Vulkan::Runtime& m_runtime;
    BufferCache& m_buffer_cache;
    Game::TextureSubstitution m_substitution;
    Game::SubstitutePictures m_pictures;
    UniqueImage m_picture_image;
    u32 m_c_layers = 0;
    u32 m_generation = 0;
    bool m_has_generation = false;
    bool m_is_active = false;
    std::map<std::tuple<u32, bool, bool, u32>, vk::UniqueImageView> m_picture_views;
    std::unordered_map<const Game::TextureSubstitute*, Composite> m_composites;
};

} // namespace VideoCore
