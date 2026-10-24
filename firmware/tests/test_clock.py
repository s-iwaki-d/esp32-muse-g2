"""Exercise production clock readiness with a controlled network, SNTP and RTC."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ClockReadiness(unittest.TestCase):
    def test_cold_boot_retained_time_retry_and_clock_loss(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "esp_log.h").write_text('''#pragma once
void test_log(const char *, const char *, ...);
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
''')
            (directory / "esp_timer.h").write_text('''#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
''')
            (directory / "esp_netif_sntp.h").write_text('''#pragma once
#include <stdbool.h>
#include <sys/time.h>
typedef int esp_err_t;
#define ESP_OK 0
const char *esp_err_to_name(int);
typedef struct {
    bool start, wait_for_sync;
    void (*sync_cb)(struct timeval *);
    const char *server;
} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(host) { .start = true, .wait_for_sync = true, .server = (host) }
int esp_netif_sntp_init(const esp_sntp_config_t *);
int esp_netif_sntp_start(void);
''')
            binary = str(directory / "test-clock")
            subprocess.run([
                *shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", temp, "-I", str(ROOT / "src"),
                str(ROOT / "src/bridge_clock.c"), str(ROOT / "tests/test_clock.c"), "-o", binary,
            ], check=True)
            for scenario in range(4):
                with self.subTest(scenario=scenario):
                    subprocess.run([binary, str(scenario)], check=True)


if __name__ == "__main__":
    unittest.main()
