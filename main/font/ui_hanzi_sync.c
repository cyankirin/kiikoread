/* SPDX-License-Identifier: Apache-2.0
 * 中文：见 ui_hanzi_sync.h。下载沿用 OTA 白名单与在线字体的网络租用模式：
 *       复用已连接的 STA，否则在空闲时临时连接已保存的 WiFi，结束恢复原状。
 * English: See ui_hanzi_sync.h. Downloading follows the OTA whitelist and the online-font
 *       network lease pattern: reuse a connected STA, else temporarily connect the saved
 *       WiFi while idle, restoring the previous state afterwards.
 */
#include "ui_hanzi_sync.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "book_store.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_online.h"
#include "read_pico_transfer.h"
#include "ui_hanzi.h"

#define FONT_DIR "/flash/fonts"
#define FONT_PART FONT_DIR "/.ui-hanzi.part"
#define FONT_BLOCK 8192u
// 版本对应的分发文件名；升版时与 flash/fonts/ 一并更新，URL 须通过 OTA 白名单。
// Version-pinned distribution name; update together with flash/fonts/ on releases, and the
// URL must pass the OTA whitelist.
#define UI_HANZI_FONT_URL "https://cyankirin.github.io/kiikoread/fonts/ui-hanzi-0.3.3-rc117.01.bin"

static atomic_bool s_running;
static const char *TAG = "ui_hanzi_sync";

static bool make_directory(const char *path) {
    struct stat st;
    return !stat(path, &st) ? S_ISDIR(st.st_mode) : (!mkdir(path, 0755) || errno == EEXIST);
}

static bool ensure_network(bool *owned) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready &&
        status.state != READ_PICO_TRANSFER_UPLOADING) return true;
    if (status.state != READ_PICO_TRANSFER_STOPPED) return false;
    char name[33];
    bool saved = false;
    if (read_pico_transfer_get_saved_wifi(name, &saved) != ESP_OK || !saved) return false;
    read_pico_transfer_cfg_t config = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
    if (read_pico_transfer_start(&config) != ESP_OK) return false;
    *owned = true;
    int64_t limit = esp_timer_get_time() + 30000000;
    while (esp_timer_get_time() < limit) {
        read_pico_transfer_service_poll();
        read_pico_transfer_get_status(&status);
        if (status.network_ready) return true;
        if (status.state == READ_PICO_TRANSFER_ERROR) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

// 重定向只接受同样通过 OTA 白名单的 HTTPS 地址；不跳向任意主机。
// Redirects must pass the same OTA whitelist; never hop to an arbitrary host.
typedef struct { char location[256]; bool overflow; } font_redirect_t;
static esp_err_t font_header_event(esp_http_client_event_t *event) {
    font_redirect_t *r = event->user_data;
    if (r && event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        !strcasecmp(event->header_key, "Location")) {
        size_t n = strlen(event->header_value);
        r->overflow = n >= sizeof(r->location);
        if (!r->overflow) memcpy(r->location, event->header_value, n + 1);
    }
    return ESP_OK;
}
static esp_http_client_handle_t open_font(const char *url) {
    char current[256];
    esp_http_client_handle_t client = NULL;
    snprintf(current, sizeof(current), "%s", url);
    for (unsigned hops = 0; hops < 5; ++hops) {
        if (!pico_release_url_valid(current)) break;
        font_redirect_t redirect = {0};
        esp_http_client_config_t config = {.url = current, .timeout_ms = 10000, .buffer_size = 2048,
            .buffer_size_tx = 1024, .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true, .event_handler = font_header_event, .user_data = &redirect};
        client = esp_http_client_init(&config);
        if (!client) break;
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        esp_http_client_set_header(client, "User-Agent", PICO_OTA_CAPABILITY);
        if (esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0) goto failed;
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            esp_http_client_set_user_data(client, NULL);  // 后续读体事件不再触碰栈上的重定向缓冲。
            return client;
        }
        if ((status == 301 || status == 302 || status == 307 || status == 308) &&
            !redirect.overflow && redirect.location[0]) {
            snprintf(current, sizeof(current), "%s", redirect.location);
            esp_http_client_cleanup(client);
            client = NULL;
            continue;
        }
failed:
        esp_http_client_cleanup(client);
        return NULL;
    }
    esp_http_client_cleanup(client);
    return NULL;
}

static esp_err_t download_font(void) {
    esp_http_client_handle_t client = open_font(UI_HANZI_FONT_URL);
    if (!client) return ESP_FAIL;
    uint8_t *buffer = heap_caps_malloc(FONT_BLOCK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    FILE *file = NULL;
    esp_err_t result = ESP_FAIL;
    int64_t length = esp_http_client_get_content_length(client);
    if (!buffer || (length > 0 && (uint64_t)length != UI_HANZI_FONT_BYTES)) goto done;
    file = fopen(FONT_PART, "wb");
    if (!file) goto done;
    uint32_t received = 0;
    int64_t idle = esp_timer_get_time();
    while (received < UI_HANZI_FONT_BYTES) {
        int n = esp_http_client_read(client, (char *)buffer, FONT_BLOCK);
        if (n == -ESP_ERR_HTTP_EAGAIN && esp_timer_get_time() - idle < 20000000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (n <= 0) {
            if (!n && esp_http_client_is_complete_data_received(client)) break;
            if (esp_timer_get_time() - idle > 20000000) goto done;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if ((uint32_t)n > UI_HANZI_FONT_BYTES - received) goto done;
        if (fwrite(buffer, 1, (size_t)n, file) != (size_t)n) goto done;
        received += (uint32_t)n;
        idle = esp_timer_get_time();
    }
    if (received != UI_HANZI_FONT_BYTES) goto done;
    if (fflush(file) || fsync(fileno(file))) goto done;
    if (fclose(file)) { file = NULL; goto done; }
    file = NULL;
    // 先完整校验临时文件，再原子改名，绝不发布半截数据。
    // Verify the temp file fully, then rename atomically; never publish partial data.
    result = ui_hanzi_font_file_valid(FONT_PART) ? ESP_OK : ESP_FAIL;
done:
    if (file) fclose(file);
    if (result != ESP_OK) unlink(FONT_PART);
    esp_http_client_cleanup(client);
    heap_caps_free(buffer);
    if (result == ESP_OK) ESP_LOGI(TAG, "external supplement downloaded (%u bytes)", (unsigned)received);
    else ESP_LOGW(TAG, "external supplement download failed: %s", esp_err_to_name(result));
    return result;
}

static void worker(void *arg) {
    (void)arg;
    if (book_store_prepare_flash() == ESP_OK && make_directory(FONT_DIR) && !ui_hanzi_source_refresh()) {
        // 外置补字缺失或校验不符：空闲时经 WiFi 拉取；失败静默保持 PIF1 降级。
        // Supplement missing or failing verification: fetch over WiFi while idle; failures
        // silently keep the PIF1 fallback.
        if (pico_release_url_valid(UI_HANZI_FONT_URL)) {
            bool owned = false;
            if (ensure_network(&owned) && download_font() == ESP_OK) ui_hanzi_source_refresh();
            if (owned) read_pico_transfer_stop();
        }
    }
    atomic_store(&s_running, false);
    vTaskDelete(NULL);
}

void ui_hanzi_sync_start(void) {
    if (atomic_exchange(&s_running, true)) return;
    if (xTaskCreate(worker, "hanzi_sync", 8192, NULL, 3, NULL) != pdPASS) atomic_store(&s_running, false);
}
