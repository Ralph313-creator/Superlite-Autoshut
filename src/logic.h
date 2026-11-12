/* Superlite Autoshut - pure decision logic (no Windows calls, unit tested). */
#ifndef AUTOSHUT_LOGIC_H
#define AUTOSHUT_LOGIC_H

#include <stddef.h> /* wchar_t, NULL */

typedef enum {
    AS_ACTIVE = 0,   /* someone used the PC recently: no warning */
    AS_WARN = 1,     /* idle time reached: show the countdown */
    AS_SHUTDOWN = 2  /* countdown finished: shut the PC down */
} as_action;

typedef struct {
    unsigned idle_ms; /* how long with no input before the warning shows */
    unsigned warn_ms; /* how long the warning counts down */
} as_config;

/* Decide what to do. idle_ms = time since the last keyboard/mouse input;
   shown_ms = how long the warning has been on screen (0 if not shown).
   The PC only shuts down after the full warning was shown, even if it was
   idle long before. secs_left (may be NULL) gets the countdown seconds. */
as_action as_decide(const as_config *cfg, unsigned idle_ms, unsigned shown_ms, unsigned *secs_left);

/* How long to sleep before the next check. While nobody touches the PC
   the idle time only grows, so it is safe to sleep until the warning is due. */
unsigned as_next_check_ms(const as_config *cfg, unsigned idle_ms);

/* 1 if the warning should beep when the countdown shows secs_left. */
int as_should_beep(unsigned secs_left);

/* Parse a minutes argument ("10"). Returns 1 and sets *out for 1..1440. */
int as_parse_minutes(const wchar_t *s, unsigned *out);

#define AS_MIN_MINUTES 1u
#define AS_MAX_MINUTES 1440u
#define AS_DEFAULT_MINUTES 10u
#define AS_DEFAULT_WARN_SECONDS 60u
#define AS_WARN_TICK_MS 200u

#endif
