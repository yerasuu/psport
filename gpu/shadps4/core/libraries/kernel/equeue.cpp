// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <thread>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"

// psport: the runtime's descriptor table (src/kernel/runtime_file.c).
extern "C" uint64_t runtime_file_serial(int h_fd);
extern "C" int runtime_file_ready(int h_fd, uint64_t serial, int is_write, int64_t* p_out_data, int* p_out_is_eof);
extern "C" int runtime_file_changes(int h_fd, uint64_t serial, int64_t p_inout_state[4]);

namespace Libraries::Kernel {

// psport: shadPS4 124328bcd
static bool IsIoFilter(s16 filter) {
    return filter == OrbisKernelEvent::Filter::Read || filter == OrbisKernelEvent::Filter::Write ||
           filter == OrbisKernelEvent::Filter::Vnode;
}

// psport: which open of the descriptor table the ident is, 0 when none.
static u64 DescriptorSerial(u64 ident) {
    return ident <= 0x7fffffff ? runtime_file_serial(static_cast<int>(ident)) : 0;
}

extern boost::asio::io_context io_context;
extern void KernelSignalRequest();

static std::unordered_map<s32, EqueueInternal*> kqueues;
static constexpr auto HrTimerSpinlockThresholdNs = 1200000u;

EqueueInternal* GetEqueue(OrbisKernelEqueue eq) {
    if (!kqueues.contains(eq)) {
        return nullptr;
    }
    return kqueues[eq];
}

static void HrTimerCallback(OrbisKernelEqueue eq, const OrbisKernelEvent& kevent) {
    if (kqueues.contains(eq)) {
        kqueues[eq]->TriggerEvent(kevent.ident, OrbisKernelEvent::Filter::HrTimer, kevent.udata);
    }
}

static void TimerCallback(OrbisKernelEqueue eq, const OrbisKernelEvent& kevent) {
    if (kqueues.contains(eq) && kqueues[eq]->EventExists(kevent.ident, kevent.filter)) {
        kqueues[eq]->TriggerEvent(kevent.ident, OrbisKernelEvent::Filter::Timer, kevent.udata);
        if (!(kevent.flags & OrbisKernelEvent::Flags::OneShot)) {
            // Reschedule the event for its next period.
            kqueues[eq]->ScheduleEvent(kevent.ident, kevent.filter, TimerCallback);
        }
    }
}

// Events are uniquely identified by id and filter.
bool EqueueInternal::AddEvent(EqueueEvent& event) {
    // Save id and filter before event is moved into m_events.
    const u64 id = event.event.ident;
    const auto filter = event.event.filter;

    {
        std::scoped_lock lock{m_mutex};

        // Calculate timer interval
        event.time_added = std::chrono::steady_clock::now();
        if (event.event.filter == OrbisKernelEvent::Filter::Timer) {
            // Set timer interval, this is stored in milliseconds for timers.
            event.timer_interval = std::chrono::milliseconds(event.event.data);
        } else if (event.event.filter == OrbisKernelEvent::Filter::HrTimer) {
            // Retrieve inputted time, this is stored in the bintime format.
            OrbisKernelBintime* time = reinterpret_cast<OrbisKernelBintime*>(event.event.data);

            // Convert the bintime format to a timespec.
            OrbisKernelTimespec ts;
            ts.tv_sec = time->sec;
            ts.tv_nsec = (1000000000 * (time->frac >> 32)) >> 32;

            // Then use the timespec to set the timer interval.
            event.timer_interval = std::chrono::nanoseconds(ts.tv_nsec + ts.tv_sec * 1000000000);
        }

        // psport: a read or write filter's data is its low-water mark with NOTE_LOWAT (1), a
        // vnode filter's fflags the changes it reports (shadPS4 124328bcd, FreeBSD's kevent).
        const bool is_io = IsIoFilter(filter);
        if (is_io) {
            event.io_serial = DescriptorSerial(id);
            event.io_low_water = filter != OrbisKernelEvent::Filter::Vnode && (event.event.fflags & 1)
                                     ? event.event.data
                                     : 0;
            event.io_notes = filter == OrbisKernelEvent::Filter::Vnode ? event.event.fflags : 0;
        }

        // First, check if there's already an event with the same id and filter.
        const auto& find_it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
            return ev.event.ident == id && ev.event.filter == filter;
        });
        // If there is a duplicate event, we need to update that instead.
        if (find_it != m_events.cend()) {
            // Specifically, update user data and timer_interval.
            // Trigger status and event data should remain intact.
            auto& old_event = *find_it;
            old_event.timer_interval = event.timer_interval;
            old_event.event.udata = event.event.udata;
            if (is_io) { // psport
                if (old_event.io_serial != event.io_serial) { // another open behind the same number
                    old_event.io_serial = event.io_serial;
                    old_event.vnode_state[0] = -1;
                    old_event.Clear();
                }
                old_event.io_low_water = event.io_low_water;
                old_event.io_notes = event.io_notes;
                if (CheckIoEvent(old_event) && old_event.IsTriggered()) {
                    m_cond.notify_one();
                }
            }
            return true;
        }

