// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_free_check.h"
#include "game_profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <pthread.h>
#include <sys/mman.h>
#include <unordered_set>
#include <unistd.h>

#include "../../src/platform/ps_host.h"

namespace PsFreeCheck {
namespace {
using u64 = std::uint64_t;
using u32 = std::uint32_t;

constexpr u64 PageSize = 0x1000;
/// Blocks a page can have: the free bitmasks hold 128.
constexpr u64 MaxPoolBlocks = 128;
/// The game's small-object pool (Game::SmallObjectPool), set once by Enabled().
u64 s_off_header = 0, s_cb_block = 0, s_max_blocks = 0;

const char* const SourceNames[] = {"EOP fence",  "EOS fence",   "EOS GDS store", "WriteData",
                                   "compute WriteData", "ReleaseMem", "DumpConstRam",
                                   "DMA fill",   "DMA copy",    "buffer download",
                                   "image download"};

const char* Name(u32 source) {
    return source < std::size(SourceNames) ? SourceNames[source] : "?";
}

struct Header {
    u64 next, prev, head, c_blocks;
};

bool IsBlock(u64 page, u64 p) {
    return p >= page && p - page < s_off_header && (p - page) % s_cb_block == 0;
}

bool Plausible(u64 page, const Header& h) {
    const auto pointer = [](u64 p) { return p != 0 && (p & 7) == 0 && p < (1ull << 47); };
    // A page with every block free has gone back to the allocator below (by the pool's
    // release): its old header means nothing. The page list links point at other page
    // headers or at the pool's list heads; at least one of them is a header (pools of one
    // page are skipped: other data at a page end looked like headers).
    if (!pointer(h.next) || !pointer(h.prev) || h.c_blocks >= s_max_blocks) {
        return false;
    }
    if ((h.next & (PageSize - 1)) != s_off_header && (h.prev & (PageSize - 1)) != s_off_header) {
        return false;
    }
    return h.c_blocks == 0 ? h.head == 0 : IsBlock(page, h.head);
}

struct WalkResult {
    bool is_ok;
    u64 holder;   ///< where the bad pointer is stored (a free block, or the header's head)
    u64 value;    ///< the bad pointer
    u32 length;   ///< free blocks walked
    u64 free[2];  ///< bit per block index: free
};

bool IsFree(const u64 (&free)[2], u64 i_block) {
    return (free[i_block / 64] >> (i_block % 64)) & 1;
}

/// Walks the free list of `page`; `read(address)` returns the qword there.
template <typename Read>
WalkResult WalkFree(u64 page, const Header& h, Read&& read) {
    WalkResult r{true, page + s_off_header + 16, h.head, 0, {0, 0}};
    u64 node = h.head;
    while (node != 0) {
        if (!IsBlock(page, node) || r.length >= h.c_blocks) {
            r.is_ok = false;
            r.value = node;
            return r;
        }
        const u64 i_block = (node - page) / s_cb_block;
        if (IsFree(r.free, i_block)) {
            r.is_ok = false; // a loop
            r.value = node;
            return r;
        }
        r.free[i_block / 64] |= 1ull << (i_block % 64);
        ++r.length;
        r.holder = node;
        node = read(node);
    }
    if (r.length != h.c_blocks) {
        r.is_ok = false;
        r.value = 0;
    }
    return r;
}

u64 NowNs() {
    static const auto s_start = std::chrono::steady_clock::now();
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - s_start)
                   .count());
}

u32 Tid() {
    static thread_local const u32 s_tid = u32(gettid());
    return s_tid;
}

// Recent emulator writes into pool pages.
struct Entry {
    u64 va_range, cb_range, value, ns, seq;
    u32 source, tid;
};
std::array<Entry, 1 << 15> s_ring;
std::atomic<u64> s_ring_head{0};

void Record(u64 va_range, u64 cb_range, u64 value, Source source, u64 seq) {
    s_ring[s_ring_head.fetch_add(1, std::memory_order_relaxed) % s_ring.size()] =
        Entry{va_range, cb_range, value, NowNs(), seq, u32(source), Tid()};
}

// Pool pages the watcher walks: an open-addressing set, insert-only.
std::array<std::atomic<u64>, 1 << 14> s_pages{};
std::atomic<u32> s_c_pages{0};

