// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "game_profile.h"

namespace Vulkan::Motion {

struct VertexRange {
    uint32_t i_first{}, c_vertices{};
    bool operator==(const VertexRange&) const = default;
};

// Reserve only the vertices referenced by this draw, not the whole shared vertex buffer.
// Primitive restart markers are not vertex shader invocations. Reject signed overflow.
template <class Index, size_t Extent>
VertexRange IndexedRange(std::span<const Index, Extent> indices, int32_t base_vertex, bool is_restart_enabled) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const auto index : indices) {
        if (is_restart_enabled && index == std::numeric_limits<Index>::max()) {
            continue;
        }
        lo = std::min(lo, uint32_t(index));
        hi = std::max(hi, uint32_t(index));
    }
    if (lo > hi || int64_t(lo) + base_vertex < 0 ||
        int64_t(hi) + base_vertex > INT32_MAX) {
        return {};
    }
    return {uint32_t(int64_t(lo) + base_vertex), hi - lo + 1};
}

// A vertex shader constant buffer (stride 16) of a G-buffer draw by its size, as the game
// profile describes its bone palettes (Game::BonePalettes).
enum class BufferRole { Other, Skeleton, SmallSkeleton };
inline BufferRole ClassifyBuffer(uint32_t cb_buffer, const Game::UpscalerPlacement& placement) {
    const auto& palettes = placement.bone_palettes;
    if (!palettes.cb_bone || cb_buffer == placement.camera.cb_constants ||
        cb_buffer > palettes.cb_max_skeleton) {
        return BufferRole::Other;
    }
    if (cb_buffer >= palettes.cb_min_skeleton) {
        return BufferRole::Skeleton;
    }
    return cb_buffer >= palettes.cb_min_small_skeleton && cb_buffer % palettes.cb_bone == 0
        ? BufferRole::SmallSkeleton : BufferRole::Other;
}

// Scanning and hashing the index list of every stored draw each frame took ~10% of the GPU
// thread. Static meshes keep their index buffers, so a result is reused and fully verified
// again every `Revalidate` frames. A stale result only costs camera-quality vectors: the
// shader bounds every history access by the stored range.
class IndexRangeCache {
public:
    static constexpr uint64_t Revalidate = 32, Unused = 600;
    struct Key {
        uint64_t va_indices{};
        uint32_t c_indices{}, cb_index{};
        int32_t base_vertex{};
        bool is_restart_enabled{};
        bool operator==(const Key&) const = default;
    };
    struct Result {
        VertexRange range;
        uint64_t topology{};
    };
    struct Stats {
        uint64_t hits{}, scans{}, stale{};
    } m_stats;

    template <class Scan>
    Result Get(const Key& key, uint64_t frame, Scan&& scan) {
        auto& entry = m_entries[key];
        entry.last_use = frame;
        if (entry.is_valid && frame - entry.verified < Revalidate) {
            ++m_stats.hits;
            return entry.result;
        }
        ++m_stats.scans;
        const Result fresh = scan();
        if (entry.is_valid && (fresh.topology != entry.result.topology ||
                            !(fresh.range == entry.result.range))) {
            ++m_stats.stale; // the cached result was wrong for up to Revalidate frames
        }
        entry.result = fresh;
        entry.verified = frame;
        entry.is_valid = true;
        return entry.result;
    }
    void Trim(uint64_t frame) {
        std::erase_if(m_entries, [frame](const auto& e) { return frame - e.second.last_use > Unused; });
    }
    size_t Size() const { return m_entries.size(); }

private:
    struct Entry {
        Result result;
        uint64_t verified{}, last_use{};
        bool is_valid{};
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            uint64_t h = k.va_indices * 0x9e3779b97f4a7c15ULL;
            h ^= (uint64_t(k.c_indices) << 32 | uint32_t(k.base_vertex)) + (h << 6) + (h >> 2);
            h ^= uint64_t(k.cb_index << 1 | uint32_t(k.is_restart_enabled)) + (h << 6) + (h >> 2);
            return size_t(h);
        }
    };
    std::unordered_map<Key, Entry, KeyHash> m_entries;
};

