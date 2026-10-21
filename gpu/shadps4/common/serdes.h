// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/assert.h"
#include "common/types.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace Serialization {

template <typename T>
concept Container = requires(T t) {
    typename T::iterator;
    { t.begin() } -> std::same_as<typename T::iterator>;
    { t.end() } -> std::same_as<typename T::iterator>;
    { t.size() } -> std::convertible_to<std::size_t>;
};

struct Archive {
    void Alloc(size_t size) {
        container.resize(size);
    }

    void Grow(size_t size) {
        container.resize(container.size() + size);
    }

    void Merge(const Archive& ar) {
        container.insert(container.end(), ar.container.cbegin(), ar.container.cend());
        offset = container.size();
    }

    [[nodiscard]] size_t SizeBytes() const {
        return container.size();
    }

    u8* CurrPtr() {
        return container.data() + offset;
    }

    void Advance(size_t size) {
        if (size > container.size() - offset) {
            MarkCorrupt("an entry shorter than what it holds");
            offset = static_cast<u32>(container.size());
            return;
        }
        offset += size;
    }

    /// psport: a cache entry that cannot be used: shorter than what it claims to hold (a file cut
    /// short by a crash or a power loss, issue #28), or rejected by the driver while preloading.
    /// Reads past the end leave zeros; the pipeline cache checks this once the entry is read,
    /// drops it and compiles it again instead of stopping the game. The first reason stays.
    void MarkCorrupt(const char* sz_reason) {
        if (!sz_corrupt) {
            sz_corrupt = sz_reason;
        }
    }

    [[nodiscard]] bool IsCorrupt() const {
        return sz_corrupt != nullptr;
    }

    [[nodiscard]] const char* CorruptReason() const {
        return sz_corrupt;
    }

    std::vector<u8>&& TakeOff() {
        offset = 0;
        return std::move(container);
    }

    [[nodiscard]] bool IsEoS() const {
        return offset >= container.size();
    }

    Archive() = default;
    explicit Archive(std::vector<u8>&& v) : container{v} {}

private:
    u32 offset{};
    std::vector<u8> container{};
    const char* sz_corrupt = nullptr;

    friend struct Writer;
    friend struct Reader;
};

struct Writer {
    template <typename T>
    void Write(const T* ptr, size_t size) {
        if (ar.offset + size >= ar.container.size()) {
            ar.Grow(size);
        }
        std::memcpy(ar.CurrPtr(), reinterpret_cast<const void*>(ptr), size);
        ar.Advance(size);
    }

    template <typename T>
        requires(!Container<T>)
    void Write(const T& value) {
        const auto size = sizeof(value);
        Write(&value, size);
    }

    void Write(const auto& v) {
        Write(v.size());
        for (const auto& elem : v) {
            Write(elem);
        }
    }

    void Write(const std::string& s) {
        Write(s.size());
        Write(s.c_str(), s.size());
    }

    Writer() = delete;
    explicit Writer(Archive& ar_) : ar{ar_} {}

    Archive& ar;
};

struct Reader {
    template <typename T>
    void Read(T* ptr, size_t size) {
        if (size > ar.container.size() - ar.offset) {
            ar.MarkCorrupt("an entry shorter than what it holds");
            std::memset(reinterpret_cast<void*>(ptr), 0, size);
            return;
        }
        std::memcpy(reinterpret_cast<void*>(ptr), ar.CurrPtr(), size);
        ar.Advance(size);
    }

    template <typename T>
        requires(!Container<T>)
    void Read(T& value) {
        const auto size = sizeof(value);
        Read(&value, size);
    }

    void Read(auto& v) {
        size_t num_elements{};
        Read(num_elements);
        if (num_elements > ar.container.size() - ar.offset) {
            ar.MarkCorrupt("an element count past the end of the entry");
            return;
        }
        for (size_t i = 0; i < num_elements && !ar.IsCorrupt(); ++i) {
            v.emplace_back();
            Read(v.back());
        }
    }

    void Read(std::string& s) {
        size_t length{};
        Read(length);
        if (length > ar.container.size() - ar.offset) {
            ar.MarkCorrupt("a string length past the end of the entry");
            return;
        }
        s.resize(length);
        Read(s.data(), length);
    }

    Reader() = delete;
    explicit Reader(Archive& ar_) : ar{ar_} {}

    Archive& ar;
};

} // namespace Serialization
