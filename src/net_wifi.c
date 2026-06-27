#include "include/net_wifi.h"
#include "wifi_config.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

// ─── WiFi connection timeout ──────────────────────────────────────────────────
// After this many milliseconds without a successful connection, stop retrying
// and continue with the memory filesystem backend.
#define WIFI_TIMEOUT_MS 15000

// ── State ─────────────────────────────────────────────────────────────────────

static bool     wifi_done        = false;  // true once connected or timed out
static uint32_t wifi_start_ms    = 0;
static uint32_t wifi_last_poll   = 0;

// ── API ───────────────────────────────────────────────────────────────────────

void net_wifi_start(void)
{
    printf("[WiFi] Starting (SSID: %s)...\n", WIFI_SSID);
    cyw43_arch_enable_sta_mode();
    int err = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    if (err) {
        printf("[WiFi] Failed to start connection (err=%d)\n", err);
        wifi_done = true;
        return;
    }
    wifi_start_ms  = to_ms_since_boot(get_absolute_time());
    wifi_last_poll = wifi_start_ms;
}

void net_wifi_poll(void)
{
    if (wifi_done) return;

    // Rate-limit to once per second
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - wifi_last_poll < 1000) return;
    wifi_last_poll = now;

    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

    if (status == CYW43_LINK_UP) {
        wifi_done = true;
        struct netif *n = netif_list;
        if (n)
            printf("[WiFi] Connected, IP: %s\n", ip4addr_ntoa(netif_ip4_addr(n)));
        else
            printf("[WiFi] Connected (IP not yet assigned)\n");
        return;
    }

    if (status < 0) {
        wifi_done = true;
        printf("[WiFi] Failed (status=%d) — continuing with memory backend\n", status);
        return;
    }

    // Still trying — check timeout
    if (now - wifi_start_ms >= WIFI_TIMEOUT_MS) {
        wifi_done = true;
        printf("[WiFi] Timeout after %d s — continuing with memory backend\n",
               WIFI_TIMEOUT_MS / 1000);
    }
}
