#include "bridge_clock.h"

#include <stdint.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"

#define CLOCK_MIN_UNIX_SECONDS 1700000000LL
#define CLOCK_RETRY_US (60 * 1000000LL)

static const char *TAG = "bridge_clock";
static bool s_started;
static bool s_synced;
static int64_t s_retry_at;

static void time_synced(struct timeval *tv)
{
    bool valid = tv && tv->tv_sec >= CLOCK_MIN_UNIX_SECONDS;
    __atomic_store_n(&s_synced, valid, __ATOMIC_RELEASE);
    if (valid) {
        ESP_LOGI(TAG, "Network time synchronized (Unix %lld)", (long long)tv->tv_sec);
    }
}

bool bridge_clock_ready(bool network_ready)
{
    if (!network_ready) return false;

    int64_t now = esp_timer_get_time();
    if (!s_started && now >= s_retry_at) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com");
        config.wait_for_sync = false;  /* callback, no extra semaphore/task */
        config.sync_cb = time_synced;
        esp_err_t err = esp_netif_sntp_init(&config);
        s_started = err == ESP_OK;
        s_retry_at = now + CLOCK_RETRY_US;
        if (s_started) {
            ESP_LOGI(TAG, "Synchronizing network time before opening the bridge");
        } else {
            ESP_LOGW(TAG, "Network time initialization failed: %s; retrying", esp_err_to_name(err));
        }
    }

    bool ready = s_started && __atomic_load_n(&s_synced, __ATOMIC_ACQUIRE)
                 && time(NULL) >= CLOCK_MIN_UNIX_SECONDS;
    if (s_started && !ready && now >= s_retry_at) {
        /* Retry DNS/UDP after an unavailable network without waiting through
         * SNTP's potentially long exponential backoff. Keep the bridge closed. */
        esp_err_t err = esp_netif_sntp_start();
        s_retry_at = now + CLOCK_RETRY_US;
        ESP_LOGW(TAG, "Network time not synchronized; retry: %s", esp_err_to_name(err));
    }
    return ready;
}