// Open-addressing map for the per-frame tables of History: entries are keyed per draw and
// the tables are cleared every frame. std::unordered_map allocated a node per insert (four
// per draw) and freed them all at the frame end; here clear() only starts a new generation.
template <class Key, class Value, class Hash>
class FlatMap {
public:
    FlatMap() : m_slots(64) {}

    Value& operator[](const Key& key) {
        return Insert(key).first->value;
    }
    /// Inserts `value` when `key` is absent (like std::unordered_map::emplace).
    void emplace(const Key& key, const Value& value) {
        const auto [slot, inserted] = Insert(key);
        if (inserted) {
            slot->value = value;
        }
    }
    const Value* find(const Key& key) const {
        const size_t mask = m_slots.size() - 1;
        for (size_t i = Start(key) & mask;; i = (i + 1) & mask) {
            const Slot& slot = m_slots[i];
            if (slot.generation != m_generation) {
                return nullptr;
            }
            if (slot.key == key) {
                return &slot.value;
            }
        }
    }
    void clear() {
        m_c_entries = 0;
        if (++m_generation == 0) { // wrapped: stale slots could look current
            for (auto& slot : m_slots) {
                slot.generation = 0;
            }
            m_generation = 1;
        }
    }
    void swap(FlatMap& other) noexcept {
        m_slots.swap(other.m_slots);
        std::swap(m_c_entries, other.m_c_entries);
        std::swap(m_generation, other.m_generation);
    }

private:
    struct Slot {
        Key key{};
        Value value{};
        uint32_t generation = 0;
    };
    static size_t Start(const Key& key) {
        const uint64_t h = uint64_t(Hash{}(key)) * 0x9e3779b97f4a7c15ULL;
        return size_t(h ^ (h >> 29));
    }
    std::pair<Slot*, bool> Insert(const Key& key) {
        if ((m_c_entries + 1) * 2 > m_slots.size()) {
            Grow();
        }
        const size_t mask = m_slots.size() - 1;
        for (size_t i = Start(key) & mask;; i = (i + 1) & mask) {
            Slot& slot = m_slots[i];
            if (slot.generation != m_generation) {
                slot = {key, Value{}, m_generation};
                ++m_c_entries;
                return {&slot, true};
            }
            if (slot.key == key) {
                return {&slot, false};
            }
        }
    }
    void Grow() {
        std::vector<Slot> old(m_slots.size() * 2);
        old.swap(m_slots);
        const uint32_t live = m_generation;
        m_generation = 1;
        m_c_entries = 0;
        for (const auto& slot : old) {
            if (slot.generation == live) {
                *Insert(slot.key).first = {slot.key, slot.value, m_generation};
            }
        }
    }

    std::vector<Slot> m_slots;
    size_t m_c_entries = 0;
    uint32_t m_generation = 1;
};

inline uint32_t JitterPhases(uint32_t render_width, uint32_t output_width) {
    if (!render_width || !output_width) {
        return 8;
    }
    const double ratio = double(output_width) / render_width;
    return uint32_t(std::clamp(std::ceil(8.0 * ratio * ratio), 8.0, 256.0));
}

struct Draw {
    uint64_t shader{}, geometry{}, indices{}, topology{};
    uint32_t c_indexes{}, instances{}, first_instance{};
    VertexRange vertices;
    bool operator==(const Draw& rhs) const = default;
};

// CPU bookkeeping shared with the regression test. Draw identity includes topology and
// offsets: a reused index buffer or another submesh must never read uninitialised history.
class History {
public:
    explicit History(uint32_t c_capacity) : m_c_capacity{c_capacity} {}