void Watch(u64 page) {
    static thread_local u64 s_last = 0;
    if (page == s_last) {
        return;
    }
    s_last = page;
    u64 slot = (page >> 12) * 0x9E3779B97F4A7C15ull >> 50;
    for (u32 probe = 0; probe < 64; ++probe, slot = (slot + 1) % s_pages.size()) {
        u64 expected = s_pages[slot].load(std::memory_order_relaxed);
        if (expected == page) {
            return;
        }
        if (expected == 0) {
            if (s_pages[slot].compare_exchange_strong(expected, page) || expected == page) {
                if (expected != page) {
                    s_c_pages.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
        }
    }
}

std::atomic<u64> s_checked{0};
std::atomic<u64> s_c_free_hits{0};
std::atomic<u64> s_c_header_hits{0};
std::atomic<u64> s_c_corruptions{0};
std::atomic<u64> s_unordered{0};
std::atomic<u64> s_next_seq{0};
std::atomic<u64> s_max_written_seq{0};

// Fences as the GPU command thread decoded them, and the guest's graphics submissions.
struct Decoded {
    u64 label, value, packet, buffer, submit, ns;
};
std::array<Decoded, 1 << 15> s_decoded;
std::atomic<u64> s_decoded_head{0};
struct Submitted {
    u64 submit, buffer, cb_range, ns;
};
std::array<Submitted, 1 << 12> s_submitted;
std::atomic<u64> s_submitted_head{0};

/// When the guest submitted `submit` (0 when no longer in the log).
u64 SubmitTime(u64 submit) {
    const u64 c_submitted = s_submitted_head.load(std::memory_order_relaxed);
    for (u64 i = c_submitted; i-- > (c_submitted > s_submitted.size() ? c_submitted - s_submitted.size() : 0);) {
        const Submitted s = s_submitted[i % s_submitted.size()];
        if (s.submit == submit) {
            return s.ns;
        }
    }
    return 0;
}

/// The decoded fences whose label lies in [begin, end), newest first.
void PrintDecoded(u64 begin, u64 end, u64 now, u32 limit) {
    const u64 c_decoded = s_decoded_head.load(std::memory_order_relaxed);
    u32 shown = 0;
    for (u64 i = c_decoded; i-- > (c_decoded > s_decoded.size() ? c_decoded - s_decoded.size() : 0) && shown < limit;) {
        const Decoded d = s_decoded[i % s_decoded.size()];
        if (d.label < begin || d.label >= end) {
            continue;
        }
        const u64 submit_ns = SubmitTime(d.submit);
        std::fprintf(stderr,
                     "Free check:   %8.3f ms before: decoded fence %#llx value %#llx, packet %#llx "
                     "in buffer %#llx, submission %llu (submitted %.3f ms before)\n",
                     double(now - d.ns) / 1e6, (unsigned long long)d.label,
                     (unsigned long long)d.value, (unsigned long long)d.packet,
                     (unsigned long long)d.buffer, (unsigned long long)d.submit,
                     submit_ns ? double(now - submit_ns) / 1e6 : -1.0);
        ++shown;
    }
    if (shown == 0) {
        std::fprintf(stderr, "Free check:   no decoded fence there in the log\n");
    }
}

/// Each kind of report has its own budget: a flood of one does not hide the others.
struct Budget {
    std::atomic<u32> m_used{0};
    u32 m_limit;
    bool Take() {
        return m_used.fetch_add(1, std::memory_order_relaxed) < m_limit;
    }
};
Budget s_write_reports{.m_limit = 64};
Budget s_corruption_reports{.m_limit = 64};
Budget s_order_reports{.m_limit = 32};
Budget s_latency_reports{.m_limit = 32};
Budget s_duplicate_reports{.m_limit = 32};

// Fence latency and duplicates: when each fence reached the draw recording thread (NextFenceSeq)
// and which sequence number each slot last wrote. A guest that waits for a label with a
// timeout frees its object when the label is late; a fence written twice lands late too.
constexpr u64 SeqSlots = 1 << 16;
std::array<std::atomic<u64>, SeqSlots> s_seq_start_ns{};
std::array<std::atomic<u64>, SeqSlots> s_seq_written{};
std::atomic<u64> s_c_duplicates{0}, s_c_late_20ms{0}, s_c_late_50ms{0}, s_max_latency_ns{0};

void PrintEntry(const Entry& e, u64 now) {
    std::fprintf(stderr,
                 "Free check:   %8.3f ms before: %s %#llx +%llu value %#llx seq %llu tid %u\n",
                 double(now - e.ns) / 1e6, Name(e.source), (unsigned long long)e.va_range,
                 (unsigned long long)e.cb_range, (unsigned long long)e.value,
                 (unsigned long long)e.seq, e.tid);
}

/// The logged writes overlapping [begin, end), newest first.
void PrintRecent(u64 begin, u64 end, u64 now, u32 limit) {
    const u64 c_recorded = s_ring_head.load(std::memory_order_relaxed);
    u32 shown = 0;
    for (u64 i = c_recorded; i-- > (c_recorded > s_ring.size() ? c_recorded - s_ring.size() : 0) && shown < limit;) {
        const Entry e = s_ring[i % s_ring.size()];
        if (e.va_range + e.cb_range <= begin || e.va_range >= end) {
            continue;
        }
        PrintEntry(e, now);
        ++shown;
    }
    if (shown == 0) {
        std::fprintf(stderr, "Free check:   no emulator writes there in the log\n");
    }
}

void PrintBlock(const unsigned char* p_bytes, u64 page, u64 block) {
    for (u64 at = block; at < block + s_cb_block && at + 8 <= page + PageSize; at += 8) {
        u64 v;
        std::memcpy(&v, p_bytes + (at - page), 8);
        std::fprintf(stderr, "Free check:   [%#llx] = %#llx\n", (unsigned long long)at,
                     (unsigned long long)v);
    }
}

bool ReadPage(u64 page, unsigned char* p_out) {
    return ps_host_read_memory(p_out, reinterpret_cast<const void*>(page), PageSize) == std::int64_t(PageSize);
}

/// What the watcher saw of a page's blocks: when each was last seen taken and released by the
/// game (to ~1 ms).
struct Track {
    bool is_valid = false;
    u64 free[2]{};
    std::array<u64, MaxPoolBlocks> allocated_at{}, freed_at{};
};

void Watcher() {
    ps_host_thread_set_name("psFreeCheck");
    std::array<u64, 256> reported{};
    u32 reported_next = 0;
    std::unordered_map<u64, Track> tracks;
    alignas(64) unsigned char buf[PageSize];
    const auto walk_page = [&](u64 page, Header& h, WalkResult& r) {
        if (!ReadPage(page, buf)) {
            return false;
        }
        std::memcpy(&h, buf + s_off_header, sizeof(h));
        if (!Plausible(page, h)) {
            return false;
        }
        r = WalkFree(page, h, [&](u64 at) {
            u64 v;
            std::memcpy(&v, buf + (at - page), 8);
            return v;
        });
        return true;
    };
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        static u64 s_last_report = 0, s_last_l20 = 0, s_last_l50 = 0, s_last_dup = 0;
        if (const u64 now = NowNs(); now - s_last_report > 10000000000ull) {
            const u64 l20 = s_c_late_20ms.load(), l50 = s_c_late_50ms.load(), dup = s_c_duplicates.load();
            if (s_last_report) {
                std::fprintf(stderr,
                             "Free check: fences in 10 s: %llu over 20 ms, %llu over 50 ms, %llu "
                             "written twice (max latency so far %.1f ms)\n",
                             (unsigned long long)(l20 - s_last_l20),
                             (unsigned long long)(l50 - s_last_l50),
                             (unsigned long long)(dup - s_last_dup), s_max_latency_ns.load() / 1e6);
            }
            s_last_report = now;
            s_last_l20 = l20;
            s_last_l50 = l50;
            s_last_dup = dup;
        }
        for (auto& slot : s_pages) {
            const u64 page = slot.load(std::memory_order_relaxed);
            if (page == 0) {
                continue;
            }
            Header h;
            WalkResult r;
            if (!walk_page(page, h, r)) {
                tracks.erase(page);
                continue;
            }
            if (r.is_ok) {
                const u64 now = NowNs();
                Track& t = tracks[page];
                for (u64 i = 0; i < s_max_blocks; ++i) {
                    const bool is_previous_free = IsFree(t.free, i), is_now_free = IsFree(r.free, i);
                    if (t.is_valid && is_previous_free != is_now_free) {
                        (is_now_free ? t.freed_at : t.allocated_at)[i] = now;
                    }
                }
                t.free[0] = r.free[0];
                t.free[1] = r.free[1];
                t.is_valid = true;
                continue;
            }
            // The game may be inside an allocation or release: the same damage three times
            // over ~1 ms is not a race with it.
            bool is_persistent = true;
            for (int again = 0; again < 3 && is_persistent; ++again) {
                std::this_thread::sleep_for(std::chrono::microseconds(300));
                Header h2;
                WalkResult r2;
                is_persistent = walk_page(page, h2, r2) && !r2.is_ok && r2.holder == r.holder &&
                             r2.value == r.value;
            }
            if (!is_persistent) {
                continue;
            }
            const u64 key = r.holder ^ (r.value * 0x9E3779B97F4A7C15ull);
            if (std::find(reported.begin(), reported.end(), key) != reported.end()) {
                continue;
            }
            reported[reported_next++ % reported.size()] = key;
            s_c_corruptions.fetch_add(1, std::memory_order_relaxed);
            if (!s_corruption_reports.Take()) {
                continue;
            }
            const u64 now = NowNs();
            std::fprintf(stderr,
                         "Free check: CORRUPTED free list in pool page %#llx at %.3f s: "
                         "%#llx holds %#llx (%u of %llu free blocks walked; header next %#llx "
                         "prev %#llx head %#llx count %llu)\n",
                         (unsigned long long)page, double(now) / 1e9,
                         (unsigned long long)r.holder, (unsigned long long)r.value, r.length,
                         (unsigned long long)h.c_blocks, (unsigned long long)h.next,
                         (unsigned long long)h.prev, (unsigned long long)h.head,
                         (unsigned long long)h.c_blocks);
            if (r.holder < page + s_off_header) {
                PrintBlock(buf, page, r.holder);
                const u64 i_block = (r.holder - page) / s_cb_block;
                if (const auto it = tracks.find(page); it != tracks.end()) {
                    const Track& t = it->second;
                    const auto ago = [&](u64 at) { return at ? double(now - at) / 1e6 : -1.0; };
                    std::fprintf(stderr,
                                 "Free check:   block %#llx: last taken %.3f ms ago, last "
                                 "released %.3f ms ago (-1: not seen), free in the last good "
                                 "walk: %s\n",
                                 (unsigned long long)r.holder, ago(t.allocated_at[i_block]),
                                 ago(t.freed_at[i_block]), IsFree(t.free, i_block) ? "yes" : "no");
                }
                std::fprintf(stderr, "Free check:   emulator writes into this block:\n");
                PrintRecent(r.holder, r.holder + s_cb_block, now, 24);
                std::fprintf(stderr, "Free check:   fences decoded for this block:\n");
                PrintDecoded(r.holder, r.holder + s_cb_block, now, 16);
            }
            std::fprintf(stderr, "Free check:   emulator writes into the page:\n");
            PrintRecent(page, page + PageSize, now, 32);
            std::fprintf(stderr, "Free check:   fences written out of stream order so far: "
                                 "%llu\n",
                         (unsigned long long)s_unordered.load());
        }
    }
}

void StartWatcher() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        std::fprintf(stderr, "Free check: on (PS_FREE_CHECK=1): pool pages of %llu-byte "
                             "blocks, header at +%#llx\n",
                     (unsigned long long)s_cb_block, (unsigned long long)s_off_header);
        std::thread(Watcher).detach();
    });
}

