// SPDX-License-Identifier: GPL-2.0-or-later
// The render's voice life cycle (spec 03 §2, 04 §1.2, §1.3, §1.5, §2.1, 08 §1): the flush that hands
// VoiceControl's changes to the render, the graph order of the submixer and mastering voices, the
// grain's events around each voice's processing, and the post that retires finished voices and
// calls the game's voice callbacks.
#include <algorithm>
#include <cstring>
#include <utility>

#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kSamplerRackId = 0x1000;
constexpr u32 kSubmixerRackId = 0x2000;
constexpr u32 kMasteringRackId = 0x3000;
// Re-setting a voice without PLAY kills it only when it is in use since SDK 1.75 (spec 04 §5).
constexpr u32 kSetupKillsIdleSdkVersion = 0x1750000;

void LinkActive(Rack* p_rack, Voice* p_voice) {
    p_voice->p_prev_active = p_rack->p_last_active;
    p_voice->p_next_active = nullptr;
    (p_rack->p_last_active ? p_rack->p_last_active->p_next_active : p_rack->p_first_active) = p_voice;
    p_rack->p_last_active = p_voice;
}

void UnlinkActive(Rack* p_rack, Voice* p_voice) {
    (p_voice->p_prev_active ? p_voice->p_prev_active->p_next_active : p_rack->p_first_active) =
        p_voice->p_next_active;
    (p_voice->p_next_active ? p_voice->p_next_active->p_prev_active : p_rack->p_last_active) =
        p_voice->p_prev_active;
    p_voice->p_next_active = nullptr;
    p_voice->p_prev_active = nullptr;
}

Voice* FindVoice(System* p_system, u32 rack_uid, u32 i_voice) {
    for (u32 rack_class = RACK_CLASS_INTERMEDIATE; rack_class < RACK_CLASS_COUNT; ++rack_class) {
        for (Rack* p_rack = p_system->p_first_rack[rack_class]; p_rack; p_rack = p_rack->p_next) {
            if (p_rack->uid == rack_uid) {
                return i_voice < p_rack->limits.max_voices ? &p_rack->p_voices[i_voice] : nullptr;
            }
        }
    }
    return nullptr;
}

void StartVoice(System* p_system, Voice* p_voice) {
    Rack* p_rack = p_voice->p_rack;
    if (!p_voice->is_active) {
        p_voice->is_active = true;
        p_rack->c_active_voices.fetch_add(1, std::memory_order_relaxed);
        if (p_rack->p_kind->rack_class == RACK_CLASS_SOURCE) {
            LinkActive(p_rack, p_voice);
        }
    }
    if (p_rack->p_kind->rack_class != RACK_CLASS_SOURCE) {
        p_system->is_topology_dirty = true;
    }
    p_voice->flags = STATE_IN_USE;
    p_voice->grain_events = 0;
    p_voice->is_first_grain = true;
    p_voice->is_in_cycle = false;
    p_voice->c_in_channels = p_voice->p_params->c_in_channels;
    p_voice->c_out_channels = p_voice->p_params->c_out_channels;
    std::memset(static_cast<void*>(p_voice->p_dsp), 0, sizeof(VoiceDsp));
    ResetSource(p_voice);
    std::fill_n(p_voice->p_filter_states, std::size_t(p_rack->limits.max_filters) * kMaxChannels, BiquadState{});
    for (u32 i_port = 0; i_port < p_rack->limits.max_ports; ++i_port) {
        p_voice->p_active_ports[i_port].is_fresh = true;
    }
    std::fill_n(p_voice->p_input_started, std::size_t(p_rack->limits.max_inputs) * p_rack->c_input_blocks, false);
}

