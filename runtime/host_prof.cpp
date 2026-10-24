// --host-sample A B (Windows): samples the emulation thread's host instruction pointer about
// every 0.25 ms during frames A..B and writes "offset count" lines (offsets from the image
// base, hottest first) to host_samples.txt; tools/host_profile.sh symbolizes them to source
// lines, which works for generated code too (funcs_*.c / flat_regions.c).
#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <vector>

namespace psx {

static std::unordered_map<uint64_t, uint64_t> samples;
static std::atomic<bool> stop{false};
static HANDLE target = nullptr;

void host_prof_start(const volatile uint64_t* frame, uint64_t from, uint64_t to) {
    if (!to) return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    timeBeginPeriod(1);
    std::thread([frame, from, to] {
        uint64_t base = (uint64_t)GetModuleHandleA(nullptr);
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::microseconds(250));
            uint64_t f = *frame;
            if (f < from || f >= to) continue;
            if (SuspendThread(target) == (DWORD)-1) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(target, &ctx)) samples[ctx.Rip - base]++;
            ResumeThread(target);
        }
    }).detach();
}

void host_prof_report() {
    if (!target) return;
    stop = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::vector<std::pair<uint64_t, uint64_t>> v;
    uint64_t total = 0;
    for (auto& [off, n] : samples) { v.push_back({n, off}); total += n; }
    std::sort(v.rbegin(), v.rend());
    FILE* f = fopen("host_samples.txt", "w");
    if (!f) return;
    fprintf(f, "# %llu samples\n", (unsigned long long)total);
    for (auto& [n, off] : v) fprintf(f, "%llx %llu\n", (unsigned long long)off, (unsigned long long)n);
    fclose(f);
}

}  // namespace psx
#else
#include <cstdint>
namespace psx {
void host_prof_start(const volatile uint64_t*, uint64_t, uint64_t) {}
void host_prof_report() {}
}  // namespace psx
#endif
