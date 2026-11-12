#include <stdio.h>
#include <wchar.h>
#include "../src/logic.h"

static int failed = 0, run = 0;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const as_config cfg = { 10u * 60u * 1000u, 60u * 1000u };

static void test_active_before_idle_time(void) {
    CHECK(as_decide(&cfg, 0, 0, NULL) == AS_ACTIVE);
    CHECK(as_decide(&cfg, cfg.idle_ms - 1, 0, NULL) == AS_ACTIVE);
}

static void test_input_cancels_a_running_warning(void) {
    CHECK(as_decide(&cfg, 100, 30000, NULL) == AS_ACTIVE);
}

static void test_warns_when_idle_time_reached(void) {
    unsigned left = 0;
    CHECK(as_decide(&cfg, cfg.idle_ms, 0, &left) == AS_WARN);
    CHECK(left == 60);
}

static void test_full_warning_even_when_idle_far_past_the_limit(void) {
    /* helper restarted (killed, settings changed) on a PC idle for hours:
       it must still show the whole countdown, not shut down at once */
    unsigned left = 0;
    CHECK(as_decide(&cfg, 0xFFFFFFFFu, 0, &left) == AS_WARN);
    CHECK(left == 60);
}

static void test_countdown_follows_time_shown(void) {
    unsigned left = 0;
    as_decide(&cfg, cfg.idle_ms + 500, 500, &left);
    CHECK(left == 60);
    as_decide(&cfg, cfg.idle_ms + 1000, 1000, &left);
    CHECK(left == 59);
    as_decide(&cfg, cfg.idle_ms + cfg.warn_ms - 1, cfg.warn_ms - 1, &left);
    CHECK(left == 1);
}

static void test_shuts_down_when_countdown_ends(void) {
    CHECK(as_decide(&cfg, cfg.idle_ms + cfg.warn_ms, cfg.warn_ms, NULL) == AS_SHUTDOWN);
    CHECK(as_decide(&cfg, 0xFFFFFFFFu, cfg.warn_ms + 5000, NULL) == AS_SHUTDOWN);
}

static void test_sleeps_until_warning_is_due(void) {
    CHECK(as_next_check_ms(&cfg, 0) == cfg.idle_ms);
    CHECK(as_next_check_ms(&cfg, cfg.idle_ms - 3000) == 3000);
}

static void test_short_sleeps_while_warning(void) {
    CHECK(as_next_check_ms(&cfg, cfg.idle_ms) == AS_WARN_TICK_MS);
    CHECK(as_next_check_ms(&cfg, cfg.idle_ms + 30000) == AS_WARN_TICK_MS);
}

static void test_never_sleeps_too_little_or_too_much(void) {
    /* 1 ms before the warning: still sleep a sane minimum, not a busy loop */
    CHECK(as_next_check_ms(&cfg, cfg.idle_ms - 1) == AS_WARN_TICK_MS);
}

static void test_beeps_every_ten_seconds_then_every_second(void) {
    CHECK(as_should_beep(60));
    CHECK(as_should_beep(50));
    CHECK(!as_should_beep(59));
    CHECK(!as_should_beep(6));
    CHECK(as_should_beep(5));
    CHECK(as_should_beep(1));
    CHECK(!as_should_beep(0));
}

static void test_parse_minutes(void) {
    unsigned m = 0;
    CHECK(as_parse_minutes(L"10", &m) && m == 10);
    CHECK(as_parse_minutes(L"1", &m) && m == 1);
    CHECK(as_parse_minutes(L"1440", &m) && m == 1440);
    CHECK(!as_parse_minutes(L"0", &m));
    CHECK(!as_parse_minutes(L"1441", &m));
    CHECK(!as_parse_minutes(L"", &m));
    CHECK(!as_parse_minutes(L"abc", &m));
    CHECK(!as_parse_minutes(L"10x", &m));
    CHECK(!as_parse_minutes(L"-5", &m));
    CHECK(!as_parse_minutes(L"99999999999", &m));
    CHECK(!as_parse_minutes(NULL, &m));
}

static void test_parse_power_action(void) {
    CHECK(as_parse_power(L"shutdown") == AS_POWER_SHUTDOWN);
    CHECK(as_parse_power(L"restart") == AS_POWER_RESTART);
    CHECK(as_parse_power(L"Restart") == AS_POWER_RESTART);
    CHECK(as_parse_power(L"SLEEP") == AS_POWER_SLEEP);
    CHECK(as_parse_power(L"hibernate") == AS_POWER_HIBERNATE);
}

static void test_unknown_power_action_means_shutdown(void) {
    CHECK(as_parse_power(L"") == AS_POWER_SHUTDOWN);
    CHECK(as_parse_power(L"reboot please") == AS_POWER_SHUTDOWN);
    CHECK(as_parse_power(L"sleepy") == AS_POWER_SHUTDOWN);
    CHECK(as_parse_power(NULL) == AS_POWER_SHUTDOWN);
}

static void test_power_names_round_trip(void) {
    CHECK(as_parse_power(as_power_name(AS_POWER_SHUTDOWN)) == AS_POWER_SHUTDOWN);
    CHECK(as_parse_power(as_power_name(AS_POWER_RESTART)) == AS_POWER_RESTART);
    CHECK(as_parse_power(as_power_name(AS_POWER_SLEEP)) == AS_POWER_SLEEP);
    CHECK(as_parse_power(as_power_name(AS_POWER_HIBERNATE)) == AS_POWER_HIBERNATE);
}

static void test_default_message_names_the_action(void) {
    CHECK(wcsstr(as_default_message(AS_POWER_SHUTDOWN), L"shut down") != NULL);
    CHECK(wcsstr(as_default_message(AS_POWER_RESTART), L"restart") != NULL);
    CHECK(wcsstr(as_default_message(AS_POWER_SLEEP), L"sleep") != NULL);
    CHECK(wcsstr(as_default_message(AS_POWER_HIBERNATE), L"hibernate") != NULL);
}

static void test_setting_in_range_or_default(void) {
    CHECK(as_in_range_or(15, 1, 1440, 10) == 15);
    CHECK(as_in_range_or(1, 1, 1440, 10) == 1);
    CHECK(as_in_range_or(1440, 1, 1440, 10) == 1440);
    CHECK(as_in_range_or(0, 1, 1440, 10) == 10);
    CHECK(as_in_range_or(-3, 1, 1440, 10) == 10);
    CHECK(as_in_range_or(5000, 1, 1440, 10) == 10);
}

int main(void) {
    test_parse_power_action();
    test_unknown_power_action_means_shutdown();
    test_power_names_round_trip();
    test_default_message_names_the_action();
    test_setting_in_range_or_default();
    test_active_before_idle_time();
    test_input_cancels_a_running_warning();
    test_warns_when_idle_time_reached();
    test_full_warning_even_when_idle_far_past_the_limit();
    test_countdown_follows_time_shown();
    test_shuts_down_when_countdown_ends();
    test_sleeps_until_warning_is_due();
    test_short_sleeps_while_warning();
    test_never_sleeps_too_little_or_too_much();
    test_beeps_every_ten_seconds_then_every_second();
    test_parse_minutes();
    printf("%d/%d checks passed\n", run - failed, run);
    return failed ? 1 : 0;
}
