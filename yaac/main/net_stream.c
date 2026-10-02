#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "net_stream.h"
#include "wifi_credentials.h"

static const char *TAG = "net_stream";

#define QUEUE_DEPTH     16      /* ~0.5 s of audio at 512 samples per packet */

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint8_t  version;
    uint8_t  flags;
    uint16_t samples;
    uint32_t session;
    uint32_t seq;
    uint32_t sample_rate;
    int16_t  pcm[NET_STREAM_MAX_SAMPLES];
} packet_t;

static QueueHandle_t queue;
static volatile bool connected;
static char ip_str[16], bcast_str[16];
static volatile uint32_t bcast_addr;    /* subnet broadcast, network byte order */
static volatile bool active;
static uint32_t session, seq, rate;
static bool first;

/* ---- Wi-Fi --------------------------------------------------------------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (connected) ESP_LOGW(TAG, "Wi-Fi lost (reason %d), reconnecting", d->reason);
        connected = false;
        ip_str[0] = 0;
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        /* Directed broadcast of our subnet, e.g. 192.168.0.255 for a /24 */
        esp_ip4_addr_t b = {.addr = e->ip_info.ip.addr | ~e->ip_info.netmask.addr};
        snprintf(bcast_str, sizeof(bcast_str), IPSTR, IP2STR(&b));
        bcast_addr = b.addr;
        connected = true;
        ESP_LOGI(TAG, "Wi-Fi connected to \"%s\", IP %s; streaming UDP broadcast to %s:%d",
                 YAAC_WIFI_SSID, ip_str, bcast_str, NET_STREAM_PORT);
    }
}

/* ---- Sender -------------------------------------------------------------- */

static void sender_task(void *arg)
{
    static packet_t pkt;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const int on = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0) {
        ESP_LOGE(TAG, "SO_BROADCAST failed (errno %d)", errno);
    }
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(NET_STREAM_PORT),
    };
    uint32_t sent = 0, failed = 0;
    int last_errno = 0;
    for (;;) {
        if (xQueueReceive(queue, &pkt, portMAX_DELAY) != pdTRUE) continue;
        if (!connected) continue;
        dst.sin_addr.s_addr = bcast_addr;       /* follows the current subnet */
        const size_t len = offsetof(packet_t, pcm) + pkt.samples * sizeof(int16_t);
        if (sendto(sock, &pkt, len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            failed++;
            last_errno = errno;
        } else {
            sent++;
        }
        if (pkt.flags & NET_FLAG_END) {
            ESP_LOGI(TAG, "Session %lu streamed to %s:%d: %lu packets, %lu failed (last errno %d)",
                     (unsigned long)pkt.session, bcast_str, NET_STREAM_PORT, (unsigned long)sent,
                     (unsigned long)failed, failed ? last_errno : 0);
            sent = failed = 0;
        }
    }
}

static void enqueue(const int16_t *samples, size_t n, uint8_t flags)
{
    static packet_t pkt;    /* only called from the audio fetch task */
    memcpy(pkt.magic, "YAAC", 4);
    pkt.version = 1;
    pkt.flags = flags;
    pkt.samples = (uint16_t)n;
    pkt.session = session;
    pkt.seq = seq++;
    pkt.sample_rate = rate;
    if (n) memcpy(pkt.pcm, samples, n * sizeof(int16_t));
    xQueueSend(queue, &pkt, 0);     /* drop rather than stall the audio path */
}

/* ---- API ----------------------------------------------------------------- */

void net_stream_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, YAAC_WIFI_SSID, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, YAAC_WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);      /* steady low-latency streaming */

    /* ~17 KB of packets: keep them in PSRAM, internal RAM is needed by Wi-Fi and the AFE */
    queue = xQueueCreateWithCaps(QUEUE_DEPTH, sizeof(packet_t), MALLOC_CAP_SPIRAM);
    assert(queue);
    if (xTaskCreatePinnedToCoreWithCaps(sender_task, "net_tx", 4096, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start sender task");
        abort();
    }
    ESP_LOGI(TAG, "Connecting to \"%s\"... (free internal RAM %u bytes)", YAAC_WIFI_SSID,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool net_stream_connected(void) { return connected; }
const char *net_stream_ip(void) { return ip_str; }
bool net_stream_active(void) { return active; }

void net_stream_begin(uint32_t sample_rate)
{
    session++;
    seq = 0;
    rate = sample_rate;
    first = true;
    active = true;
}

void net_stream_push(const int16_t *samples, size_t n)
{
    if (!active) return;
    while (n) {
        const size_t chunk = n < NET_STREAM_MAX_SAMPLES ? n : NET_STREAM_MAX_SAMPLES;
        enqueue(samples, chunk, first ? NET_FLAG_START : 0);
        first = false;
        samples += chunk;
        n -= chunk;
    }
}

void net_stream_end(void)
{
    if (!active) return;
    enqueue(NULL, 0, NET_FLAG_END | (first ? NET_FLAG_START : 0));
    active = false;
}
