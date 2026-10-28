// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/error.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/network/net.h"

#include <deque>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <wepoll.h>
#endif

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
// ADD libepoll-shim if using freebsd!
#include <sys/epoll.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/eventfd.h>
#endif

namespace Libraries::Net {

#ifdef _WIN32
using epoll_handle = HANDLE;
#else
using epoll_handle = int;
#endif

// psport: the epoll tag of an Epoll's abort_fd, apart from the net ids the other entries carry.
constexpr u64 EPOLL_ABORT_TAG = ~u64{0};

struct Epoll {
    std::vector<std::pair<u32 /*netId*/, OrbisNetEpollEvent>> events{};
    std::string name;
    epoll_handle epoll_fd;
    std::deque<u32> async_resolutions{};
#ifdef __linux__
    // psport: sceNetEpollAbort wakes the threads waiting at that moment through this eventfd, which
    // the last of them to leave clears, so an abort with nobody waiting has no effect (libSceNet's
    // waits end with EINTR when the kernel aborts them, kb #735).
    int abort_fd{-1};
    int c_waiters{};
#endif
    // psport: the host errno of a failed epoll_create1, which sceNetEpollCreate returns mapped.
    int create_errno{};

    explicit Epoll(const char* name_) : name(name_), epoll_fd(epoll_create1(0)) {
        if (name_ == nullptr) {
            name = "anon";
        }
        if (!IsCreated()) {
            create_errno = errno;
            LOG_ERROR_COUNTED(Lib_Net, "epoll {}: host epoll_create1 failed: {}", name,
                              Common::GetLastErrorMsg());
            return;
        }
#ifdef __linux__
        abort_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        epoll_event abort_event{.events = EPOLLIN, .data = {.u64 = EPOLL_ABORT_TAG}};
        if (abort_fd == -1 || epoll_ctl(epoll_fd, EPOLL_CTL_ADD, abort_fd, &abort_event) == -1) {
            LOG_ERROR(Lib_Net, "epoll {}: no abort eventfd ({}), sceNetEpollAbort will not wake it",
                      name, Common::GetLastErrorMsg());
        }
#endif
    }

    bool IsCreated() const noexcept {
#ifdef _WIN32
        return epoll_fd != nullptr;
#else
        return epoll_fd != -1;
#endif
    }

    bool Destroyed() const noexcept {
        return destroyed;
    }

    void Destroy() noexcept {
        events.clear();
#ifdef _WIN32
        epoll_close(epoll_fd);
        epoll_fd = nullptr;
#else
        close(epoll_fd);
        epoll_fd = -1;
#endif
#ifdef __linux__
        if (abort_fd != -1) {
            close(abort_fd);
            abort_fd = -1;
        }
#endif
        name = "";
        destroyed = true;
    }

private:
    bool destroyed{};
};

u32 ConvertEpollEventsIn(u32 orbis_events);
u32 ConvertEpollEventsOut(u32 epoll_events);

class EpollTable {
public:
    EpollTable() = default;
    virtual ~EpollTable() = default;

    int CreateHandle(const char* name);
    void DeleteHandle(int d);
    Epoll* GetEpoll(int d);

private:
    std::vector<Epoll> epolls;
    std::mutex m_mutex;
};

} // namespace Libraries::Net