void FlushPorts(System* p_system, Voice* p_voice) {
    const Rack& rack = *p_voice->p_rack;
    for (u32 i_port = 0; i_port < rack.limits.max_ports; ++i_port) {
        const VoicePort& pending = p_voice->p_ports[i_port];
        ActivePort& active = p_voice->p_active_ports[i_port];
        Voice* p_dest = nullptr;
        u32 i_input = 0;
        if (pending.is_used && pending.h_dest) {
            p_dest = FindVoice(p_system, pending.dest_rack_uid, pending.i_dest_voice);
            i_input = pending.i_dest_input;
        }
        if (p_dest != active.p_dest || i_input != active.i_input) {
            active.is_fresh = true;
            if (rack.p_kind->rack_class != RACK_CLASS_SOURCE) {
                p_system->is_topology_dirty = true;
            }
        }
        active.p_dest = p_dest;
        active.i_input = i_input;
        active.volume = pending.is_used ? pending.volume : 1.0f;
        active.matrix_id = pending.is_used ? pending.matrix_id : -1;
        active.delay_samples = pending.is_used ? pending.delay_samples : 0;
    }
}

// Spec 03 §2: under the voice's lock, everything VoiceControl left pending reaches the render.
void FlushVoice(System* p_system, Voice* p_voice) {
    VoiceLock lock{p_voice};
    p_voice->is_dirty = false;
    const u32 control = p_voice->control.load(std::memory_order_relaxed);
    u32 events = 0;
    if ((control & VOICE_PENDING_SETUP) && !(control & VOICE_PENDING_START)) {
        if (p_voice->is_active || !IsCompiledSdkAtLeast(kSetupKillsIdleSdkVersion)) {
            events |= GRAIN_KILL;
        }
        ResetSource(p_voice);
    }
    if (control & VOICE_PENDING_START) {
        StartVoice(p_system, p_voice);
        events |= GRAIN_PLAY;
    }
    constexpr std::pair<u32, u32> kEvents[] = {{VOICE_PENDING_STOP, GRAIN_STOP},
                                               {VOICE_PENDING_STOP_IMMEDIATE, GRAIN_STOP_IMMEDIATE},
                                               {VOICE_PENDING_KILL, GRAIN_KILL},
                                               {VOICE_PENDING_PAUSE, GRAIN_PAUSE},
                                               {VOICE_PENDING_RESUME, GRAIN_RESUME}};
    for (const auto& [pending_bit, grain_bit] : kEvents) {
        if (control & pending_bit) {
            events |= grain_bit;
        }
    }
    p_voice->grain_events |= events;
    p_voice->control.fetch_and(~u32(VOICE_PENDING_EVENTS | VOICE_PENDING_SETUP), std::memory_order_relaxed);
    WaveformQueue& queue = p_voice->queue;
    queue.c_committed = queue.c_blocks;
    queue.is_terminated_committed = queue.is_terminated;
    const Rack& rack = *p_voice->p_rack;
    *p_voice->p_active_params = *p_voice->p_params;
    std::copy_n(p_voice->p_filters, rack.limits.max_filters, p_voice->p_active_filters);
    for (u32 i_filter = 0; i_filter < rack.limits.max_filters; ++i_filter) {
        if (p_voice->p_active_filters[i_filter].is_set) {
            DesignFilter(&p_voice->p_active_filters[i_filter], p_system->sample_rate_hz);
        }
    }
    if (p_voice->p_mastering_matrices) {
        *p_voice->p_active_mastering_matrices = *p_voice->p_mastering_matrices;
    }
    p_voice->are_params_changed = true;
    ExpandVoiceMatrices(p_voice);
    if (control & VOICE_PENDING_START) {
        std::copy_n(p_voice->p_matrix_levels, std::size_t(rack.limits.max_matrices) * kMatrixCells,
                    p_voice->p_previous_matrix_levels);
    }
    FlushPorts(p_system, p_voice);
    p_voice->pfn_active_callback = p_voice->pfn_callback;
    p_voice->active_callback_data = p_voice->callback_data;
    p_voice->active_callback_flags = p_voice->callback_flags;
}