/// One page of a write: report a write into a free block or the header.
void CheckPage(u64 page, u64 begin, u64 end, u64 value, u64 cb_range, Source source, u64 seq,
               bool is_small) {
    Header h;
    std::memcpy(&h, reinterpret_cast<const void*>(page + s_off_header), sizeof(h));
    if (!Plausible(page, h)) {
        return;
    }
    if (is_small) {
        Watch(page);
    }
    Record(begin, end - begin, value, source, seq);
    const auto report = [&](const char* sz_what, u64 block) {
        if (!s_write_reports.Take()) {
            return;
        }
        const u64 now = NowNs();
        std::fprintf(stderr,
                     "Free check: %s write %#llx +%llu (of %llu) value %#llx seq %llu into %s "
                     "%#llx of pool page %#llx (free %llu), tid %u, at %.3f s\n",
                     Name(source), (unsigned long long)begin, (unsigned long long)(end - begin),
                     (unsigned long long)cb_range, (unsigned long long)value,
                     (unsigned long long)seq, sz_what, (unsigned long long)block,
                     (unsigned long long)page, (unsigned long long)h.c_blocks, Tid(),
                     double(now) / 1e9);
        if (block < page + s_off_header) {
            PrintBlock(reinterpret_cast<const unsigned char*>(page), page, block);
            PrintRecent(block, block + s_cb_block, now, 16);
            PrintDecoded(block, block + s_cb_block, now, 8);
        } else {
            PrintRecent(page, page + PageSize, now, 16);
        }
    };
    if (end > page + s_off_header) {
        s_c_header_hits.fetch_add(1, std::memory_order_relaxed);
        report("the HEADER", page + s_off_header);
        return;
    }
    // The game changes the list all the time (labels are taken and released in bursts): a
    // walk that met it halfway is repeated.
    WalkResult r{};
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::memcpy(&h, reinterpret_cast<const void*>(page + s_off_header), sizeof(h));
        if (!Plausible(page, h)) {
            return;
        }
        r = WalkFree(page, h, [](u64 at) {
            u64 v;
            std::memcpy(&v, reinterpret_cast<const void*>(at), 8);
            return v;
        });
        if (r.is_ok) {
            break;
        }
    }
    if (!r.is_ok) {
        return; // still changing, or damaged: the watcher reports damage
    }
    for (u64 block = page + (begin - page) / s_cb_block * s_cb_block; block < end;
         block += s_cb_block) {
        if (IsFree(r.free, (block - page) / s_cb_block)) {
            s_c_free_hits.fetch_add(1, std::memory_order_relaxed);
            report("FREE block", block);
            return;
        }
    }
}
} // namespace

