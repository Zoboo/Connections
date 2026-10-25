#include "platform_profile.h"
#if CONNECTIONS_PROFILE
#include <switch.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "rendering/drawing.h"
#include "core/version.h"

/* Software renderer: CPU submission is measured; GPU and audio underruns
 * cannot be inferred from SDL frame/queue timings. No clocks are changed. */
#define BINS 1001
#define BUDGET_MS (1000.0 / 30.0)
typedef struct {
    uint64_t frames, late, missed;
    double sum, worst, work_worst, update_sum, render_sum;
    uint32_t histogram[BINS]; /* 0.1 ms buckets, final bucket >=100 ms */
    uint64_t first, last;
    char scenario[48];
} Samples;
typedef struct {
    double fps, avg, p99, worst, work, update, render, network;
    uint64_t late, missed, memory;
    uint32_t clocks[3];
    char mode[32];
} Display;
static Samples samples;
static Display display;
static SDL_SpinLock guard;
static SDL_atomic_t stopped, reset, network_us, skipped, overflow, focus_reset, paused_excluded;
static AppletHookCookie focus_hook;
static void focus_changed(AppletHookType hook, void *unused) {
    (void)unused;
    if (hook == AppletHookType_OnFocusState || hook == AppletHookType_OnResume)
        SDL_AtomicSet(&focus_reset, 1);
}
#define SEGMENTS 16
typedef struct { Samples samples; Display metadata; } Segment;
static Segment completed[SEGMENTS];
static unsigned segment_head, segment_count;
static FILE *profile_log;
static SDL_atomic_t log_error;

/* Called only while guard is owned; bounded and never performs I/O. */
static void finish_segment(void) {
    if (!samples.frames) return;
    if (segment_count == SEGMENTS) { SDL_AtomicAdd(&overflow, 1); return; }
    unsigned index = (segment_head + segment_count++) % SEGMENTS;
    completed[index].samples = samples;
    completed[index].metadata = display;
}
static SDL_Thread *worker;
static bool enabled;
static uint64_t begun, updated, rendered, previous;
static double tick_ms;

static void log_result(const char *call, Result rc) {
    SDL_Log("PERF_SERVICE %s Result=0x%08lX", call, (unsigned long)rc);
    if (profile_log) {
        fprintf(profile_log, "PERF_SERVICE %s Result=0x%08lX\n", call, (unsigned long)rc);
        fflush(profile_log);
    }
}

static void summarize(const Samples *snapshot, Display *out, const char *kind) {
    Display next = *out;
        double seconds = (snapshot->last - snapshot->first) * tick_ms / 1000.0;
        if (snapshot->frames) {
            next.avg = snapshot->sum / snapshot->frames;
            next.update = snapshot->update_sum / snapshot->frames;
            next.render = snapshot->render_sum / snapshot->frames;
            next.fps = next.avg > 0 ? 1000.0 / next.avg : 0;
            uint64_t rank = (snapshot->frames * 99 + 99) / 100, count = 0;
            for (int i = 0; i < BINS; ++i) {
                count += snapshot->histogram[i];
                if (count >= rank) { next.p99 = (i + 1) * 0.1; break; }
            }
        }
        next.worst = snapshot->worst; next.work = snapshot->work_worst;
        next.late = snapshot->late; next.missed = snapshot->missed;
        next.network = out->network;
        char line[900];
        bool known = next.clocks[0] && next.clocks[1] && next.clocks[2] && !strstr(next.mode, "unknown") && next.mode[0];
        int lost = SDL_AtomicGet(&skipped), full = SDL_AtomicGet(&overflow);
        snprintf(line, sizeof(line), "PERF target_fps=30 mode=%s cpu_hz=%lu gpu_hz=%lu emc_hz=%lu scenario=%s frames=%llu seconds=%.2f avg_ms=%.3f p99_ms=%.3f worst_ms=%.3f work_worst_ms=%.3f update_ms=%.3f render_cpu_ms=%.3f late=%llu missed=%llu network_ms=%.3f memory_bytes=%llu status=%s gpu_time=NA audio_underruns=NA kind=%s skipped_samples=%d lost_segments=%d paused_excluded=%d\n",
                 next.mode[0] ? next.mode : "unknown", (unsigned long)next.clocks[0], (unsigned long)next.clocks[1], (unsigned long)next.clocks[2], snapshot->scenario,
                 (unsigned long long)snapshot->frames, seconds, next.avg, next.p99, next.worst, next.work, next.update, next.render,
                 (unsigned long long)next.late, (unsigned long long)next.missed, next.network, (unsigned long long)next.memory,
                 !known ? "unmeasured-clocks" : lost || full ? "incomplete" : seconds < 120 ? "sampling" : next.work <= BUDGET_MS * 0.8 && !next.late ? "candidate" : "insufficient-headroom", kind, lost, full, SDL_AtomicGet(&paused_excluded));
        SDL_Log("%s", line);
        if (profile_log) { fputs(line, profile_log); fflush(profile_log); }
        *out = next;
}
static void flush_segments(void) {
    for (;;) {
        Segment item;
        SDL_AtomicLock(&guard);
        if (!segment_count) { SDL_AtomicUnlock(&guard); break; }
        item = completed[segment_head];
        segment_head = (segment_head + 1) % SEGMENTS; --segment_count;
        SDL_AtomicUnlock(&guard);
        summarize(&item.samples, &item.metadata, "segment");
    }
}