        // Clear input data from event.
        event.event.data = 0;

        // Remove add flag from event
        event.event.flags &= ~OrbisKernelEvent::Flags::Add;

        // Clear flag is appended to most event types internally.
        // psport: not to read and write filters, which hold while their descriptor is ready
        // (FreeBSD sets it for vnode filters, whose changes add up until they are read).
        if (event.event.filter != OrbisKernelEvent::Filter::User &&
            event.event.filter != OrbisKernelEvent::Filter::Read &&
            event.event.filter != OrbisKernelEvent::Filter::Write) {
            event.event.flags |= OrbisKernelEvent::Flags::Clear;
        }

        // psport: an I/O filter is checked at once (shadPS4 124328bcd).
        if (is_io) {
            CheckIoEvent(event);
        }
        const bool is_triggered = event.IsTriggered();

        const auto& it = std::ranges::find(m_events, event);
        if (it != m_events.cend()) {
            *it = std::move(event);
        } else {
            m_events.emplace_back(std::move(event));
        }
        if (is_triggered) {
            m_cond.notify_one();
        }
    }

    // Schedule callbacks for timer events
    if (filter == OrbisKernelEvent::Filter::Timer) {
        return this->ScheduleEvent(id, OrbisKernelEvent::Filter::Timer, TimerCallback);
    } else if (filter == OrbisKernelEvent::Filter::HrTimer) {
        return this->ScheduleEvent(id, OrbisKernelEvent::Filter::HrTimer, HrTimerCallback);
    }

    return true;
}

bool EqueueInternal::ScheduleEvent(u64 id, s16 filter,
                                   void (*callback)(OrbisKernelEqueue, const OrbisKernelEvent&)) {
    // psport: only timers schedule (the console's timers are kernel filters, nothing in libkernel).
    if (filter != OrbisKernelEvent::Filter::Timer && filter != OrbisKernelEvent::Filter::HrTimer) {
        LOG_ERROR_COUNTED(Kernel_Event, "ScheduleEvent: filter {} is not a timer, not scheduled", filter);
        return false;
    }
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });
    if (it == m_events.cend()) {
        return false;
    }

    const auto& event = *it;

    if (!it->timer) {
        it->timer = std::make_unique<boost::asio::steady_timer>(io_context, event.timer_interval);
    } else {
        // If the timer already exists we are scheduling a reoccurrence after the next period.
        // Set the expiration time to the previous occurrence plus the period.
        it->timer->expires_at(it->timer->expiry() + event.timer_interval);
    }

    it->timer->async_wait(
        [this, event_data = event.event, callback](const boost::system::error_code& ec) {
            if (ec) {
                if (ec != boost::system::errc::operation_canceled) {
                    LOG_ERROR(Kernel_Event, "Timer callback error: {}", ec.message());
                } else {
                    // Timer was cancelled (removed) before it triggered
                    LOG_DEBUG(Kernel_Event, "Timer cancelled");
                }
                return;
            }
            callback(this->m_handle, event_data);
        });
    KernelSignalRequest();

    return true;
}

bool EqueueInternal::RemoveEvent(u64 id, s16 filter) {
    bool has_found = false;
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });
    if (it != m_events.cend()) {
        m_events.erase(it);
        has_found = true;
    }
    return has_found;
}

