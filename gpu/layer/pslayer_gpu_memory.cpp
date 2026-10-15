// SPDX-License-Identifier: GPL-2.0-or-later
#include "pslayer_gpu_memory.h"

#include <algorithm>

namespace PsLayer {

GpuMemory& GpuMemory::Get() {
    static GpuMemory s_instance;
    return s_instance;
}

void GpuMemory::SetTranslate(GuestTranslate translate_) {
    m_pfn_translate = translate_;
}

void GpuMemory::SetBlockShift(std::uint32_t shift) {
    m_block_shift = shift;
}

void GpuMemory::AddGuestSource(std::uint64_t phys, std::uint64_t cb_range, VkBuffer h_buffer,
                               VkDeviceAddress va_device, void* p_owner) {
    std::scoped_lock lk{m_write_mutex};
    const auto* p_old = m_p_guest_sources.load(std::memory_order_acquire);
    auto* p_list = p_old ? new std::vector<MemorySource*>(*p_old) : new std::vector<MemorySource*>();
    auto* p_source = new MemorySource{MemorySource::Kind::Guest, phys, cb_range, h_buffer, va_device,
                                    p_owner};
    p_list->insert(std::ranges::upper_bound(*p_list, phys, {}, &MemorySource::base), p_source);
    m_p_guest_sources.store(p_list, std::memory_order_release);
}

const MemorySource* GpuMemory::GuestSource(std::uint64_t phys) const {
    const auto* p_list = m_p_guest_sources.load(std::memory_order_acquire);
    if (!p_list || p_list->empty()) {
        return nullptr;
    }
    // The last source starting at or below phys.
    auto it = std::ranges::upper_bound(*p_list, phys, {}, &MemorySource::base);
    if (it == p_list->begin()) {
        return nullptr;
    }
    const MemorySource* p_source = *--it;
    return phys - p_source->base < p_source->cb_source ? p_source : nullptr;
}

std::optional<Span> GpuMemory::ResolveInPlace(std::uint64_t va_range, std::uint64_t cb_range) const {
    const auto prefix = ResolvePrefix(va_range, cb_range);
    if (!prefix || prefix->cb_span != cb_range) {
        return std::nullopt;
    }
    return prefix;
}

std::optional<Span> GpuMemory::ResolvePrefix(std::uint64_t va_range, std::uint64_t cb_range) const {
    if (!m_pfn_translate || cb_range == 0) {
        return std::nullopt;
    }
    std::uint64_t phys = 0, map_end = 0;
    if (!m_pfn_translate(va_range, &phys, &map_end) || map_end <= va_range) {
        return std::nullopt;
    }
    const MemorySource* p_source = GuestSource(phys);
    if (!p_source) {
        return std::nullopt;
    }
    const std::uint64_t limit = std::min(cb_range, p_source->base + p_source->cb_source - phys);
    // The mapping may be split (protection changes): the next pieces must continue it in
    // physical memory.
    std::uint64_t covered = map_end - va_range;
    while (covered < limit) {
        const std::uint64_t at = va_range + covered;
        std::uint64_t next_phys = 0, next_end = 0;
        if (!m_pfn_translate(at, &next_phys, &next_end) || next_phys != phys + covered ||
            next_end <= at) {
            break;
        }
        covered = next_end - va_range;
    }
    return Span{p_source, phys - p_source->base, va_range, std::min(covered, limit)};
}

Resolution GpuMemory::Resolve(std::uint64_t va_range, std::uint64_t cb_range) const {
    if (cb_range == 0) {
        return {};
    }
    const std::uint64_t first = va_range >> m_block_shift;
    const std::uint64_t end = ((va_range + cb_range - 1) >> m_block_shift) + 1;
    {
        std::shared_lock lk{m_mirror_mutex};
        if (AnyValidLocked(first, end)) {
            if (AllValidLocked(first, end)) {
                auto it = m_mirrors.upper_bound(va_range);
                if (it != m_mirrors.begin()) {
                    const MemorySource& mirror = (--it)->second;
                    if (va_range >= mirror.base && va_range + cb_range <= mirror.base + mirror.cb_source) {
                        return {Resolution::Kind::Mirror,
                                Span{&mirror, va_range - mirror.base, va_range, cb_range}};
                    }
                }
            }
            return {Resolution::Kind::Mixed, {}};
        }
    }
    if (const auto span = ResolveInPlace(va_range, cb_range)) {
        return {Resolution::Kind::InPlace, *span};
    }
    return {ResolvePrefix(va_range, cb_range) ? Resolution::Kind::Mixed : Resolution::Kind::None, {}};
}

void GpuMemory::AddMirror(std::uint64_t start, std::uint64_t cb_range, VkBuffer h_buffer,
                          VkDeviceAddress va_device, void* p_owner) {
    std::unique_lock lk{m_mirror_mutex};
    m_mirrors[start] = MemorySource{MemorySource::Kind::Mirror, start, cb_range, h_buffer, va_device,
                                  p_owner};
}

void GpuMemory::RemoveMirror(std::uint64_t start) {
    std::unique_lock lk{m_mirror_mutex};
    m_mirrors.erase(start);
}

std::optional<MemorySource> GpuMemory::MirrorAt(std::uint64_t va_guest) const {
    std::shared_lock lk{m_mirror_mutex};
    auto it = m_mirrors.upper_bound(va_guest);
    if (it == m_mirrors.begin()) {
        return std::nullopt;
    }
    const MemorySource& mirror = (--it)->second;
    if (va_guest - mirror.base >= mirror.cb_source) {
        return std::nullopt;
    }
    return mirror;
}

std::vector<MemorySource> GpuMemory::MirrorsIn(std::uint64_t start, std::uint64_t end) const {
    std::vector<MemorySource> out;
    std::shared_lock lk{m_mirror_mutex};
    auto it = m_mirrors.upper_bound(start);
    if (it != m_mirrors.begin()) {
        --it;
    }
    for (; it != m_mirrors.end() && it->first < end; ++it) {
        if (it->second.base + it->second.cb_source > start) {
            out.push_back(it->second);
        }
    }
    return out;
}

void GpuMemory::MarkValid(std::uint64_t first, std::uint64_t end) {
    if (first >= end) {
        return;
    }
    std::unique_lock lk{m_mirror_mutex};
    m_valid_version.fetch_add(1, std::memory_order_release);
    // Merge with every run touching [first, end).
    auto it = m_valid.upper_bound(first);
    if (it != m_valid.begin() && std::prev(it)->second >= first) {
        --it;
    }
    std::uint64_t a = first, b = end;
    while (it != m_valid.end() && it->first <= b) {
        a = std::min(a, it->first);
        b = std::max(b, it->second);
        m_valid_blocks -= it->second - it->first;
        it = m_valid.erase(it);
    }
    m_valid[a] = b;
    m_valid_blocks += b - a;
}

void GpuMemory::MarkInvalid(std::uint64_t first, std::uint64_t end) {
    if (first >= end) {
        return;
    }
    std::unique_lock lk{m_mirror_mutex};
    m_valid_version.fetch_add(1, std::memory_order_release);
    auto it = m_valid.upper_bound(first);
    if (it != m_valid.begin()) {
        --it;
    }
    while (it != m_valid.end() && it->first < end) {
        const std::uint64_t a = it->first, b = it->second;
        if (b <= first) {
            ++it;
            continue;
        }
        it = m_valid.erase(it);
        m_valid_blocks -= b - a;
        if (a < first) {
            m_valid[a] = first;
            m_valid_blocks += first - a;
        }
        if (b > end) {
            it = m_valid.emplace(end, b).first;
            m_valid_blocks += b - end;
            ++it;
        }
    }
}

bool GpuMemory::AnyValidLocked(std::uint64_t first, std::uint64_t end) const {
    auto it = m_valid.upper_bound(first);
    if (it != m_valid.begin() && std::prev(it)->second > first) {
        return true;
    }
    return it != m_valid.end() && it->first < end;
}

bool GpuMemory::AllValidLocked(std::uint64_t first, std::uint64_t end) const {
    auto it = m_valid.upper_bound(first);
    if (it == m_valid.begin()) {
        return false;
    }
    --it;
    return it->first <= first && it->second >= end;
}

bool GpuMemory::AnyValid(std::uint64_t first, std::uint64_t end) const {
    std::shared_lock lk{m_mirror_mutex};
    return AnyValidLocked(first, end);
}

bool GpuMemory::AllValid(std::uint64_t first, std::uint64_t end) const {
    std::shared_lock lk{m_mirror_mutex};
    return AllValidLocked(first, end);
}

std::uint64_t GpuMemory::ValidBytes() const {
    std::shared_lock lk{m_mirror_mutex};
    return m_valid_blocks << m_block_shift;
}

} // namespace PsLayer