// Spec 04 §2.1, before the rack's processing.
u32 BeginEvents(Voice* p_voice, u32 events) {
    u32& flags = p_voice->flags;
    u32 event_code = (flags & STATE_PAUSED) ? EVENT_CODE_SILENT : EVENT_CODE_NONE;
    if ((events & GRAIN_PAUSE) && !(flags & STATE_PAUSED)) {
        if (events & GRAIN_PLAY) {
            flags = (flags | STATE_PAUSED) & ~u32(STATE_PLAYING);
            event_code = EVENT_CODE_SILENT;
        } else {
            event_code = EVENT_CODE_FADE_OUT_TAIL;
        }
    }
    if ((events & GRAIN_RESUME) && (flags & STATE_PAUSED)) {
        flags &= ~u32(STATE_PAUSED);
        event_code = EVENT_CODE_FADE_IN_HEAD;
    }
    if (events & GRAIN_STOP) {
        flags |= STATE_STOPPED;
        event_code = EVENT_CODE_STOP;
    }
    if (events & (GRAIN_STOP_IMMEDIATE | GRAIN_KILL)) {
        event_code = EVENT_CODE_CUT;
    }
    return event_code;
}

// Spec 04 §2.1, after it: a STOP the envelope took over keeps the voice for its release.
void EndEvents(Voice* p_voice, u32 events, u32 event_code) {
    u32& flags = p_voice->flags;
    if ((events & GRAIN_PAUSE) && event_code == EVENT_CODE_FADE_OUT_TAIL) {
        flags = (flags | STATE_PAUSED) & ~u32(STATE_PLAYING);
    }
    if ((events & (GRAIN_STOP_IMMEDIATE | GRAIN_KILL)) || ((events & GRAIN_STOP) && event_code == EVENT_CODE_STOP)) {
        flags = 0;
    }
}

// The voices whose ports lead to p_target, one per call, resuming the scan where the last call left it.
Voice* NextSource(Voice* p_target) {
    GraphCursor& cursor = p_target->graph_cursor;
    for (; cursor.p_rack; cursor = {cursor.p_rack->p_next, 0, 0}) {
        const RackLimits& limits = cursor.p_rack->limits;
        for (; cursor.i_voice < limits.max_voices; ++cursor.i_voice, cursor.i_port = 0) {
            Voice* p_voice = &cursor.p_rack->p_voices[cursor.i_voice];
            if (!p_voice->is_active) {
                continue;
            }
            while (cursor.i_port < limits.max_ports) {
                if (p_voice->p_active_ports[cursor.i_port++].p_dest == p_target) {
                    return p_voice;
                }
            }
        }
    }
    return nullptr;
}

void AppendOrdered(Voice** pp_tail, System* p_system, Voice* p_voice) {
    p_voice->p_next_ordered = nullptr;
    (*pp_tail ? (*pp_tail)->p_next_ordered : p_system->p_first_ordered) = p_voice;
    *pp_tail = p_voice;
}

} // namespace

void ReleaseChannelWorks(Voice* p_voice) {
    Rack* p_rack = p_voice->p_rack;
    const u32 c_works = std::exchange(p_voice->c_channel_works, 0);
    p_rack->c_channel_works_used.fetch_sub(c_works, std::memory_order_relaxed);
    for (u32 i_input = 0; i_input < p_rack->limits.max_inputs; ++i_input) {
        p_rack->c_input_works_used[i_input].fetch_sub(c_works, std::memory_order_relaxed);
    }
}

void QueueVoiceCallback(Voice* p_voice, u32 type, const WaveformBlock& block, u32 c_repeats) {
    // Flags bit 0 asks for block ends (type 1), bit 1 for loops (type 2).
    if (!(p_voice->active_callback_flags & type) || !p_voice->pfn_active_callback) {
        return;
    }
    Rack* p_rack = p_voice->p_rack;
    if (p_rack->c_callbacks >= p_rack->c_callback_capacity) {
        static std::uint64_t s_c_uses;
        NoteDropped(&s_c_uses, "Ngs2 voice callback with the rack's callback queue full");
        return;
    }
    QueuedCallback& queued = p_rack->p_callbacks[p_rack->c_callbacks++];
    queued.pfn_handler = p_voice->pfn_active_callback;
    queued.info = {};
    queued.info.callbackData = p_voice->active_callback_data;
    queued.info.voiceHandle = reinterpret_cast<OrbisNgs2Handle>(&p_voice->handle);
    queued.info.flag = type;
    auto& payload = queued.info.param.waveformBlock;
    payload.userData = block.user_data;
    payload.data = block.p_data + block.off_data;
    payload.dataSize = block.cb_data;
    payload.repeatedCount = c_repeats;
    payload.attributeFlags = block.is_marked ? 1 : 0;
}