int EqueueInternal::WaitForEvents(OrbisKernelEvent* ev, int num, const OrbisKernelUseconds* timo) {
    if (timo != nullptr && *timo == 0) {
        // Effectively acts as a poll; only events that have already
        // arrived at the time of this function call can be received
        std::scoped_lock lock{m_mutex}; // psport: shadPS4 124328bcd
        return GetTriggeredEvents(ev, num);
    }
    const auto micros = timo ? *timo : 0u;

    if (HasSmallTimer()) {
        // If a small timer is set, just wait for it to expire.
        return WaitForSmallTimer(ev, num, micros);
    }

    int count = 0;

    const auto predicate = [&] {
        count = GetTriggeredEvents(ev, num);
        return count > 0;
    };

    std::unique_lock lock{m_mutex};
    // psport: nothing triggers an I/O filter, so the queue looks at its descriptors every 1-10 ms
    // while it has one (shadPS4 124328bcd).
    if (HasPendingIoEvents()) {
        const auto deadline = micros == 0 ? std::chrono::steady_clock::time_point::max()
                                          : std::chrono::steady_clock::now() + std::chrono::microseconds(micros);
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            const auto remaining = deadline == std::chrono::steady_clock::time_point::max()
                                       ? std::chrono::milliseconds(10)
                                       : std::chrono::duration_cast<std::chrono::milliseconds>(
                                             deadline - std::chrono::steady_clock::now());
            m_cond.wait_for(lock, std::clamp(remaining, std::chrono::milliseconds(1), std::chrono::milliseconds(10)));
        }
    } else if (micros == 0) {
        // Wait indefinitely for events
        m_cond.wait(lock, predicate);
    } else {
        // Wait up until the timeout value
        m_cond.wait_for(lock, std::chrono::microseconds(micros), predicate);
    }

    return count;
}

bool EqueueInternal::TriggerEvent(u64 ident, s16 filter, void* trigger_data) {
    bool has_found = false;
    {
        std::scoped_lock lock{m_mutex};
        for (auto& event : m_events) {
            if (event.event.ident == ident && event.event.filter == filter) {
                if (filter == OrbisKernelEvent::Filter::VideoOut) {
                    event.TriggerDisplay(trigger_data);
                } else if (filter == OrbisKernelEvent::Filter::User) {
                    event.TriggerUser(trigger_data);
                } else if (filter == OrbisKernelEvent::Filter::Timer ||
                           filter == OrbisKernelEvent::Filter::HrTimer) {
                    event.TriggerTimer();
                } else {
                    event.Trigger(trigger_data);
                }
                has_found = true;
            }
        }
    }
    m_cond.notify_one();
    return has_found;
}

int EqueueInternal::GetTriggeredEvents(OrbisKernelEvent* ev, int num) {
    int count = 0;
    for (auto it = m_events.begin(); it != m_events.end();) {
        if (IsIoFilter(it->event.filter) && !CheckIoEvent(*it)) { // psport
            it = m_events.erase(it);
            continue;
        }
        if (it->IsTriggered()) {
            ev[count++] = it->event;
            if (it->event.flags & OrbisKernelEvent::Flags::Clear) {
                it->Clear();
            }
            if (it->event.flags & OrbisKernelEvent::Flags::OneShot) {
                it = m_events.erase(it);
            } else {
                ++it;
            }

            if (count == num) {
                break;
            }
        } else {
            ++it;
        }
    }

    return count;
}

// psport: shadPS4 124328bcd, as FreeBSD's filters report: data is the bytes to read or the room to
// write, EOF set once the other end is gone; a vnode filter collects the changes it asked for.
bool EqueueInternal::CheckIoEvent(EqueueEvent& ev) {
    const int h_fd = static_cast<int>(ev.event.ident);
    if (ev.event.filter == OrbisKernelEvent::Filter::Vnode) {
        const int notes = runtime_file_changes(h_fd, ev.io_serial, ev.vnode_state);
        if (notes < 0) {
            return false;
        }
        const u32 seen = (ev.IsTriggered() ? ev.event.fflags : 0u) | (static_cast<u32>(notes) & ev.io_notes);
        if (seen) {
            ev.TriggerIo(0, seen);
        }
        return true;
    }
    s64 data = 0;
    int is_eof = 0;
    const int ready =
        runtime_file_ready(h_fd, ev.io_serial, ev.event.filter == OrbisKernelEvent::Filter::Write, &data, &is_eof);
    if (ready < 0) {
        return false;
    }
    if (ready && (is_eof || static_cast<u64>(data) >= ev.io_low_water)) {
        ev.TriggerIo(static_cast<u64>(data), 0, is_eof ? OrbisKernelEvent::Flags::Eof : 0);
    } else {
        ev.Clear();
    }
    return true;
}

