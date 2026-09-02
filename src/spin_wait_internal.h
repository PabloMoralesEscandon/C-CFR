#ifndef CFR_SPIN_WAIT_INTERNAL_H
#define CFR_SPIN_WAIT_INTERNAL_H

#include <stddef.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sched.h>
#endif

enum { CFR_SPIN_BEFORE_YIELD = 64 };

static inline void cfr_cpu_relax(void) {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
#endif
}

static inline void cfr_spin_wait(size_t *spin_count) {
    cfr_cpu_relax();
    if (*spin_count < CFR_SPIN_BEFORE_YIELD) {
        *spin_count += 1;
        return;
    }
#if defined(_WIN32)
    (void)SwitchToThread();
#elif defined(__unix__) || defined(__APPLE__)
    (void)sched_yield();
#endif
}

#endif