void FlushRack(System* p_system, Rack* p_rack, bool can_flush) {
    if (p_rack->c_input_blocks > 0) {
        // The block that becomes the newest of each input ring waits for its first writer again.
        p_rack->i_input_block = (p_rack->i_input_block + 1) % p_rack->c_input_blocks;
        const u32 i_newest = (p_rack->i_input_block + p_rack->c_input_blocks - 1) % p_rack->c_input_blocks;
        for (u32 i_voice = 0; i_voice < p_rack->limits.max_voices; ++i_voice) {
            Voice& voice = p_rack->p_voices[i_voice];
            for (u32 i_input = 0; i_input < p_rack->limits.max_inputs; ++i_input) {
                voice.p_input_started[i_input * p_rack->c_input_blocks + i_newest] = false;
            }
        }
    }
    if (!can_flush || !p_rack->flush_lock.TryLock()) {
        return;
    }
    for (u32 i_voice = 0; i_voice < p_rack->limits.max_voices; ++i_voice) {
        Voice* p_voice = &p_rack->p_voices[i_voice];
        if (p_voice->is_dirty.load(std::memory_order_relaxed)) {
            FlushVoice(p_system, p_voice);
        }
    }
    p_rack->flush_lock.Unlock();
}

// Spec 04 §1.3: from each mastering voice backwards along the patches, sources before their
// destinations; a voice met again while its own sources are being walked closes a cycle.
void OrderGraph(System* p_system) {
    p_system->is_topology_dirty = false;
    p_system->p_first_ordered = nullptr;
    p_system->state_flags.fetch_and(~u32(SYSTEM_STATE_CYCLE), std::memory_order_relaxed);
    for (u32 rack_class = RACK_CLASS_INTERMEDIATE; rack_class < RACK_CLASS_COUNT; ++rack_class) {
        for (Rack* p_rack = p_system->p_first_rack[rack_class]; p_rack; p_rack = p_rack->p_next) {
            for (u32 i_voice = 0; i_voice < p_rack->limits.max_voices; ++i_voice) {
                p_rack->p_voices[i_voice].graph_mark = GRAPH_UNVISITED;
                p_rack->p_voices[i_voice].is_in_cycle = false;
            }
        }
    }
    Voice* p_tail = nullptr;
    Rack* p_first_source_rack = p_system->p_first_rack[RACK_CLASS_INTERMEDIATE];
    for (Rack* p_rack = p_system->p_first_rack[RACK_CLASS_MASTERING]; p_rack; p_rack = p_rack->p_next) {
        for (u32 i_voice = 0; i_voice < p_rack->limits.max_voices; ++i_voice) {
            Voice* p_root = &p_rack->p_voices[i_voice];
            if (!p_root->is_active) {
                continue;
            }
            p_root->graph_mark = GRAPH_VISITING;
            p_root->p_graph_parent = nullptr;
            p_root->graph_cursor = {p_first_source_rack, 0, 0};
            for (Voice* p_current = p_root; p_current;) {
                Voice* p_source = NextSource(p_current);
                if (!p_source) {
                    p_current->graph_mark = GRAPH_DONE;
                    AppendOrdered(&p_tail, p_system, p_current);
                    p_current = p_current->p_graph_parent;
                } else if (p_source->graph_mark == GRAPH_VISITING) {
                    p_source->is_in_cycle = true;
                    p_system->state_flags.fetch_or(SYSTEM_STATE_CYCLE, std::memory_order_relaxed);
                } else if (p_source->graph_mark == GRAPH_UNVISITED) {
                    p_source->graph_mark = GRAPH_VISITING;
                    p_source->p_graph_parent = p_current;
                    p_source->graph_cursor = {p_first_source_rack, 0, 0};
                    p_current = p_source;
                }
            }
        }
    }
}