bool EqueueInternal::HasPendingIoEvents() {
    return std::ranges::any_of(m_events, [](const auto& e) { return IsIoFilter(e.event.filter); });
}

bool EqueueInternal::AddSmallTimer(EqueueEvent& ev) {
    // Retrieve inputted time, this is stored in the bintime format
    OrbisKernelBintime* time = reinterpret_cast<OrbisKernelBintime*>(ev.event.data);
    OrbisKernelTimespec ts;
    ts.tv_sec = time->sec;
    ts.tv_nsec = ((1000000000 * (time->frac >> 32)) >> 32);

    // Create the small timer
    SmallTimer st;
    st.event = ev.event;
    st.added = std::chrono::steady_clock::now();
    st.interval = std::chrono::nanoseconds(ts.tv_nsec + ts.tv_sec * 1000000000);
    {
        std::scoped_lock lock{m_mutex};
        m_small_timers[st.event.ident] = std::move(st);
    }
    return true;
}

int EqueueInternal::WaitForSmallTimer(OrbisKernelEvent* ev, int num, u32 micros) {
    if (num < 1) {
        return 0;
    }

    auto curr_clock = std::chrono::steady_clock::now();
    const auto wait_end_us = (micros == 0) ? std::chrono::steady_clock::time_point::max()
                                           : curr_clock + std::chrono::microseconds{micros};
    int count = 0;
    do {
        curr_clock = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock{m_mutex};
            for (auto it = m_small_timers.begin(); it != m_small_timers.end() && count < num;) {
                const SmallTimer& st = it->second;

                if (curr_clock - st.added >= st.interval) {
                    ev[count++] = st.event;
                    it = m_small_timers.erase(it);
                } else {
                    ++it;
                }
            }

            if (count > 0)
                return count;
        }
        std::this_thread::yield();
    } while (curr_clock < wait_end_us);

    return 0;
}

bool EqueueInternal::EventExists(u64 id, s16 filter) {
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });

    return it != m_events.cend();
}

static s32 NextEqueueHandle() { // psport
    static std::atomic<s32> next{0x40000};
    return next++;
}

s32 PS4_SYSV_ABI posix_kqueue() {
    // Reserve a file handle for the kqueue
    // psport: equeue handles come from their own range, not the file table.
    s32 kqueue_handle = NextEqueueHandle();

    // Plenty of equeue logic uses names to identify queues.
    // Create a unique name for the queue we create.
    char name[32];
    memset(name, 0, sizeof(name));
    snprintf(name, sizeof(name), "kqueue%i", kqueue_handle);

    // Create the queue
    kqueues[kqueue_handle] = new EqueueInternal(kqueue_handle, name);
    LOG_INFO(Kernel_Event, "kqueue created with name {}", name);

    // Return handle.
    return kqueue_handle;
}

// Helper method to detect supported filters.
// We don't want to allow adding events we don't handle properly.
bool SupportedEqueueFilter(OrbisKernelEvent::Filter filter) {
    return filter == OrbisKernelEvent::Filter::GraphicsCore ||
           filter == OrbisKernelEvent::Filter::HrTimer ||
           filter == OrbisKernelEvent::Filter::Timer || filter == OrbisKernelEvent::Filter::User ||
           filter == OrbisKernelEvent::Filter::VideoOut || IsIoFilter(filter); // psport: shadPS4 124328bcd
}

