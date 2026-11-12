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

static const wchar_t *const power_names[] = { L"shutdown", L"restart", L"sleep", L"hibernate" };

static int same_word(const wchar_t *a, const wchar_t *b) { /* ASCII, ignores case */
    for (; *a && *b; a++, b++) {
        wchar_t x = (*a >= L'A' && *a <= L'Z') ? (wchar_t)(*a + 32) : *a;
        wchar_t y = (*b >= L'A' && *b <= L'Z') ? (wchar_t)(*b + 32) : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

as_power as_parse_power(const wchar_t *s) {
    int i;
    if (!s)
        return AS_POWER_SHUTDOWN;
    for (i = 0; i < 4; i++)
        if (same_word(s, power_names[i]))
            return (as_power)i;
    return AS_POWER_SHUTDOWN;
}

const wchar_t *as_power_name(as_power p) {
    return (unsigned)p < 4u ? power_names[p] : power_names[0];
}

const wchar_t *as_default_message(as_power p) {
    switch (p) {
    case AS_POWER_RESTART:   return L"This PC will restart. Move the mouse or press any key to cancel.";
    case AS_POWER_SLEEP:     return L"This PC will go to sleep. Move the mouse or press any key to cancel.";
    case AS_POWER_HIBERNATE: return L"This PC will hibernate. Move the mouse or press any key to cancel.";
    default:                 return L"This PC will shut down. Move the mouse or press any key to cancel.";
    }
}

int as_in_range_or(int v, int lo, int hi, int def) {
    return (v >= lo && v <= hi) ? v : def;
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