void ProcessVoice(RenderContext& context, Voice* p_voice) {
    VoiceLock lock{p_voice};
    const u32 events = std::exchange(p_voice->grain_events, 0);
    u32 event_code = BeginEvents(p_voice, events);
    // A voice played without a setup has no channels: its state is cleared and the voice ends
    // (spec 04 §2.2).
    const u32 rack_id = p_voice->p_active_params->is_setup ? p_voice->p_rack->p_kind->rack_id : 0;
    switch (rack_id) {
    case 0:
        p_voice->flags = 0;
        break;
    case kSamplerRackId:
        ProcessSamplerVoice(context, p_voice, &event_code);
        break;
    case kSubmixerRackId:
        ProcessSubmixerVoice(context, p_voice, &event_code);
        break;
    case kMasteringRackId:
        ProcessMasteringVoice(context, p_voice, &event_code);
        break;
    default: {
        // A reverb (F5) or custom (F6) voice cannot pass its setup yet, so it cannot play either.
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 reverb and custom rack processing (F5, F6)");
        break;
    }
    }
    EndEvents(p_voice, events, event_code);
    p_voice->is_first_grain = false;
    p_voice->are_params_changed = false;
}

void PostRack(Rack* p_rack) {
    System* p_system = p_rack->p_system;
    for (u32 i_voice = 0; i_voice < p_rack->limits.max_voices; ++i_voice) {
        Voice* p_voice = &p_rack->p_voices[i_voice];
        if (!p_voice->is_active) {
            continue;
        }
        VoiceLock lock{p_voice};
        if (p_voice->flags == 0) {
            ReleaseChannelWorks(p_voice);
            for (u32 i_port = 0; i_port < p_rack->limits.max_ports; ++i_port) {
                p_voice->p_active_ports[i_port].p_dest = nullptr;
            }
            if (p_rack->p_kind->rack_class == RACK_CLASS_SOURCE) {
                UnlinkActive(p_rack, p_voice);
            } else if (p_system) {
                p_system->is_topology_dirty = true;
            }
            p_voice->is_active = false;
            p_rack->c_active_voices.fetch_sub(1, std::memory_order_relaxed);
            if (p_voice->p_state) {
                std::memset(p_voice->p_state, 0, p_rack->limits.cb_voice_state);
            }
        }
        u32 control = p_voice->control.load(std::memory_order_relaxed);
        u32 flags = p_voice->flags;
        // A PLAY that arrived during this render shows as in use until the next flush starts it.
        if (control & VOICE_PENDING_START) {
            flags |= STATE_IN_USE;
        } else {
            control &= ~u32(VOICE_INITIAL_STATE);
        }
        p_voice->control.store((control & ~u32(VOICE_STATE_MASK)) | (flags & VOICE_STATE_MASK),
                               std::memory_order_relaxed);
    }
    // The handlers may call back into Ngs2 (VoiceControl, GetState), so no voice lock is held.
    for (u32 i_callback = 0; i_callback < p_rack->c_callbacks; ++i_callback) {
        const QueuedCallback& queued = p_rack->p_callbacks[i_callback];
        queued.pfn_handler(&queued.info);
    }
    p_rack->c_callbacks = 0;
    p_rack->c_renders.fetch_add(1, std::memory_order_relaxed);
}

void ForgetRackVoices(System* p_system, Rack* p_rack) {
    const auto va_first = reinterpret_cast<uintptr_t>(p_rack->p_voices);
    const auto va_end = reinterpret_cast<uintptr_t>(p_rack->p_voices + p_rack->limits.max_voices);
    for (Rack* p_first_rack : p_system->p_first_rack) {
        for (Rack* p_other = p_first_rack; p_other; p_other = p_other->p_next) {
            for (u32 i_voice = 0; i_voice < p_other->limits.max_voices; ++i_voice) {
                Voice& voice = p_other->p_voices[i_voice];
                for (u32 i_port = 0; i_port < p_other->limits.max_ports; ++i_port) {
                    ActivePort& port = voice.p_active_ports[i_port];
                    const auto va_dest = reinterpret_cast<uintptr_t>(port.p_dest);
                    if (va_dest >= va_first && va_dest < va_end) {
                        port.p_dest = nullptr;
                    }
                }
            }
        }
    }
    OrderGraph(p_system);
}

} // namespace Libraries::Ngs2