bool Enabled() {
    static const bool s_is_enabled = [] {
        const char* sz_env = std::getenv("PS_FREE_CHECK");
        if (!sz_env || sz_env[0] != '1') {
            return false;
        }
        const Game::Profile* p_game = Game::Active();
        const Game::SmallObjectPool pool = p_game ? p_game->small_object_pool : Game::SmallObjectPool{};
        if (pool.cb_block < 8 || pool.off_header + sizeof(Header) > PageSize ||
            pool.off_header / pool.cb_block > MaxPoolBlocks) {
            std::fprintf(stderr, "Free check: PS_FREE_CHECK=1 needs the game's small-object pool "
                                 "(its profile): off\n");
            return false;
        }
        s_off_header = pool.off_header;
        s_cb_block = pool.cb_block;
        s_max_blocks = s_off_header / s_cb_block;
        return true;
    }();
    return s_is_enabled;
}

namespace {
bool TrapEnabled();
void NoteTrapDecoded(u64 label);
void InstallStepTrap();
bool InstallFreeHook();
} // namespace

void NoteFenceDecoded(u64 label, u64 value, const void* p_packet, const void* p_buffer, u64 submit) {
    if (!Enabled()) {
        return;
    }
    s_decoded[s_decoded_head.fetch_add(1, std::memory_order_relaxed) % s_decoded.size()] =
        Decoded{label, value, reinterpret_cast<u64>(p_packet), reinterpret_cast<u64>(p_buffer), submit,
                NowNs()};
    if (TrapEnabled() && label != 0) {
        NoteTrapDecoded(label);
    }
}

void NoteSubmit(u64 submit, const void* p_buffer, u64 cb_range) {
    if (!Enabled()) {
        return;
    }
    s_submitted[s_submitted_head.fetch_add(1, std::memory_order_relaxed) % s_submitted.size()] =
        Submitted{submit, reinterpret_cast<u64>(p_buffer), cb_range, NowNs()};
}

u64 NextFenceSeq() {
    if (!Enabled()) {
        return 0;
    }
    const u64 seq = s_next_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    s_seq_start_ns[seq % SeqSlots].store(NowNs(), std::memory_order_relaxed);
    return seq;
}

void Check(u64 va_range, u64 cb_range, const void* p_data, Source source, u64 seq) {
    if (!Enabled() || cb_range == 0 || va_range == 0) {
        return;
    }
    StartWatcher();
    s_checked.fetch_add(1, std::memory_order_relaxed);
    u64 value = 0;
    if (p_data) {
        std::memcpy(&value, p_data, cb_range < 8 ? cb_range : 8);
    }
    if (seq != 0) {
        const u64 now = NowNs();
        const u64 start = s_seq_start_ns[seq % SeqSlots].load(std::memory_order_relaxed);
        const u64 latency = start && now > start ? now - start : 0;
        u64 max_seen = s_max_latency_ns.load(std::memory_order_relaxed);
        while (latency > max_seen && !s_max_latency_ns.compare_exchange_weak(max_seen, latency)) {
        }
        if (latency > 20000000) {
            s_c_late_20ms.fetch_add(1, std::memory_order_relaxed);
        }
        // Labels are written once the GPU has done the work: tens of ms after the recording thread
        // are normal; only stalls are reported.
        constexpr u64 ReportNs = 500000000;
        if (latency > 50000000) {
            s_c_late_50ms.fetch_add(1, std::memory_order_relaxed);
        }
        if (latency > ReportNs) {
            if (s_latency_reports.Take()) {
                std::fprintf(stderr,
                             "Free check: LATE fence: %s seq %llu at %#llx written %.1f ms after "
                             "the draw recording thread reached it, tid %u, at %.3f s\n",
                             Name(source), (unsigned long long)seq, (unsigned long long)va_range,
                             latency / 1e6, Tid(), now / 1e9);
            }
        }
        if (s_seq_written[seq % SeqSlots].exchange(seq, std::memory_order_relaxed) == seq) {
            s_c_duplicates.fetch_add(1, std::memory_order_relaxed);
            if (s_duplicate_reports.Take()) {
                std::fprintf(stderr,
                             "Free check: DUPLICATE fence: %s seq %llu at %#llx (value %#llx) "
                             "written again, tid %u, at %.3f s\n",
                             Name(source), (unsigned long long)seq, (unsigned long long)va_range,
                             (unsigned long long)value, Tid(), now / 1e9);
            }
        }
        u64 seen = s_max_written_seq.load(std::memory_order_relaxed);
        while (seen < seq && !s_max_written_seq.compare_exchange_weak(seen, seq)) {
        }
        if (seen > seq) {
            s_unordered.fetch_add(1, std::memory_order_relaxed);
            if (s_order_reports.Take()) {
                std::fprintf(stderr,
                             "Free check: OUT OF ORDER: %s seq %llu at %#llx (value %#llx) "
                             "written after seq %llu, tid %u, at %.3f s\n",
                             Name(source), (unsigned long long)seq,
                             (unsigned long long)va_range, (unsigned long long)value,
                             (unsigned long long)seen, Tid(), double(NowNs()) / 1e9);
            }
        }
    }
    const bool is_small = cb_range <= 64;
    const u64 end = va_range + cb_range;
    for (u64 page = va_range & ~(PageSize - 1); page < end; page += PageSize) {
        CheckPage(page, std::max(va_range, page), std::min(end, page + PageSize), value, cb_range,
                  source, seq, is_small);
    }
}

