/*
 * Benchmark.ino — USB Ethernet speed test on M5Stack Tab5
 *
 * Replicates the exact same tests from the IDF phase2 speed suite
 * so results are directly comparable (Arduino vs IDF).
 *
 * Test 1: HTTP single-stream download (esp_http_client, 10 MB)
 * Test 2: 4× parallel raw TCP download (proven sweet spot)
 *
 * Endpoint: speed.cloudflare.com/__down (CDN-edge, serves N bytes of zeros)
 *
 * IDF Phase 2.3 baseline results (ASIX AX88179B):
 *   Test 1: ~6.78 Mbps single stream
 *   Test 2: ~13.00 Mbps 4× parallel
 *
 * Requires: USBEth library + patched M5Stack BSP
 */

#include <USBEth.h>

/* IDF headers needed for speed test */
extern "C" {
#include "esp_http_client.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
}

/* ── Test parameters (match IDF exactly) ─────────────────── */

#define SPEED_HOST      "speed.cloudflare.com"
#define SPEED_BYTES     10485760    /* 10 MB total */
#define SPEED_TEST_URL  "http://speed.cloudflare.com/__down?bytes=10485760"
#define RECV_BUF_SIZE   (32 * 1024) /* 32 KB recv buffer */
#define MAX_PARALLEL    8

/* ── DNS resolution cache ────────────────────────────────── */

static struct sockaddr_in s_speed_addr;
static bool s_addr_resolved = false;

static bool resolve_host() {
    if (s_addr_resolved) return true;
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(SPEED_HOST, "80", &hints, &res) != 0 || !res) {
        Serial.println("DNS resolve failed!");
        return false;
    }
    memcpy(&s_speed_addr, res->ai_addr, sizeof(s_speed_addr));
    freeaddrinfo(res);
    s_addr_resolved = true;
    Serial.printf("Resolved %s -> %s\n", SPEED_HOST,
                  inet_ntoa(s_speed_addr.sin_addr));
    return true;
}

/* ── Test 1: esp_http_client single stream ───────────────── */

static void speed_test_single() {
    Serial.println("[TEST1] esp_http_client single stream, 10 MB");

    esp_http_client_config_t cfg = {};
    cfg.url = SPEED_TEST_URL;
    cfg.timeout_ms = 60000;
    cfg.buffer_size = RECV_BUF_SIZE;
    cfg.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { Serial.println("  HTTP init failed"); return; }

    esp_http_client_open(client, 0);
    esp_http_client_fetch_headers(client);

    char *buf = (char *)heap_caps_malloc(RECV_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(RECV_BUF_SIZE);
    if (!buf) { Serial.println("  malloc failed"); esp_http_client_cleanup(client); return; }

    size_t total = 0;
    int64_t t0 = esp_timer_get_time();
    int n;
    while ((n = esp_http_client_read(client, buf, RECV_BUF_SIZE)) > 0) {
        total += n;
    }
    int64_t elapsed = esp_timer_get_time() - t0;

    double secs = elapsed / 1e6;
    double mbps = (total * 8.0 / 1e6) / secs;

    Serial.println("╔══════════════════════════════════════════════╗");
    Serial.println("║  TEST 1: Single stream (esp_http_client)     ║");
    Serial.printf( "║    Bytes:   %u\n", (unsigned)total);
    Serial.printf( "║    Time:    %.2f s\n", secs);
    Serial.printf( "║    Speed:   %.2f Mbps  (%.2f MB/s)\n", mbps, total / 1e6 / secs);
    Serial.println("╚══════════════════════════════════════════════╝");

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

/* ── Raw TCP download helper ─────────────────────────────── */

static size_t raw_tcp_download(int bytes_wanted, char *buf, size_t bufsz) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return 0;

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    int rcvbuf = 65535;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct timeval tv = { .tv_sec = 30 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&s_speed_addr, sizeof(s_speed_addr)) < 0) {
        close(sock);
        return 0;
    }

    char req[256];
    int rlen = snprintf(req, sizeof(req),
        "GET /__down?bytes=%d HTTP/1.1\r\n"
        "Host: %s\r\nConnection: close\r\n\r\n",
        bytes_wanted, SPEED_HOST);
    send(sock, req, rlen, 0);

    /* Skip HTTP headers */
    size_t total = 0;
    bool hdr_done = false;
    while (!hdr_done) {
        int n = recv(sock, buf, bufsz, 0);
        if (n <= 0) { close(sock); return 0; }
        for (int i = 0; i < n - 3; i++) {
            if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') {
                total = n - (i + 4);
                hdr_done = true;
                break;
            }
        }
    }

    /* Read body */
    while (total < (size_t)bytes_wanted) {
        int n = recv(sock, buf, bufsz, 0);
        if (n <= 0) break;
        total += n;
    }
    close(sock);
    return total;
}

/* ── Test 2: parallel raw TCP ────────────────────────────── */

