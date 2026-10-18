// psport: optimizations that can be switched off while the game runs (PS_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <array>
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "../../src/platform/ps_host.h"

extern "C" std::uint64_t g_runtime_disabled_optimizations;
/// Temporary experiment bits: the second number in PS_TOGGLE_FILE (runtime_memory.c).
extern "C" std::uint64_t g_runtime_experiment_bits;
/// Recovery point for speculative guest memory reads on this thread (runtime_memory.c).
#ifdef _WIN32
// Through the runtime's accessor, since a DLL cannot import a thread_local of the executable.
extern "C" PsHostRecoveryPoint** runtime_fault_recover_slot(void);
#define g_p_runtime_fault_recover (*runtime_fault_recover_slot())
#else
extern "C" __thread PsHostRecoveryPoint* g_p_runtime_fault_recover;
#endif

namespace PsToggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    /// Images tiled back into system memory are tiled into VRAM and copied over in one transfer;
    /// bit set: tiled straight into system memory, one write over the bus per texel, as before.
    LocalTiling = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the post-processing targets
    UpdateImageFastPath = 1u << 30,
    /// Bit set: the texture collector frees nothing (A/B of its evictions while the game runs).
    TextureCollector = 1ull << 31,
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    // On by default (bit set: off): history of a thin feature this jitter phase missed is kept
    // when nothing moves. Static-camera flicker of railings/window bars p99.9 -45% (2026-10-03).
    TaaKeepNearerHistory = 1ull << 55,
    /// FSR 4 (4.0 and 4.1.1) gets the motion vectors with y negated (motionVectorScale.y = -1); FSR 3
    /// and TAA keep them. A/B of the vertical convention FSR 4 expects.
    Fsr4MotionYFlip = 1ull << 56,
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    /// The command stream is cut into segments recorded in parallel (PS_VK_RECORD_THREADS);
    /// bit set: no new cuts, one recording thread at a time.
    ParallelRecording = 1ull << 58,
    /// Finished GPU writes are read back on the readback queue (BufferCache::DownloadMemory);
    /// bit set: on the graphics queue after everything queued, as before.
    ReadbackQueue = 1ull << 59,
    /// Camera vectors from the inverse of the view matrix itself instead of the game's stored
    /// inverse; bit set: the other one of PS_CAMERA_INVERSE (A/B while the game runs).
    CameraOwnInverse = 1ull << 60,
    /// Scene textures use PS_ANISO (16) times anisotropic filtering instead of the game's
    /// ratio; bit set: the game's own samplers (A/B while the game runs).
    ForcedAniso = 1ull << 61,
    /// Camera motion vectors: the screen y sign derived from the G-buffer viewport, inverted
    /// (A/B of the y convention while the game runs).
    CameraYFlip = 1ull << 62,
    /// FSR 4 on the scaled presets gets the tonemapped frame decoded to linear light (its output
    /// encoded again); bit set: the encoded frame as before (A/B while the game runs).
    Fsr4EncodedInput = 1ull << 63,
    // Bits 20-29 are used as raw debug toggles by the camera/object motion and the upscaler.
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&g_runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
/// A temporary experiment switched on by bit `bit` of the second number in PS_TOGGLE_FILE.
inline bool Experiment(std::uint64_t bit) {
    return (__atomic_load_n(&g_runtime_experiment_bits, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace PsToggle

namespace PsStats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> g_c_tracker_faults{0};
/// Writes the GPU side heard of from the writers (libc copies over watched pages, no write tracking).
inline std::atomic<std::uint64_t> g_c_cpu_write_notes{0}, g_cb_cpu_write_note{0};
/// Ranges the game's GPU memory allocator handed out (guest hook).
inline std::atomic<std::uint64_t> g_c_gpu_range_allocs{0}, g_cb_gpu_range_alloc{0};
/// Buffer bindings: bytes bound where the game's memory is (read over PCIe on a
/// discrete GPU) and bytes bound to VRAM copies.
inline std::atomic<std::uint64_t> g_cb_bound_in_place{0}, g_cb_bound_vram{0};
inline std::atomic<std::uint64_t> g_cb_bound_in_place_written{0};
/// Command processor writes put into the GPU's VRAM data (no readback); blocks whose GPU data went
/// back to the game's memory before moving in place; downloads of GPU data (readbacks).
inline std::atomic<std::uint64_t> g_c_gpu_data_updates{0}, g_c_copied_back_blocks{0}, g_c_readbacks{0};
inline std::atomic<std::uint64_t> g_c_late_vram_writes{0}; ///< labels and the like put into VRAM copies
/// Copies of in-place data the CPU writes into VRAM for reading (ShadowCopy).
inline std::atomic<std::uint64_t> g_c_shadow_copies{0}, g_cb_shadow{0};
/// Images tiled back into system memory: through VRAM with one copy (LocalTiling), and bytes.
inline std::atomic<std::uint64_t> g_c_local_tilings{0}, g_cb_local_tiling{0};
/// Submissions sent early because the GPU had finished everything before them.
inline std::atomic<std::uint64_t> g_c_idle_flushes{0};
/// EOP fences with data: decoded, and their labels written (a growing gap: lost fences, guest leaks).
inline std::atomic<std::uint64_t> g_eop_decoded{0}, g_eop_written{0}, g_c_gpu_labels{0};
/// sceGnmAreSubmitsAllowed calls, and those that answered no (submission lock held).
inline std::atomic<std::uint64_t> g_c_submits_allowed_queries{0}, g_submits_refused{0}, g_c_submit_done_calls{0};
/// Guest time blocked in Gnm submissions on the previous frame (submission lock or PS_SUBMIT_LOCK=frame).
inline std::atomic<std::uint64_t> g_c_gnm_frame_waits{0}, g_gnm_frame_wait_ns{0};
/// Command buffers checksummed by the frame overlap guard (PsSubmitGuard): time (ns) and bytes.
inline std::atomic<std::uint64_t> g_submit_guard_ns{0}, g_cb_submit_guard{0};
/// Submissions to the compute queues (sceGnmDingDong).
inline std::atomic<std::uint64_t> g_c_asc_submits{0};
/// The game's GPU range allocators, as its profile's hooks see them: object, live ranges, bytes.
inline std::array<std::atomic<std::uint64_t>, 4> g_range_allocators{}, g_range_live{}, g_range_bytes{};
inline std::atomic<std::int64_t> g_c_hot_pages{0};
/// Stall diagnostics (PS_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> g_images_registered{0}, g_images_unregistered{0};
inline std::atomic<std::uint64_t> g_cb_image_upload{0};
inline std::atomic<std::uint64_t> g_cb_buffer_upload{0};
inline std::atomic<int> g_gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
inline std::atomic<std::uint64_t> g_c_draws{0}, g_c_dispatches{0}, g_c_submissions{0};
/// Shader stage lookups (PipelineCache::GetProgram) that reused the last permutation because its
/// specialization inputs were equal, and those that built and compared a specialization.
inline std::atomic<std::uint64_t> g_c_spec_key_hits{0}, g_c_spec_builds{0};
/// Frames the GPU command thread has started (display pass), for per-frame diagnostics.
inline std::atomic<std::uint64_t> g_c_gpu_frames{0};
/// Wall time spent in operations suspected of stalls (ns, all threads).
inline std::atomic<std::uint64_t> g_t_resident{0}, g_t_protect{0}, g_t_image_create{0}, g_t_refresh{0},
    g_t_staging{0}, g_t_host_wait{0}, g_t_copy{0}, g_cb_copy{0}, g_t_read_faults{0}, g_c_read_faults{0},
    g_t_write_faults{0}, g_t_copy_cpu{0}, g_copy_sys_us{0}, g_copy_minflt{0};
/// Layer memory module (PS_LAYER_MEMORY), wall time per kind (ns, inclusive): 0 mprotect for write
/// traps, 1 promotions, 2 demotions, 3 trap processing, 4 yielding VRAM, 5 volatile refreshes,
/// 6 republishing paged entries, 7 paged uploads; and the mprotect calls.
inline std::array<std::atomic<std::uint64_t>, 12> g_t_layer{};
/// Page table entries written (blocks); 8-11 of g_t_layer: their staging request, guest view,
/// mirrors, copy command.
inline std::atomic<std::uint64_t> g_layer_entries_written{0};
inline std::atomic<std::uint64_t> g_c_layer_mprotects{0};
/// PS_FRAME_STATS: GPU busy time of the sampled frames (Vulkan::GpuBusySampler), the frames read
/// and those whose timestamps were not ready; false: no sampler (off, or no timestamps).
inline std::atomic<bool> g_is_gpu_busy_sampled{false};
inline std::atomic<std::uint64_t> g_gpu_busy_ns{0}, g_c_gpu_busy_frames{0}, g_c_gpu_busy_unread{0};
/// The draw pipe's stage B busy cycles (TSC), null without the draw pipe.
inline std::atomic<const std::atomic<std::uint64_t>*> g_p_stage_b_busy_cycles{nullptr};
/// Diagnostics are collected only with PS_FRAME_STATS=1.
inline const bool g_is_enabled = [] {
    const char* sz_env = std::getenv("PS_FRAME_STATS");
    return sz_env && sz_env[0] == '1';
}();
struct Timer {
    std::atomic<std::uint64_t>& m_total;
    std::chrono::steady_clock::time_point m_start =
        g_is_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!g_is_enabled) {
            return;
        }
        m_total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - m_start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// Wall time the GPU command thread waited for guest submissions (ns).
inline std::atomic<std::uint64_t> g_gpu_idle_ns{0};
/// Draws recorded into the reduced scene targets, and draws after the scene started.
inline std::atomic<std::uint64_t> g_c_reduced_draws{0}, g_c_scene_draws{0};
/// Wall time spent blocked in the scheduler (ns): waiting for the recording thread to drain,
/// for host copies before a submission or fence, and for GPU ticks.
inline std::atomic<std::uint64_t> g_sync_recording_ns{0}, g_host_copies_wait_ns{0}, g_tick_wait_ns{0},
    g_copy_threads_wait_ns{0}, g_c_host_copy_waits{0}, g_host_copy_waits_skipped{0};
/// Wall time the frame preparation waited for the GPU to finish an earlier frame (PS_FRAMES_AHEAD):
/// how GPU-bound the frames are (PS_FRAME_LOG).
inline std::atomic<std::uint64_t> g_present_wait_ns{0};
/// Frames shown (Presenter::PrepareFrame).
inline std::atomic<std::uint32_t> g_frame_number{0};
/// Device memory allocated (VMA blocks, arena residency) and freed, bytes.
inline std::atomic<std::uint64_t> g_cb_device_alloc{0}, g_cb_device_free{0};
/// Memory statistics: registered images (their guest size) and how many; the buffer cache's VRAM
/// for guest blocks (allocated, never returned to the driver), and the part of it unused (its
/// free list and the rest of the current 64 MiB block).
inline std::atomic<std::uint64_t> g_cb_live_image{0}, g_c_live_images{0};
/// Every Vulkan image we allocated (their memory as allocated: host sizes, mips, scene targets).
inline std::atomic<std::uint64_t> g_cb_vk_image{0};
inline std::atomic<std::uint64_t> g_cb_residency_alloc{0}, g_cb_residency_unused{0};
/// The texture cache collector: the usage it compares, the mark it starts at, images it freed.
inline std::atomic<std::uint64_t> g_cb_gc_used{0}, g_cb_gc_trigger{0}, g_c_gc_freed_images{0};
/// Its critical mark (images used a moment ago evicted past it): VRAM copies of the game's
/// memory stop short of it and yield first (BufferCache::VramPromotionsPaused, LayerProcessIdle).
inline std::atomic<std::uint64_t> g_cb_gc_critical{0};
/// Seconds of a steady clock, updated at every guest submission (cheap ages for caches).
inline std::atomic<std::uint32_t> g_coarse_second{0};
/// VRAM blocks moved back in place when idle, unbound with the memory the game unmapped, and
/// residency chunks given back to the driver.
inline std::atomic<std::uint64_t> g_cb_vram_idle{0}, g_cb_vram_unmapped{0}, g_cb_vram_chunks_freed{0};
/// Layer memory module: dynamic blocks the loaders rewrote whole (may get a VRAM copy again), and
/// GPU writes kept in place because the CPU wrote their blocks (layer_cpu_blocks).
inline std::atomic<std::uint64_t> g_cb_dynamic_reloaded{0}, g_cb_gpu_writes_kept_by_cpu{0};
/// Residency chunks, chunks sent away (ProcessIdleBlocks), and their blocks that had to stay.
inline std::atomic<std::uint64_t> g_c_residency_chunks{0}, g_c_evacuations{0}, g_evac_kept_gpu{0}, g_evac_kept_other{0};
/// Images the collector passed over (GPU-written, not evictable without memory pressure).
inline std::atomic<std::uint64_t> g_c_gc_kept_images{0};
/// Collector passes that scanned, passes skipped while it backed off (a pass freed nothing) and
/// backoffs (TextureCache::BackOffCollector).
inline std::atomic<std::uint64_t> g_c_gc_passes{0}, g_c_gc_skipped_passes{0}, g_c_gc_backoffs{0};
struct WaitTimer {
    std::atomic<std::uint64_t>& m_total;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
    ~WaitTimer() {
        m_total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - m_start).count(),
                        std::memory_order_relaxed);
    }
};
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> g_gpu_sys_us{0}, g_gpu_user_us{0}, g_c_gpu_invol_switches{0},
    g_c_gpu_vol_switches{0}, g_c_gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> g_c_gpu_signal_faults{0};
/// Protection changes: calls and pages, those removing write access (TLB shootdowns) apart.
inline std::atomic<std::uint64_t> g_c_protect_calls{0}, g_c_protect_pages{0}, g_c_protect_revoke_calls{0},
    g_c_protect_revoke_pages{0};
} // namespace PsStats