void DumpAtFault(u64 rax, u64 r14) {
    if (!Enabled()) {
        return;
    }
    std::fprintf(stderr,
                 "Free check: %llu writes checked, %llu into free blocks, %llu into pool "
                 "headers, %llu corrupted lists seen, %llu fences out of order, %u pool pages "
                 "watched\n",
                 (unsigned long long)s_checked.load(), (unsigned long long)s_c_free_hits.load(),
                 (unsigned long long)s_c_header_hits.load(), (unsigned long long)s_c_corruptions.load(),
                 (unsigned long long)s_unordered.load(), s_c_pages.load());
    std::fprintf(stderr,
                 "Free check: fences: max latency %.1f ms, %llu over 20 ms, %llu over 50 ms, %llu "
                 "written twice\n",
                 s_max_latency_ns.load() / 1e6, (unsigned long long)s_c_late_20ms.load(),
                 (unsigned long long)s_c_late_50ms.load(), (unsigned long long)s_c_duplicates.load());
    const u64 page = rax & ~(PageSize - 1);
    std::fprintf(stderr, "Free check: fault in pool page %#llx (r14 %#llx); writes into it:\n",
                 (unsigned long long)page, (unsigned long long)r14);
    PrintRecent(page, page + PageSize, NowNs(), 64);
}
namespace {
/// PS_LABEL_TRAP: fences decoded into pool pages (labels) until their write has landed.
struct Pending {
    u32 c_pending = 0;
    u64 decoded_ns = 0;
    u64 writing_ns = 0; ///< when the newest write started (0: not started)
};
struct LabelTrap {
    std::mutex mutex;
    std::unordered_map<u64, Pending> pending; ///< label qword -> fences not yet landed
    /// Mode 1: pages with pending labels -> how many; the ones made read-only; every page ever
    /// made read-only (a fault that raced with its release, OnStaleTrapFault).
    std::unordered_map<u64, u32> page_pending;
    std::unordered_set<u64> read_only;
    std::unordered_set<u64> ever_trapped;
    std::atomic<u32> reports{0}, slow_reports{0};
    /// Reports whose fence write had started: the guest may have seen it land a moment before
    /// NoteFenceWritten (or the write is stuck): their own budget.
    std::atomic<u32> started_reports{0};
    std::atomic<u64> faults{0}, hits{0};
};
LabelTrap& Trap() {
    static LabelTrap s_trap;
    return s_trap;
}
constexpr u64 ImageBase = 0x800000000ull, ImageEnd = 0x810000000ull;
/// The game's label release code (Game::LabelReleaseCode), set while the breakpoints are in.
u64 s_free_hook = 0, s_gc_hook = 0;

/// PS_LABEL_TRAP: 1 pool pages with pending labels read-only (every guest write into them is
/// single-stepped: slow, it hid the bug), 2 breakpoints on the guest allocator's release and on
/// the collector's "done" branch: a label block released, or taken as done, before its fence
/// landed is reported with the guest's view and call chain.
int TrapMode() {
    static const int s_mode = [] {
        const char* sz_env = std::getenv("PS_LABEL_TRAP");
        const int m = Enabled() && sz_env && (sz_env[0] == '1' || sz_env[0] == '2') ? sz_env[0] - '0' : 0;
        if (m != 0) {
            InstallStepTrap();
            if (m == 2 && !InstallFreeHook()) {
                return 0;
            }
            std::fprintf(stderr, "Free check: label trap on (PS_LABEL_TRAP=%d)\n", m);
        }
        return m;
    }();
    return s_mode;
}
bool TrapEnabled() {
    return TrapMode() != 0;
}
bool ReadQword(u64 va_guest, u64& value) {
    return ps_host_read_memory(&value, reinterpret_cast<const void*>(va_guest), 8) == 8;
}
u32 SignalTid() {
    return u32(ps_host_thread_id());
}
void NoteTrapDecoded(u64 label) {
    const u64 page = label & ~(PageSize - 1);
    if (label - page >= s_off_header) {
        return;
    }
    Header h;
    if (ps_host_read_memory(&h, reinterpret_cast<const void*>(page + s_off_header), sizeof(h)) != std::int64_t(sizeof(h)) ||
        !Plausible(page, h)) {
        return;
    }
    auto& t = Trap();
    std::scoped_lock lk{t.mutex};
    auto& p = t.pending[label & ~7ull];
    if (p.c_pending++ == 0) {
        ++t.page_pending[page];
    }
    p.decoded_ns = NowNs();
    p.writing_ns = 0;
    t.ever_trapped.insert(page);
    if (TrapMode() == 1 && t.read_only.insert(page).second) {
        mprotect(reinterpret_cast<void*>(page), PageSize, PROT_READ);
    }
}
} // namespace

void NoteFenceWriting(u64 label) {
    if (!TrapEnabled() || label == 0) {
        return;
    }
    auto& t = Trap();
    std::scoped_lock lk{t.mutex};
    if (const auto it = t.pending.find(label & ~7ull); it != t.pending.end()) {
        it->second.writing_ns = NowNs();
    }
}

