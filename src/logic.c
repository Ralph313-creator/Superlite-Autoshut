#include "logic.h"

as_action as_decide(const as_config *cfg, unsigned idle_ms, unsigned shown_ms, unsigned *secs_left) {
    unsigned left_ms;
    if (idle_ms < cfg->idle_ms)
        return AS_ACTIVE;
    if (shown_ms >= cfg->warn_ms)
        return AS_SHUTDOWN;
    left_ms = cfg->warn_ms - shown_ms;
    if (secs_left)
        *secs_left = (left_ms + 999u) / 1000u;
    return AS_WARN;
}

unsigned as_next_check_ms(const as_config *cfg, unsigned idle_ms) {
    unsigned wait;
    if (idle_ms >= cfg->idle_ms)
        return AS_WARN_TICK_MS;
    wait = cfg->idle_ms - idle_ms;
    return wait < AS_WARN_TICK_MS ? AS_WARN_TICK_MS : wait;
}

int as_should_beep(unsigned secs_left) {
    if (secs_left == 0)
        return 0;
    return secs_left <= 5 || secs_left % 10 == 0;
}

int as_parse_minutes(const wchar_t *s, unsigned *out) {
    unsigned v = 0;
    if (!s || !*s)
        return 0;
    for (; *s; s++) {
        if (*s < L'0' || *s > L'9')
            return 0;
        v = v * 10u + (unsigned)(*s - L'0');
        if (v > AS_MAX_MINUTES)
            return 0;
    }
    if (v < AS_MIN_MINUTES)
        return 0;
    *out = v;
    return 1;
}