// psport: nchanges and nevents are kevent's 32-bit ints; the upper half of their registers is
// whatever the game left there.
s32 PS4_SYSV_ABI posix_kevent(s32 handle, OrbisKernelEvent* changelist, s32 nchanges,
                              OrbisKernelEvent* eventlist, s32 nevents,
                              OrbisKernelTimespec* timeout) {
    LOG_INFO(Kernel_Event, "called, eq = {}, nchanges = {}, nevents = {}", handle, nchanges,
             nevents);

    // Get the equeue
    if (!kqueues.contains(handle)) {
        *__Error() = POSIX_EBADF;
        return ORBIS_FAIL;
    }
    auto equeue = kqueues[handle];

    // First step is to apply all changes in changelist.
    for (s32 i = 0; i < nchanges; i++) {
        auto event = changelist[i];
        if (!SupportedEqueueFilter(event.filter)) {
            LOG_ERROR(Kernel_Event, "Unsupported event filter {}",
                      magic_enum::enum_name(event.filter));
            continue;
        }

        // Check the event flags to determine the appropriate action
        if (event.flags & OrbisKernelEvent::Flags::Add) {
            if (IsIoFilter(event.filter) && !DescriptorSerial(event.ident)) { // psport
                *__Error() = POSIX_EBADF;
                return ORBIS_FAIL;
            }
            // The caller is requesting to add an event.
            EqueueEvent internal_event{};
            internal_event.event = event;
            if (!equeue->AddEvent(internal_event)) {
                // Failed to add event, return error.
                *__Error() = POSIX_ENOMEM;
                return ORBIS_FAIL;
            }
        }

        if (event.flags & OrbisKernelEvent::Flags::Delete) {
            // The caller is requesting to remove an event.
            if (!equeue->RemoveEvent(event.ident, event.filter)) {
                // Failed to remove event, return error.
                *__Error() = POSIX_ENOENT;
                return ORBIS_FAIL;
            }
        }

        if (event.filter == OrbisKernelEvent::Filter::User && (event.fflags & 0x1000000)) { // psport: shadPS4 124328bcd
            // For user events, this fflags value indicates we need to trigger the event.
            if (!equeue->TriggerEvent(event.ident, OrbisKernelEvent::Filter::User, event.udata)) {
                *__Error() = POSIX_ENOENT;
                return ORBIS_FAIL;
            }
        } else if (event.fflags != 0 && !IsIoFilter(event.filter)) { // psport: shadPS4 124328bcd
            // The title is using filter-specific flags. Right now, these are unhandled.
            LOG_ERROR(Kernel_Event, "Unhandled fflags {:#x} for event filter {}", event.fflags,
                      magic_enum::enum_name(event.filter));
            continue;
        }
    }

    // psport: libkernel passes nevents to the console's kernel unchecked; EINVAL for a negative
    // count is FreeBSD's answer, inferred since the console's kernel was not read.
    if (nevents < 0) {
        LOG_ERROR_COUNTED(Kernel_Event, "kevent: negative nevents {}, EINVAL", nevents);
        *__Error() = POSIX_EINVAL;
        return ORBIS_FAIL;
    }

    // Now we need to wait on the event list.
    s32 count = 0;
    if (nevents > 0) {
        if (timeout != nullptr) {
            OrbisKernelUseconds micros = (timeout->tv_sec * 1000000) + (timeout->tv_nsec / 1000);
            count = equeue->WaitForEvents(eventlist, nevents, &micros);
        } else {
            count = equeue->WaitForEvents(eventlist, nevents, nullptr);
        }
    }
    return count;
}