void NoteFenceWritten(u64 label, u64 write_ns) {
    if (!TrapEnabled() || label == 0) {
        return;
    }
    auto& t = Trap();
    if (write_ns > 200000 && t.slow_reports.fetch_add(1, std::memory_order_relaxed) < 32) {
        std::fprintf(stderr, "Free check: SLOW fence write %#llx: %.3f ms, tid %u, at %.3f s\n",
                     (unsigned long long)label, write_ns / 1e6, Tid(), NowNs() / 1e9);
    }
    std::scoped_lock lk{t.mutex};
    const auto it = t.pending.find(label & ~7ull);
    if (it == t.pending.end() || --it->second.c_pending != 0) {
        return;
    }
    t.pending.erase(it);
    const u64 page = label & ~(PageSize - 1);
    if (const auto pp = t.page_pending.find(page); pp != t.page_pending.end() && --pp->second == 0) {
        t.page_pending.erase(pp);
        if (t.read_only.erase(page)) {
            mprotect(reinterpret_cast<void*>(page), PageSize, PROT_READ | PROT_WRITE);
        }
    }
}

namespace {
/// Threads single-stepping the write that faulted on a trapped page (TF set): the page is
/// writable for that one instruction, then read-only again (OnStepTrap). A table instead of
/// thread_local: this library is loaded with dlopen, its TLS is not signal-safe.
struct StepSlot {
    std::atomic<u32> tid{0};
    std::atomic<u64> page{0};
};
std::array<StepSlot, 256> s_step_slots;
constexpr u64 TrapFlag = 0x100;

void OnFreeHook(PsHostFaultContext* p_context);
void OnGcHook(PsHostFaultContext* p_context);

int OnStepTrap(PsHostFaultContext* p_context) {
    // PS_LABEL_TRAP=2: the breakpoints (RIP is past the int3).
    const u64 rip = ps_host_fault_register(p_context, PS_HOST_REG_RIP);
    if (s_free_hook && rip == s_free_hook + 1) {
        OnFreeHook(p_context);
        return 1;
    }
    if (s_gc_hook && rip == s_gc_hook + 1) {
        OnGcHook(p_context);
        return 1;
    }
    const u32 tid = SignalTid();
    for (auto& slot : s_step_slots) {
        if (slot.tid.load(std::memory_order_acquire) != tid) {
            continue;
        }
        const u64 page = slot.page.exchange(0, std::memory_order_acq_rel);
        slot.tid.store(0, std::memory_order_release);
        ps_host_fault_set_register(p_context, PS_HOST_REG_RFLAGS,
                                   ps_host_fault_register(p_context, PS_HOST_REG_RFLAGS) & ~TrapFlag);
        auto& t = Trap();
        std::scoped_lock lk{t.mutex};
        if (page && t.read_only.count(page)) {
            mprotect(reinterpret_cast<void*>(page), PageSize, PROT_READ);
        }
        return 1;
    }
    return 0; // not a step of ours
}

/// int3 over the first byte of an instruction the hook then performs itself.
bool PatchInt3(u64 va_range, std::initializer_list<unsigned char> expected) {
    unsigned char bytes[8]{};
    if (ps_host_read_memory(bytes, reinterpret_cast<const void*>(va_range), expected.size()) !=
            std::int64_t(expected.size()) ||
        std::memcmp(bytes, expected.begin(), expected.size()) != 0) {
        std::fprintf(stderr, "Free check: hook skipped (unexpected code at +%#llx)\n",
                     (unsigned long long)(va_range - ImageBase));
        return false;
    }
    void* p_page = reinterpret_cast<void*>(va_range & ~(PageSize - 1));
    if (mprotect(p_page, PageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<unsigned char*>(va_range), static_cast<unsigned char>(0xcc),
                     __ATOMIC_SEQ_CST);
    mprotect(p_page, PageSize, PROT_READ | PROT_EXEC);
    return true;
}

/// The release starts with `mov rax, rsi` (48 89 f0), the collector's branch with
/// `mov rcx, [rsp+0x28]` (48 8b 4c 24 28).
bool InstallFreeHook() {
    const Game::Profile* p_game = Game::Active();
    if (!p_game || !p_game->label_release.release) {
        std::fprintf(stderr, "Free check: PS_LABEL_TRAP=2 needs the game's label release code (its "
                             "profile): off\n");
        return false;
    }
    // Set before the int3 goes in, since another thread may reach it at once.
    s_free_hook = p_game->label_release.release;
    if (!PatchInt3(s_free_hook, {0x48, 0x89, 0xf0, 0x48, 0x85, 0xc0})) {
        s_free_hook = 0;
        return false;
    }
    s_gc_hook = p_game->label_release.collector_done;
    if (s_gc_hook && !PatchInt3(s_gc_hook, {0x48, 0x8b, 0x4c, 0x24, 0x28})) {
        s_gc_hook = 0;
    }
    return true;
}

void InstallStepTrap() {
    if (const int error = ps_host_fault_install(PS_HOST_FAULT_BREAKPOINT, OnStepTrap, PS_HOST_FAULT_NESTED)) {
        std::fprintf(stderr, "Free check: the step trap could not be installed (%s)\n", std::strerror(error));
    }
}

bool ArmStep(PsHostFaultContext* p_context, u64 page) {
    const u32 tid = SignalTid();
    for (auto& slot : s_step_slots) {
        u32 expected = 0;
        if (slot.tid.load(std::memory_order_acquire) == tid ||
            slot.tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
            slot.page.store(page, std::memory_order_release);
            ps_host_fault_set_register(p_context, PS_HOST_REG_RFLAGS,
                                       ps_host_fault_register(p_context, PS_HOST_REG_RFLAGS) | TrapFlag);
            return true;
        }
    }
    return false; // table full: the page stays writable until the next decode
}

bool IsGuest(u64 v) {
    return v >= ImageBase && v < ImageEnd;
}

/// The newest decodes and writes of fences into the label qword at `label`, for a report.
int AppendHistory(char* sz_line, int cb_used, int cb_line, u64 label, const char* sz_name) {
    const u64 now = NowNs(), want = label & ~7ull;
    cb_used += std::snprintf(sz_line + cb_used, cb_line - cb_used, "\n  %s %#llx decoded:", sz_name, (unsigned long long)want);
    int c_shown = 0;
    const u64 nd = s_decoded_head.load(std::memory_order_relaxed);
    for (u64 i = nd; i-- > (nd > s_decoded.size() ? nd - s_decoded.size() : 0) && c_shown < 4 &&
                     cb_used < cb_line - 80;) {
        const Decoded& d = s_decoded[i % s_decoded.size()];
        if ((d.label & ~7ull) == want) {
            cb_used += std::snprintf(sz_line + cb_used, cb_line - cb_used, " [%.3f ms ago, submission %llu, value %#llx]",
                               (now - d.ns) / 1e6, (unsigned long long)d.submit,
                               (unsigned long long)d.value);
            ++c_shown;
        }
    }
    cb_used += std::snprintf(sz_line + cb_used, cb_line - cb_used, "; written:");
    c_shown = 0;
    const u64 c_recorded = s_ring_head.load(std::memory_order_relaxed);
    for (u64 i = c_recorded; i-- > (c_recorded > s_ring.size() ? c_recorded - s_ring.size() : 0) && c_shown < 4 && cb_used < cb_line - 80;) {
        const Entry& e = s_ring[i % s_ring.size()];
        if ((e.va_range & ~7ull) == want && e.source == u32(Eop)) {
            cb_used += std::snprintf(sz_line + cb_used, cb_line - cb_used, " [%.3f ms ago, seq %llu, value %#llx]",
                               (now - e.ns) / 1e6, (unsigned long long)e.seq,
                               (unsigned long long)e.value);
            ++c_shown;
        }
    }
    return cb_used;
}

/// The guest call chain at a breakpoint or fault: [rsp] (a leaf's return address) and the
/// rbp frames, as guest offsets.
int AppendStack(char* sz_line, int cb_used, int cb_line, const PsHostFaultContext* p_context, bool is_leaf) {
    if (is_leaf) {
        u64 ret = 0;
        if (ReadQword(ps_host_fault_register(p_context, PS_HOST_REG_RSP), ret)) {
            cb_used += std::snprintf(sz_line + cb_used, cb_line - cb_used, "\n  caller: %s%#llx", IsGuest(ret) ? "+" : "",
                               (unsigned long long)(IsGuest(ret) ? ret - ImageBase : ret));
        }
    }
    cb_used += std::snprintf(sz_line + cb_used, cb_line - cb_used, "\n  frames:");
    u64 rbp = ps_host_fault_register(p_context, PS_HOST_REG_RBP);
    for (int i = 0; i < 16 && rbp && cb_used < cb_line - 24; ++i) {
        u64 saved = 0, ret = 0;
        if (!ReadQword(rbp, saved) || !ReadQword(rbp + 8, ret)) {
            break;
        }
        cb_used += std::snprintf(sz_line + cb_used, cb_line - cb_used, " %s%#llx", IsGuest(ret) ? "+" : "",
                           (unsigned long long)(IsGuest(ret) ? ret - ImageBase : ret));
        if (saved <= rbp) {
            break;
        }
        rbp = saved;
    }
    return cb_used;
}

/// "not written" / "write started X ms ago": where the pending fence is.
int AppendState(char* sz_line, int cb_used, int cb_line, const Pending& p) {
    const u64 now = NowNs();
    cb_used += std::snprintf(sz_line + cb_used, cb_line - cb_used, "%u fence(s) pending, decoded %.3f ms ago, ", p.c_pending,
                       (now - p.decoded_ns) / 1e6);
    if (p.writing_ns) {
        cb_used += std::snprintf(sz_line + cb_used, cb_line - cb_used, "write started %.3f ms ago",
                           (now - p.writing_ns) / 1e6);
    } else {
        cb_used += std::snprintf(sz_line + cb_used, cb_line - cb_used, "write not started");
    }
    return cb_used;
}

bool FindPending(u64 label, Pending& out) {
    auto& t = Trap();
    std::scoped_lock lk{t.mutex};
    const auto it = t.pending.find(label & ~7ull);
    if (it == t.pending.end()) {
        return false;
    }
    out = it->second;
    return true;
}

bool TakeReport(const Pending& p) {
    auto& t = Trap();
    t.hits.fetch_add(1, std::memory_order_relaxed);
    return p.writing_ns ? t.started_reports.fetch_add(1, std::memory_order_relaxed) < 16
                        : t.reports.fetch_add(1, std::memory_order_relaxed) < 32;
}

void Emit(char* sz_line, int cb_used, int cb_line) {
    cb_used += std::snprintf(sz_line + cb_used, cb_line - cb_used, "\n");
    const ssize_t written = write(2, sz_line, std::min<std::size_t>(cb_used, cb_line - 1));
    (void)written;
}

/// The guest allocator's release (int3): performs the replaced `mov rax, rsi`; a label block
/// whose fence has not landed is reported with the caller.
void OnFreeHook(PsHostFaultContext* p_context) {
    const u64 block = ps_host_fault_register(p_context, PS_HOST_REG_RSI);
    ps_host_fault_set_register(p_context, PS_HOST_REG_RAX, block);
    ps_host_fault_set_register(p_context, PS_HOST_REG_RIP, s_free_hook + 3);
    Pending p;
    if (!FindPending(block, p)) {
        return;
    }
    if (!TakeReport(p)) {
        return;
    }
    char sz_line[3072];
    int cb_used = std::snprintf(sz_line, sizeof(sz_line),
                          "Free check: RELEASED label block %#llx, tid %u, at %.3f s: ",
                          (unsigned long long)block, SignalTid(), NowNs() / 1e9);
    cb_used = AppendState(sz_line, cb_used, sizeof(sz_line), p);
    cb_used = AppendStack(sz_line, cb_used, sizeof(sz_line), p_context, true);
    u64 value = 0;
    ReadQword(block, value);
    cb_used += std::snprintf(sz_line + cb_used, sizeof(sz_line) - cb_used, "\n  value %#llx", (unsigned long long)value);
    cb_used = AppendHistory(sz_line, cb_used, sizeof(sz_line), block, "this label");
    Emit(sz_line, cb_used, sizeof(sz_line));
}

/// The collector's "done" branch (int3): performs the replaced `mov rcx, [rsp+0x28]`. r14 is the
/// block taken as done, its first qword the label (or bit 0: a pointer to the batch's label),
/// eax what the collector read there (4). Reported when that label's fence has not landed.
void OnGcHook(PsHostFaultContext* p_context) {
    ps_host_fault_set_register(p_context, PS_HOST_REG_RCX,
                               *reinterpret_cast<const u64*>(ps_host_fault_register(p_context, PS_HOST_REG_RSP) + 0x28));
    ps_host_fault_set_register(p_context, PS_HOST_REG_RIP, s_gc_hook + 5);
    const u64 node = ps_host_fault_register(p_context, PS_HOST_REG_R14);
    u64 raw = 0;
    ReadQword(node, raw);
    const u64 label = (raw & 1) ? raw & ~1ull : node;
    Pending p, own;
    const bool is_label_pending = FindPending(label, p);
    const bool is_node_pending = label != node && FindPending(node, own);
    if (!is_label_pending && !is_node_pending) {
        return;
    }
    if (!TakeReport(is_label_pending ? p : own)) {
        return;
    }
    char sz_line[3072];
    int cb_used = std::snprintf(sz_line, sizeof(sz_line),
                          "Free check: TAKEN AS DONE block %#llx, tid %u, at %.3f s: first qword "
                          "%#llx, read %#x",
                          (unsigned long long)node, SignalTid(), NowNs() / 1e9,
                          (unsigned long long)raw, unsigned(ps_host_fault_register(p_context, PS_HOST_REG_RAX)));
    if (is_label_pending) {
        cb_used += std::snprintf(sz_line + cb_used, sizeof(sz_line) - cb_used, "\n  label %#llx: ",
                           (unsigned long long)label);
        cb_used = AppendState(sz_line, cb_used, sizeof(sz_line), p);
    }
    if (is_node_pending) {
        cb_used += std::snprintf(sz_line + cb_used, sizeof(sz_line) - cb_used, "\n  block's own label: ");
        cb_used = AppendState(sz_line, cb_used, sizeof(sz_line), own);
    }
    cb_used = AppendStack(sz_line, cb_used, sizeof(sz_line), p_context, false);
    cb_used = AppendHistory(sz_line, cb_used, sizeof(sz_line), label, "label");
    if (label != node) {
        cb_used = AppendHistory(sz_line, cb_used, sizeof(sz_line), node, "block");
    }
    Emit(sz_line, cb_used, sizeof(sz_line));
}
} // namespace

bool OnTrapFault(PsHostFaultContext* p_context, u64 va_guest) {
    if (TrapMode() != 1) {
        return false;
    }
    const u64 page = va_guest & ~(PageSize - 1);
    auto& t = Trap();
    std::unique_lock lk{t.mutex};
    if (!t.read_only.count(page)) {
        return false;
    }
    t.faults.fetch_add(1, std::memory_order_relaxed);
    const auto it = t.pending.find(va_guest & ~7ull);
    const bool is_hit = it != t.pending.end();
    const Pending p = is_hit ? it->second : Pending{};
    // Writable for the faulting instruction only (single step), read-only again after it.
    mprotect(reinterpret_cast<void*>(page), PageSize, PROT_READ | PROT_WRITE);
    if (!ArmStep(p_context, page)) {
        t.read_only.erase(page);
    }
    lk.unlock();
    const u64 rip = ps_host_fault_register(p_context, PS_HOST_REG_RIP);
    if (!is_hit || !IsGuest(rip)) {
        return true;
    }
    t.hits.fetch_add(1, std::memory_order_relaxed);
    if (t.reports.fetch_add(1, std::memory_order_relaxed) >= 24) {
        return true;
    }
    char sz_line[3072];
    int cb_used = std::snprintf(sz_line, sizeof(sz_line),
                          "Free check: TRAP guest write %#llx into a label block, tid %u, at %.3f "
                          "s: rip +%#llx rdi %#llx rsi %#llx rax %#llx rcx %#llx\n  ",
                          (unsigned long long)va_guest, SignalTid(), NowNs() / 1e9,
                          (unsigned long long)(rip - ImageBase),
                          (unsigned long long)ps_host_fault_register(p_context, PS_HOST_REG_RDI),
                          (unsigned long long)ps_host_fault_register(p_context, PS_HOST_REG_RSI),
                          (unsigned long long)ps_host_fault_register(p_context, PS_HOST_REG_RAX),
                          (unsigned long long)ps_host_fault_register(p_context, PS_HOST_REG_RCX));
    cb_used = AppendState(sz_line, cb_used, sizeof(sz_line), p);
    cb_used = AppendStack(sz_line, cb_used, sizeof(sz_line), p_context, true);
    u64 old = 0;
    ReadQword(va_guest & ~7ull, old);
    cb_used += std::snprintf(sz_line + cb_used, sizeof(sz_line) - cb_used, "\n  old value %#llx", (unsigned long long)old);
    cb_used = AppendHistory(sz_line, cb_used, sizeof(sz_line), va_guest, "this label");
    Emit(sz_line, cb_used, sizeof(sz_line));
    return true;
}

bool OnStaleTrapFault(u64 va_guest) {
    if (TrapMode() != 1) {
        return false;
    }
    const u64 page = va_guest & ~(PageSize - 1);
    auto& t = Trap();
    std::scoped_lock lk{t.mutex};
    if (!t.ever_trapped.count(page)) {
        return false;
    }
    // The write faulted while the page was read-only and its fences were written before the
    // handler got the lock: the page is writable again (or made so) and the write is retried.
    // Trapped again meanwhile: the retry faults into OnTrapFault.
    if (!t.read_only.count(page)) {
        mprotect(reinterpret_cast<void*>(page), PageSize, PROT_READ | PROT_WRITE);
    }
    return true;
}
} // namespace PsFreeCheck
