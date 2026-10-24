#include "bridge_clock.h"
#include "esp_netif_sntp.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t now_us;
static time_t wall_time;
static int init_calls, restart_calls, init_result;
static void (*notify_sync)(struct timeval *);

int64_t esp_timer_get_time(void) { return now_us; }
time_t time(time_t *out) { if (out) *out = wall_time; return wall_time; }
const char *esp_err_to_name(int err) { return err ? "test failure" : "ESP_OK"; }
void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }

int esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    init_calls++;
    assert(config->start && !config->wait_for_sync && config->sync_cb);
    assert(config->server && *config->server);
    if (!init_result) notify_sync = config->sync_cb;
    return init_result;
}
int esp_netif_sntp_start(void) { restart_calls++; return 0; }

static void synchronized(time_t seconds)
{
    wall_time = seconds;
    struct timeval value = { .tv_sec = seconds };
    assert(notify_sync);
    notify_sync(&value);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: /* Cold power-on: no bridge until a fresh valid synchronization. */
        assert(!bridge_clock_ready(false));
        assert(init_calls == 0);
        assert(!bridge_clock_ready(true));
        assert(init_calls == 1);
        synchronized(100);
        assert(!bridge_clock_ready(true));
        synchronized(1792852800);
        assert(bridge_clock_ready(true));
        assert(!bridge_clock_ready(false));
        assert(bridge_clock_ready(true));
        assert(init_calls == 1);
        break;
    case 1: /* A plausible retained RTC is insufficient after a reboot. */
        wall_time = 1792852800;
        assert(!bridge_clock_ready(true));
        assert(!bridge_clock_ready(true));
        synchronized(wall_time);
        assert(bridge_clock_ready(true));
        break;
    case 2: /* Initialization failure and unreachable server recover later. */
        init_result = 1;
        assert(!bridge_clock_ready(true));
        init_result = 0;
        now_us = 59999999;
        assert(!bridge_clock_ready(true));
        assert(init_calls == 1);
        now_us++;
        assert(!bridge_clock_ready(true));
        assert(init_calls == 2);
        now_us += 60000000;
        assert(!bridge_clock_ready(true));
        assert(restart_calls == 1);
        for (int i = 0; i < 100; i++) assert(!bridge_clock_ready(true));
        assert(restart_calls == 1);
        synchronized(1792852800);
        assert(bridge_clock_ready(true));
        break;
    case 3: /* Loss of valid wall time closes readiness until recovery. */
        assert(!bridge_clock_ready(true));
        synchronized(1792852800);
        assert(bridge_clock_ready(true));
        wall_time = 0;
        assert(!bridge_clock_ready(true));
        synchronized(1792852860);
        assert(bridge_clock_ready(true));
        now_us += 3600000000LL;
        assert(bridge_clock_ready(true));
        assert(restart_calls == 0);
        break;
    default:
        return 1;
    }
    puts("clock regression passed");
    return 0;
}