typedef struct {
    int id;
    int bytes_wanted;
    size_t got;
    TaskHandle_t caller;
} dl_arg_t;

static void dl_worker(void *arg) {
    dl_arg_t *a = (dl_arg_t *)arg;
    char *buf = (char *)heap_caps_malloc(RECV_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(RECV_BUF_SIZE);
    if (buf) {
        a->got = raw_tcp_download(a->bytes_wanted, buf, RECV_BUF_SIZE);
        free(buf);
    }
    Serial.printf("  Worker %d: %u bytes\n", a->id, (unsigned)a->got);
    xTaskNotifyGive(a->caller);
    vTaskDelete(NULL);
}

static void speed_test_parallel(int num_conn) {
    int per_conn = SPEED_BYTES / num_conn;
    Serial.printf("[TEST2] %dx raw TCP, %d bytes each\n", num_conn, per_conn);

    dl_arg_t args[MAX_PARALLEL];
    int64_t t0 = esp_timer_get_time();

    for (int i = 0; i < num_conn; i++) {
        args[i].id = i;
        args[i].bytes_wanted = per_conn;
        args[i].got = 0;
        args[i].caller = xTaskGetCurrentTaskHandle();
        xTaskCreate(dl_worker, "dl", 6144, &args[i], 4, NULL);
    }
    for (int i = 0; i < num_conn; i++) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000));
    }

    int64_t elapsed = esp_timer_get_time() - t0;
    size_t total = 0;
    for (int i = 0; i < num_conn; i++) total += args[i].got;

    double secs = elapsed / 1e6;
    double mbps = (total * 8.0 / 1e6) / secs;
    Serial.println("╔══════════════════════════════════════════════╗");
    Serial.printf( "║  TEST 2: %dx parallel raw TCP                ║\n", num_conn);
    Serial.printf( "║    Bytes:   %u\n", (unsigned)total);
    Serial.printf( "║    Time:    %.2f s\n", secs);
    Serial.printf( "║    Speed:   %.2f Mbps  (%.2f MB/s)\n", mbps, total / 1e6 / secs);
    Serial.println("╚══════════════════════════════════════════════╝");
}

/* ── Speed test orchestrator (runs as FreeRTOS task) ─────── */

static void speed_test_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (!resolve_host()) {
        Serial.println("Cannot resolve speed host, aborting tests");
        vTaskDelete(NULL);
        return;
    }

    Serial.println();
    Serial.println("====================================");
    Serial.println("  USBEth SPEED TEST SUITE (Arduino)");
    Serial.println("====================================");
    Serial.println();

    /* Test 1: single stream HTTP */
    speed_test_single();
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Test 2: 4× parallel raw TCP (proven sweet spot from IDF testing) */
    speed_test_parallel(4);

    Serial.println();
    Serial.println("====================================");
    Serial.println("  ALL TESTS DONE");
    Serial.println("====================================");
    Serial.println();
    Serial.println("IDF Phase 2.3 reference (ASIX AX88179B):");
    Serial.println("  Test 1: 6.78 Mbps  (single stream)");
    Serial.println("  Test 2: 13.00 Mbps (4x parallel)");

    vTaskDelete(NULL);
}

/* ── Event callback ──────────────────────────────────────── */

void onEthEvent(usb_eth_event_t event, void *arg) {
    switch (event) {
        case USB_ETH_EVENT_GOT_IP:
            Serial.println("[ETH] Got IP!");
            break;
        case USB_ETH_EVENT_LOST_IP:
            Serial.println("[ETH] Lost IP");
            break;
        default:
            break;
    }
}

/* ── Arduino setup/loop ──────────────────────────────────── */

void setup() {
    delay(2000);
    Serial.begin(115200);
    Serial.println("\n=== USBEth Benchmark ===\n");

    USBEth.onEvent(onEthEvent);
    USBEth.setHostname("tab5-bench");

    if (!USBEth.begin()) {
        Serial.println("USBEth.begin() FAILED");
        while (1) delay(1000);
    }

    Serial.println("Waiting for dongle + DHCP...");
    uint32_t start = millis();
    while (!USBEth.hasIP()) {
        if (millis() - start > 30000) {
            Serial.println("Timeout. Check dongle + cable.");
            while (1) delay(1000);
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    Serial.println("--- Connected ---");
    Serial.printf("  IP:      %s\n", USBEth.localIP().toString().c_str());
    Serial.printf("  Gateway: %s\n", USBEth.gatewayIP().toString().c_str());
    Serial.printf("  MAC:     %s\n", USBEth.macAddress().c_str());
    Serial.println("-----------------\n");

    /* Launch speed test in a separate task (pri 3 — below USB host task) */
    xTaskCreate(speed_test_task, "speed_test", 12288, NULL, 3, NULL);
}

void loop() {
    delay(60000);  /* Nothing to do — speed test runs as its own task */
}