static int profile_worker(void *unused) {
    (void)unused;
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/Connections", 0777);
    profile_log = fopen("sdmc:/switch/Connections/performance.log", "a");
    if (!profile_log) {
        SDL_AtomicSet(&log_error, errno);
        SDL_Log("PERF_LOG Cannot open performance.log: errno=%d (%s)", errno, strerror(errno));
    }
    else {
        fputs("PERF_LOG version=" CONNECTIONS_APP_VERSION " profiling enabled; summaries every 10 seconds\n", profile_log);
        fflush(profile_log);
    }
    ClkrstSession sessions[3] = {0};
    const PcvModuleId modules[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
    bool opened[3] = {false}, clocks_ok = false;
    Result rc = clkrstInitialize();
    log_result("clkrstInitialize", rc);
    clocks_ok = R_SUCCEEDED(rc);
    if (clocks_ok) for (int i = 0; i < 3; ++i) {
        rc = clkrstOpenSession(&sessions[i], modules[i], 3);
        log_result("clkrstOpenSession", rc);
        opened[i] = R_SUCCEEDED(rc);
        if (opened[i]) {
            u32 rates[32] = {0}; s32 count = 0;
            PcvClockRatesListType type = PcvClockRatesListType_Invalid;
            rc = clkrstGetPossibleClockRates(&sessions[i], rates, 32, &type, &count);
            log_result("clkrstGetPossibleClockRates", rc);
            if (R_SUCCEEDED(rc)) {
                char list[512];
                int used = snprintf(list, sizeof(list), "PERF_RATES module=%s type=%s count=%ld hz=", i == 0 ? "cpu" : i == 1 ? "gpu" : "emc",
                    type == PcvClockRatesListType_Discrete ? "discrete" : type == PcvClockRatesListType_Range ? "range" : "invalid", (long)count);
                for (s32 n = 0; n < count && n < 32 && used < (int)sizeof(list) - 20; ++n)
                    used += snprintf(list + used, sizeof(list) - used, "%s%lu", n ? "," : "", (unsigned long)rates[n]);
                SDL_Log("%s", list);
                if (profile_log) { fprintf(profile_log, "%s\n", list); fflush(profile_log); }
            }
        }
    }
    rc = psmInitialize();
    log_result("psmInitialize", rc);
    bool power_ok = R_SUCCEEDED(rc);
    uint32_t old_clocks[3] = {0};
    char old_mode[32] = "";
    unsigned ticks = 0;
    while (!SDL_AtomicGet(&stopped)) {
        SDL_Delay(100);
        if (++ticks < 100) continue;
        ticks = 0;
        Display next = {0};
        for (int i = 0; i < 3; ++i) if (opened[i]) {
            rc = clkrstGetClockRate(&sessions[i], &next.clocks[i]);
            if (R_FAILED(rc)) log_result("clkrstGetClockRate", rc);
        }
        PsmChargerType charger = PsmChargerType_Unconnected;
        bool charger_known = false;
        if (power_ok) {
            rc = psmGetChargerType(&charger);
            charger_known = R_SUCCEEDED(rc);
            if (R_FAILED(rc)) log_result("psmGetChargerType", rc);
        }
        snprintf(next.mode, sizeof(next.mode), "%s", appletGetOperationMode() == AppletOperationMode_Console ? "docked" :
                 !charger_known ? "handheld-power-unknown" : charger == PsmChargerType_Unconnected ? "handheld" : "handheld-charging");
        rc = svcGetInfo(&next.memory, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        if (R_FAILED(rc)) log_result("svcGetInfo(memory)", rc);
        Samples snapshot;
        SDL_AtomicLock(&guard);
        snapshot = samples;
        SDL_AtomicUnlock(&guard);
        next.network = (uint32_t)SDL_AtomicSet(&network_us, 0) / 1000.0;
        bool changed = memcmp(old_clocks, next.clocks, sizeof(old_clocks)) || strcmp(old_mode, next.mode);
        memcpy(old_clocks, next.clocks, sizeof(old_clocks));
        snprintf(old_mode, sizeof(old_mode), "%s", next.mode);
        if (changed) {
            SDL_AtomicLock(&guard);
            for (unsigned n = 0; n < segment_count; ++n)
                memset(completed[(segment_head + n) % SEGMENTS].metadata.clocks, 0, sizeof(display.clocks));
            SDL_AtomicUnlock(&guard);
        }
        flush_segments();
        if (changed) {
            /* A transition occurred somewhere in the preceding interval.
             * Retain its timings, but do not claim a measured configuration. */
            SDL_AtomicLock(&guard);
            memset(display.clocks, 0, sizeof(display.clocks));
            finish_segment();
            char scenario[48]; memcpy(scenario, samples.scenario, sizeof(scenario));
            memset(&samples, 0, sizeof(samples)); memcpy(samples.scenario, scenario, sizeof(scenario));
            SDL_AtomicUnlock(&guard);
            SDL_AtomicSet(&reset, 1);
        } else {
            summarize(&snapshot, &next, "window");
        }
        SDL_AtomicLock(&guard); display = next; SDL_AtomicUnlock(&guard);
    }
    flush_segments();
    SDL_AtomicLock(&guard);
    Samples final = samples;
    Display final_display = display;
    SDL_AtomicUnlock(&guard);
    if (final.frames) summarize(&final, &final_display, "final");
    if (profile_log) { fclose(profile_log); profile_log = NULL; }
    for (int i = 0; i < 3; ++i) if (opened[i]) clkrstCloseSession(&sessions[i]);
    if (clocks_ok) clkrstExit();
    if (power_ok) psmExit();
    return 0;
}
void platform_profile_init(void) {
    // One application: developer profiling is opt-in at launch, never a separate NSP.
    FILE *flag = fopen("sdmc:/switch/Connections/profile.flag", "rb");
    if (!flag) return;
    fclose(flag);
    enabled = true;
    tick_ms = 1000.0 / SDL_GetPerformanceFrequency();
    memset(&samples, 0, sizeof(samples));
    snprintf(samples.scenario, sizeof(samples.scenario), "startup");
    SDL_AtomicSet(&stopped, 0);
    appletHook(&focus_hook, focus_changed, NULL);
    worker = SDL_CreateThread(profile_worker, "connections-profile", NULL);
}
bool platform_profile_is_enabled(void) { return enabled; }
void platform_profile_exit(void) {
    if (!enabled) return;
    appletUnhook(&focus_hook);
    SDL_AtomicSet(&stopped, 1);
    if (worker) { SDL_WaitThread(worker, NULL); worker = NULL; }
}
void platform_profile_set_scenario(const char *name) {
    if (!enabled) return;
    if (!SDL_AtomicTryLock(&guard)) { SDL_AtomicAdd(&skipped, 1); return; }
    if (strcmp(samples.scenario, name)) {
        finish_segment();
        memset(&samples, 0, sizeof(samples));
        snprintf(samples.scenario, sizeof(samples.scenario), "%s", name);
        previous = 0;
    }
    SDL_AtomicUnlock(&guard);
}
void platform_profile_frame_begin(void) {
    if (!enabled) return;
    begun = SDL_GetPerformanceCounter();
    if ((SDL_AtomicGet(&reset) || SDL_AtomicGet(&focus_reset)) && SDL_AtomicTryLock(&guard)) {
        SDL_AtomicSet(&reset, 0);
        SDL_AtomicSet(&focus_reset, 0);
        finish_segment();
        char scenario[48]; memcpy(scenario, samples.scenario, sizeof(scenario));
        memset(&samples, 0, sizeof(samples)); memcpy(samples.scenario, scenario, sizeof(scenario));
        SDL_AtomicUnlock(&guard); previous = 0;
    }
    updated = rendered = begun;
}
void platform_profile_update_end(void) { if (enabled) updated = SDL_GetPerformanceCounter(); }
void platform_profile_render_end(void) { if (enabled) rendered = SDL_GetPerformanceCounter(); }
void platform_profile_frame_end(void) {
    if (!enabled) return;
    /* Only explicit OS focus/resume notifications exclude a pause. Long
     * frames without such a notification remain real measured stalls. */
    if (SDL_AtomicGet(&focus_reset) || appletGetFocusState() != AppletFocusState_InFocus) {
        SDL_AtomicAdd(&paused_excluded, 1);
        previous = 0;
        return;
    }
    uint64_t now = SDL_GetPerformanceCounter();
    double frame = (previous ? now - previous : now - begun) * tick_ms;
    previous = now;
    if (!SDL_AtomicTryLock(&guard)) { SDL_AtomicAdd(&skipped, 1); return; }
    if (!samples.frames) samples.first = begun;
    samples.last = now; ++samples.frames; samples.sum += frame;
    if (frame > samples.worst) samples.worst = frame;
    double work = (rendered - begun) * tick_ms;
    if (work > samples.work_worst) samples.work_worst = work;
    samples.update_sum += (updated - begun) * tick_ms;
    samples.render_sum += (rendered - updated) * tick_ms;
    if (frame > BUDGET_MS * 1.05) ++samples.late;
    if (frame > BUDGET_MS * 1.5) samples.missed += (uint64_t)(frame / BUDGET_MS + 0.5) - 1;
    unsigned bin = (unsigned)(frame * 10); if (bin >= BINS) bin = BINS - 1;
    ++samples.histogram[bin]; SDL_AtomicUnlock(&guard);
}
void platform_profile_worker_sample(uint32_t us) { if (enabled) SDL_AtomicAdd(&network_us, (int)us); }
void platform_profile_draw(void) {
    if (!enabled) return;
    Display view;
    if (!SDL_AtomicTryLock(&guard)) return;
    view = display; SDL_AtomicUnlock(&guard);
    char line[160];
    drawing_rect(8, 8, 850, 110, 0xE0101010);
    snprintf(line, sizeof(line), "PROFILE 30 FPS TARGET  %s CPU %u GPU %u EMC %u MHz", view.mode, view.clocks[0]/1000000, view.clocks[1]/1000000, view.clocks[2]/1000000);
    drawing_text(line, 16, 14, 1, 0xFFFFFFFF);
    snprintf(line, sizeof(line), "FPS %.1f AVG %.2f P99 %.2f WORST %.2f ms LATE %llu MISSED %llu", view.fps, view.avg, view.p99, view.worst, (unsigned long long)view.late, (unsigned long long)view.missed);
    drawing_text(line, 16, 38, 1, 0xFFFFFFFF);
    snprintf(line, sizeof(line), "UPDATE %.2f RENDER CPU %.2f WORK MAX %.2f NET %.1f ms/10s MEM %.1f MB", view.update, view.render, view.work, view.network, view.memory/1048576.0);
    drawing_text(line, 16, 62, 1, 0xFFFFFFFF);
    if (SDL_AtomicGet(&log_error)) {
        snprintf(line, sizeof(line), "performance.log OPEN FAILED errno=%d; GPU / AUDIO COUNTERS N/A", SDL_AtomicGet(&log_error));
        drawing_text(line, 16, 86, 1, 0xFFFFFFFF);
    } else drawing_text("GPU TIME / AUDIO UNDERRUNS N/A; LOG EVERY 10s; CLOCKS READ ONLY", 16, 86, 1, 0xFFFFFFFF);
}
#endif