int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue* eq, const char* name) {
    if (eq == nullptr) {
        LOG_ERROR(Kernel_Event, "Event queue is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (name == nullptr) {
        LOG_ERROR(Kernel_Event, "Event queue name is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // Maximum is 32 including null terminator
    static constexpr u64 MaxEventQueueNameSize = 32;
    if (std::strlen(name) > MaxEventQueueNameSize) {
        LOG_ERROR(Kernel_Event, "Event queue name exceeds 32 bytes!");
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }

    LOG_INFO(Kernel_Event, "name = {}", name);

    // Reserve a file handle for the kqueue
    // psport: equeue handles come from their own range, not the file table.
    OrbisKernelEqueue kqueue_handle = NextEqueueHandle();

    // Create the equeue
    kqueues[kqueue_handle] = new EqueueInternal(kqueue_handle, name);
    *eq = kqueue_handle;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue eq) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    delete kqueues[eq];
    kqueues.erase(eq);
    return ORBIS_OK;
}

namespace {
// psport (frame stats): time the game spends waiting in each event queue, and the events it got.
struct EqueueWaitStat {
    std::string name;
    u64 waits = 0, ns = 0, events = 0;
};
std::mutex equeue_wait_mutex;
std::vector<EqueueWaitStat> equeue_wait_stats;
} // namespace

void ReportEqueueWaits(double frames) {
    std::scoped_lock lk{equeue_wait_mutex};
    if (equeue_wait_stats.empty() || frames <= 0) {
        return;
    }
    std::sort(equeue_wait_stats.begin(), equeue_wait_stats.end(),
              [](const auto& a, const auto& b) { return a.ns > b.ns; });
    std::printf("Guest event waits per frame:");
    for (std::size_t i = 0; i < std::min<std::size_t>(equeue_wait_stats.size(), 6); ++i) {
        const auto& s = equeue_wait_stats[i];
        std::printf("%s %s %.1f waits %.2f ms %.1f events", i ? ";" : "", s.name.c_str(),
                    s.waits / frames, s.ns / (frames * 1e6), s.events / frames);
    }
    std::printf("\n");
    equeue_wait_stats.clear();
}

int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue eq, OrbisKernelEvent* ev, int num, int* out,
                                     OrbisKernelUseconds* timo) {
    HLE_TRACE;
    // psport: libkernel checks num before the queue and ev (sceKernelWaitEqueue, 0x1acf0).
    if (num < 1) {
        *out = 0;
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    auto& equeue = kqueues[eq];

    TRACE_HINT(equeue->GetName());
    LOG_TRACE(Kernel_Event, "equeue = {} num = {}", equeue->GetName(), num);

    if (ev == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    const auto wait_start = std::chrono::steady_clock::now();
    *out = equeue->WaitForEvents(ev, num, timo);
    {
        const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - wait_start)
                                .count());
        std::scoped_lock lk{equeue_wait_mutex};
        auto it = std::find_if(equeue_wait_stats.begin(), equeue_wait_stats.end(),
                               [&](const auto& s) { return s.name == equeue->GetName(); });
        if (it == equeue_wait_stats.end()) {
            it = equeue_wait_stats.insert(equeue_wait_stats.end(),
                                          EqueueWaitStat{std::string(equeue->GetName())});
        }
        ++it->waits;
        it->ns += ns;
        it->events += u64(*out);
    }

    if (*out == 0) {
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAddHRTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelTimespec* ts,
                                          void* udata) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    const auto total_ns = ts->tv_sec * 1000000000 + ts->tv_nsec;

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::HrTimer;
    event.event.flags = OrbisKernelEvent::Flags::Add | OrbisKernelEvent::Flags::OneShot;
    event.event.fflags = 0;
    // Data is stored as the address of a OrbisKernelBintime struct.
    OrbisKernelBintime time{ts->tv_sec, ts->tv_nsec * 0x44b82fa09};
    event.event.data = reinterpret_cast<u64>(&time);
    event.event.udata = udata;

    // HR timers cannot be implemented within the existing event queue architecture due to the
    // slowness of the notification mechanism. For instance, a 100us timer will lose its precision
    // as the trigger time drifts by +50-700%, depending on the host PC and workload. To address
    // this issue, we use a spinlock for small waits (which can be adjusted using
    // `HrTimerSpinlockThresholdUs`) and fall back to boost asio timers if the time to tick is
    // large. Even for large delays, we truncate a small portion to complete the wait
    // using the spinlock, prioritizing precision.
    auto& equeue = kqueues[eq];
    if (total_ns < HrTimerSpinlockThresholdNs) {
        return equeue->AddSmallTimer(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
    }

    if (!equeue->AddEvent(event)) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteHRTimerEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    auto& equeue = kqueues[eq];
    if (equeue->HasSmallTimer()) {
        return equeue->RemoveSmallTimer(id) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOENT;
    } else {
        return equeue->RemoveEvent(id, OrbisKernelEvent::Filter::HrTimer)
                   ? ORBIS_OK
                   : ORBIS_KERNEL_ERROR_ENOENT;
    }
}

int PS4_SYSV_ABI sceKernelAddTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelUseconds usec,
                                        void* udata) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = static_cast<u64>(id);
    event.event.filter = OrbisKernelEvent::Filter::Timer;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    event.event.fflags = 0;
    event.event.data = usec / 1000;
    event.event.udata = udata;

    auto& equeue = kqueues[eq];
    if (!equeue->AddEvent(event)) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteTimerEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    return kqueues[eq]->RemoveEvent(id, OrbisKernelEvent::Filter::Timer)
               ? ORBIS_OK
               : ORBIS_KERNEL_ERROR_ENOENT;
}