    struct Allocation {
        uint32_t store{}, load{}, vertices{}, instances{}, first_vertex{}, first_instance{};
    };
    struct Stats {
        uint64_t draws{}, stored{}, loaded{}, unmatched{}, exhausted{}, invalid{}, still{};
    } m_stats;

    // A small skeleton (weapon, prop; most are static world pieces) only needs history
    // while its bone palette changes: with last frame's bones its motion is the camera's.
    // Checked before the index scan of Prepare, with a cheap identity. The first frame
    // of a change stores positions (nothing to load yet), the next one loads them.
    struct GateKey {
        uint64_t shader{}, geometry{}, indices{};
        uint32_t c_indexes{}, instances{}, first_instance{}, occurrence{};
        bool operator==(const GateKey&) const = default;
    };
    bool Moving(GateKey key, uint64_t palette) {
        key.occurrence = 0;
        key.occurrence = m_gate_occurrences[key]++;
        const auto* p_prev = m_gate_previous.find(key);
        const bool is_moving = p_prev && *p_prev != palette;
        m_gate_current.emplace(key, palette);
        m_stats.still += is_moving ? 0 : 1;
        return is_moving;
    }

    void NextFrame() {
        m_previous.swap(m_current);
        m_current.clear();
        m_occurrences.clear();
        m_gate_previous.swap(m_gate_current);
        m_gate_current.clear();
        m_gate_occurrences.clear();
        ++m_frame;
        m_c_used = 0;
    }

    Allocation Prepare(const Draw& draw) {
        ++m_stats.draws;
        Key key{draw, 0};
        key.occurrence = m_occurrences[key]++;
        const uint64_t c_positions = uint64_t(draw.vertices.c_vertices) * draw.instances;
        if (!c_positions || uint64_t(draw.vertices.i_first) + draw.vertices.c_vertices - 1 > INT32_MAX ||
            uint64_t(draw.first_instance) + draw.instances > UINT32_MAX) {
            ++m_stats.invalid;
            return {};
        }
        const auto* p_prev = m_previous.find(key);
        Allocation result{0, 0, draw.vertices.c_vertices, draw.instances,
                          draw.vertices.i_first, draw.first_instance};
        if (p_prev) {
            result.load = *p_prev;
            ++m_stats.loaded;
        } else {
            ++m_stats.unmatched;
        }
        if (c_positions <= m_c_capacity - m_c_used) {
            result.store = 1 + uint32_t(m_frame & 1) * m_c_capacity + m_c_used;
            m_c_used += uint32_t(c_positions);
            m_current.emplace(key, result.store);
            ++m_stats.stored;
        } else {
            ++m_stats.exhausted;
        }
        return result;
    }

    uint32_t Used() const { return m_c_used; }

private:
    struct Key {
        Draw draw;
        uint32_t occurrence;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        size_t operator()(const Key& key) const {
            uint64_t h = 0;
            const auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
            const auto& d = key.draw;
            mix(d.shader); mix(d.geometry); mix(d.indices); mix(d.topology);
            mix(d.c_indexes); mix(d.instances); mix(d.first_instance);
            mix(d.vertices.i_first); mix(d.vertices.c_vertices); mix(key.occurrence);
            return size_t(h);
        }
    };
    uint32_t m_c_capacity{}, m_c_used{};
    uint64_t m_frame{};
    FlatMap<Key, uint32_t, Hash> m_previous, m_current, m_occurrences;
    struct GateHash {
        size_t operator()(const GateKey& key) const {
            uint64_t h = 0;
            const auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
            mix(key.shader); mix(key.geometry); mix(key.indices); mix(key.c_indexes);
            mix(key.instances); mix(key.first_instance); mix(key.occurrence);
            return size_t(h);
        }
    };
    FlatMap<GateKey, uint64_t, GateHash> m_gate_previous, m_gate_current;
    FlatMap<GateKey, uint32_t, GateHash> m_gate_occurrences;
};

} // namespace Vulkan::Motion