int PS4_SYSV_ABI sceKernelAddUserEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::User;
    event.event.udata = 0;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    event.event.fflags = 0;
    event.event.data = 0;

    return kqueues[eq]->AddEvent(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
}

int PS4_SYSV_ABI sceKernelAddUserEventEdge(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::User;
    event.event.udata = 0;
    event.event.flags = OrbisKernelEvent::Flags::Add | OrbisKernelEvent::Flags::Clear;
    event.event.fflags = 0;
    event.event.data = 0;

    return kqueues[eq]->AddEvent(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
}

// psport: no null check, as libkernel's (0x1abf0): a null event faults reading 0x18 and the
// fault handler stops the game with its frames, as the console kills it.
void* PS4_SYSV_ABI sceKernelGetEventUserData(const OrbisKernelEvent* ev) {
    return ev->udata;
}

u64 PS4_SYSV_ABI sceKernelGetEventId(const OrbisKernelEvent* ev) {
    return ev->ident;
}

int PS4_SYSV_ABI sceKernelTriggerUserEvent(OrbisKernelEqueue eq, int id, void* udata) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    if (!kqueues[eq]->TriggerEvent(id, OrbisKernelEvent::Filter::User, udata)) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteUserEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    if (!kqueues[eq]->RemoveEvent(id, OrbisKernelEvent::Filter::User)) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelGetEventFilter(const OrbisKernelEvent* ev) {
    return ev->filter;
}

u64 PS4_SYSV_ABI sceKernelGetEventData(const OrbisKernelEvent* ev) {
    return ev->data;
}

// psport: shadPS4 124328bcd, without its assertions; descriptors checked in the runtime's table.
u32 PS4_SYSV_ABI sceKernelGetEventFflags(const OrbisKernelEvent* ev) {
    return ev ? ev->fflags : 0;
}

// An EV_ERROR event carries its errno in data (FreeBSD's kevent); any other has no error. Which
// field the console's libkernel reads is not confirmed (shadPS4 reads fflags).
int PS4_SYSV_ABI sceKernelGetEventError(const OrbisKernelEvent* ev) {
    return ev && (ev->flags & OrbisKernelEvent::Flags::Error) && ev->data
               ? static_cast<int>(0x80020000 | (ev->data & 0xffff))
               : 0;
}

static int AddIoEvent(OrbisKernelEqueue eq, int fd, OrbisKernelEvent::Filter filter, u32 fflags, u64 data,
                      void* udata) {
    if (!kqueues.contains(eq) || fd < 0 || !DescriptorSerial(static_cast<u64>(fd))) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }
    EqueueEvent event{};
    event.event.ident = static_cast<u64>(fd);
    event.event.filter = filter;
    event.event.udata = udata;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    event.event.fflags = fflags;
    event.event.data = data;
    return kqueues[eq]->AddEvent(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
}

static int DeleteIoEvent(OrbisKernelEqueue eq, int fd, OrbisKernelEvent::Filter filter) {
    if (!kqueues.contains(eq) || fd < 0) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }
    return kqueues[eq]->RemoveEvent(static_cast<u64>(fd), filter) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOENT;
}

int PS4_SYSV_ABI sceKernelAddReadEvent(OrbisKernelEqueue eq, int fd, size_t size, void* udata) {
    return AddIoEvent(eq, fd, OrbisKernelEvent::Filter::Read, size != 0 ? 1 : 0, size, udata);
}

int PS4_SYSV_ABI sceKernelDeleteReadEvent(OrbisKernelEqueue eq, int fd) {
    return DeleteIoEvent(eq, fd, OrbisKernelEvent::Filter::Read);
}

int PS4_SYSV_ABI sceKernelAddWriteEvent(OrbisKernelEqueue eq, int fd, size_t size, void* udata) {
    return AddIoEvent(eq, fd, OrbisKernelEvent::Filter::Write, size != 0 ? 1 : 0, size, udata);
}

int PS4_SYSV_ABI sceKernelDeleteWriteEvent(OrbisKernelEqueue eq, int fd) {
    return DeleteIoEvent(eq, fd, OrbisKernelEvent::Filter::Write);
}

// The vnode notes libkernel accepts (shadPS4 124328bcd, from its disassembly).
int PS4_SYSV_ABI sceKernelAddFileEvent(OrbisKernelEqueue eq, int fd, u32 fflags, void* udata) {
    if (fflags & 0xffffff90) {
        return kqueues.contains(eq) ? ORBIS_KERNEL_ERROR_EINVAL : ORBIS_KERNEL_ERROR_EBADF;
    }
    return AddIoEvent(eq, fd, OrbisKernelEvent::Filter::Vnode, fflags, 0, udata);
}

int PS4_SYSV_ABI sceKernelDeleteFileEvent(OrbisKernelEqueue eq, int fd) {
    return DeleteIoEvent(eq, fd, OrbisKernelEvent::Filter::Vnode);
}

void RegisterEventQueue(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("nh2IFMgKTv8", "libScePosix", 1, "libkernel", posix_kqueue);
    LIB_FUNCTION("RW-GEfpnsqg", "libScePosix", 1, "libkernel", posix_kevent);
    LIB_FUNCTION("D0OdFMjp46I", "libkernel", 1, "libkernel", sceKernelCreateEqueue);
    LIB_FUNCTION("jpFjmgAC5AE", "libkernel", 1, "libkernel", sceKernelDeleteEqueue);
    LIB_FUNCTION("fzyMKs9kim0", "libkernel", 1, "libkernel", sceKernelWaitEqueue);
    LIB_FUNCTION("vz+pg2zdopI", "libkernel", 1, "libkernel", sceKernelGetEventUserData);
    LIB_FUNCTION("4R6-OvI2cEA", "libkernel", 1, "libkernel", sceKernelAddUserEvent);
    LIB_FUNCTION("WDszmSbWuDk", "libkernel", 1, "libkernel", sceKernelAddUserEventEdge);
    LIB_FUNCTION("R74tt43xP6k", "libkernel", 1, "libkernel", sceKernelAddHRTimerEvent);
    LIB_FUNCTION("J+LF6LwObXU", "libkernel", 1, "libkernel", sceKernelDeleteHRTimerEvent);
    LIB_FUNCTION("57ZK+ODEXWY", "libkernel", 1, "libkernel", sceKernelAddTimerEvent);
    LIB_FUNCTION("YWQFUyXIVdU", "libkernel", 1, "libkernel", sceKernelDeleteTimerEvent);
    LIB_FUNCTION("F6e0kwo4cnk", "libkernel", 1, "libkernel", sceKernelTriggerUserEvent);
    LIB_FUNCTION("LJDwdSNTnDg", "libkernel", 1, "libkernel", sceKernelDeleteUserEvent);
    LIB_FUNCTION("mJ7aghmgvfc", "libkernel", 1, "libkernel", sceKernelGetEventId);
    LIB_FUNCTION("23CPPI1tyBY", "libkernel", 1, "libkernel", sceKernelGetEventFilter);
    LIB_FUNCTION("kwGyyjohI50", "libkernel", 1, "libkernel", sceKernelGetEventData);
    // psport: shadPS4 124328bcd
    LIB_FUNCTION("VHCS3rCd0PM", "libkernel", 1, "libkernel", sceKernelAddReadEvent);
    LIB_FUNCTION("JxJ4tfgKlXA", "libkernel", 1, "libkernel", sceKernelDeleteReadEvent);
    LIB_FUNCTION("R-tyYMpYaxY", "libkernel", 1, "libkernel", sceKernelAddWriteEvent);
    LIB_FUNCTION("cBGTk8S92XM", "libkernel", 1, "libkernel", sceKernelDeleteWriteEvent);
    LIB_FUNCTION("cG3t15OK4Ow", "libkernel", 1, "libkernel", sceKernelAddFileEvent);
    LIB_FUNCTION("UmXngHKB6is", "libkernel", 1, "libkernel", sceKernelDeleteFileEvent);
    LIB_FUNCTION("Q0qr9AyqJSk", "libkernel", 1, "libkernel", sceKernelGetEventFflags);
    LIB_FUNCTION("Uu-iDFC9aUc", "libkernel", 1, "libkernel", sceKernelGetEventError);
}

} // namespace Libraries::Kernel
