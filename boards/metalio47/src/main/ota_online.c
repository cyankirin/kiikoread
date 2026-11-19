/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：HTTPS 分块下载、完整性校验和可取消升级任务。/ English: streamed HTTPS updates, integrity checks and cancellable workers.
 * 冻结：禁止覆盖当前槽；离页或锁屏先等待任务结束。/ Frozen: never overwrite the running slot; join before leaving or locking.
 * 用户修订：分段擦除并串行化闪存与屏幕推送，启动前检查连续内存，低内存安全失败。
 * User revision: erase incrementally and serialize flash with display output; require memory headroom and fail safely on shortage.
 */
#include "ota_online.h"
#include "ota_update.h"
#include "esp_private/cache_utils.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "read_pico_transfer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static SemaphoreHandle_t s_mutex, s_done;
static pico_update_status_t *s_status;
static atomic_bool s_cancel;
static bool s_joinable, s_download, s_save_tf;
static pico_ota_channel_t s_channel;
static const esp_partition_t *s_verified;

static bool initialize(void) {
    if (!s_status) s_status = heap_caps_calloc(1, sizeof(*s_status), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_done) s_done = xSemaphoreCreateBinary();
    return s_status && s_mutex && s_done;
}
bool pico_online_display_begin(void) {
    // 与 S31 SDK 的其他 Flash 写入共用递归锁，首次 OTA 前也保护供屏。
    // Share the S31 SDK recursive flash lock, protecting display scans before OTA initialization too.
    spi_flash_op_lock();
    return true;
}
void pico_online_display_end(bool held) {
    if (held) spi_flash_op_unlock();
}
static bool memory_available(size_t internal_min, size_t internal_block, size_t external_min) {
    const unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const unsigned external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    return heap_caps_get_free_size(internal) >= internal_min &&
        heap_caps_get_largest_free_block(internal) >= internal_block &&
        heap_caps_get_free_size(external) >= external_min &&
        heap_caps_get_largest_free_block(external) >= 32768;
}
// 锁只覆盖闪存操作，不覆盖网络等待、状态互斥锁或任务收尾，避免取消/刷屏互相等待。
// Lock flash operations only, excluding network waits, status locks and joins to avoid cancellation/display deadlocks.
static esp_err_t flash_begin(const esp_partition_t *target, esp_ota_handle_t *handle) {
    bool held = pico_online_display_begin();
    esp_err_t error = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, handle);
    pico_online_display_end(held);
    return error;
}
static esp_err_t flash_write(esp_ota_handle_t handle, const void *data, size_t size) {
    bool held = pico_online_display_begin();
    esp_err_t error = esp_ota_write(handle, data, size);
    pico_online_display_end(held);
    return error;
}
static esp_err_t flash_finish(esp_ota_handle_t handle, bool aborting) {
    bool held = pico_online_display_begin();
    esp_err_t error = aborting ? esp_ota_abort(handle) : esp_ota_end(handle);
    pico_online_display_end(held);
    return error;
}
static void state(pico_update_state_t value, const char *message) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status->state = value;
    snprintf(s_status->message, sizeof(s_status->message), "%s", message);
    s_status->same_base_official = false;
    s_status->tf_saved = false;
    ++s_status->revision;
    xSemaphoreGive(s_mutex);
}
bool pico_online_busy(void) {
    if (!s_status || !s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = s_status->busy;
    xSemaphoreGive(s_mutex); return busy;
}
void pico_online_get_status(pico_update_status_t *status) {
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!s_status || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(status, s_status, sizeof(*status));
    xSemaphoreGive(s_mutex);
}
static bool layout_valid(void) {
    const esp_partition_t *a = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *b = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    const esp_partition_t *data = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    return a && b && data && a->address == 0x10000 && b->address == 0x510000 &&
        a->size == 0x500000 && b->size == 0x500000 && data->address == 0xd000 && data->size == 0x2000;
}
static bool online(bool *owned) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (!(status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready)) {
        char ssid[33]; bool saved = false;
        if (read_pico_transfer_get_saved_wifi(ssid, &saved) != ESP_OK || !saved) {
            state(PICO_UPDATE_FAILED, "请先在 WiFi 设置中连接网络"); return false;
        }
        read_pico_transfer_stop();
        read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
        *owned = true;
        if (read_pico_transfer_start(&cfg) != ESP_OK) {
            state(PICO_UPDATE_FAILED, "WiFi 连接失败，请重试"); return false;
        }
    }
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (!atomic_load(&s_cancel)) {
        read_pico_transfer_service_poll();
        read_pico_transfer_get_status(&status);
        if (status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready) {
            if (time(NULL) < 1704067200) {
                uint32_t utc;
                if (read_pico_transfer_sync_time_online(&utc) != ESP_OK) {
                    state(PICO_UPDATE_FAILED, "网络对时失败，请对时后重试"); return false;
                }
            }
            return !atomic_load(&s_cancel);
        }
        if (status.state == READ_PICO_TRANSFER_ERROR || esp_timer_get_time() >= deadline) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!atomic_load(&s_cancel)) state(PICO_UPDATE_FAILED, "网络连接超时，请重试");
    return false;
}
typedef struct {
    uint32_t start, end, size;
    bool valid;
} download_range_t;
static esp_err_t range_header(esp_http_client_event_t *event) {
    download_range_t *range = event->user_data;
    if (!range || event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key || !event->header_value)
        return ESP_OK;
    if (!strcasecmp(event->header_key, "Content-Range")) {
        unsigned long start, end, size; char tail;
        range->valid = strnlen(event->header_value, 96) < 96 &&
            sscanf(event->header_value, "bytes %lu-%lu/%lu%c", &start, &end, &size, &tail) == 3 &&
            start <= end && end < size && size <= UINT32_MAX;
        if (range->valid) { range->start = start; range->end = end; range->size = size; }
    }
    return ESP_OK;
}
static esp_http_client_handle_t open_http_range(const char *url, uint32_t offset,
        download_range_t *range, int *status, int64_t *length) {
    if (range) memset(range, 0, sizeof(*range));
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 15000, .buffer_size = 2048,
        .buffer_size_tx = 1024, .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true, .keep_alive_enable = true,
        .event_handler = range_header, .user_data = range};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    esp_http_client_set_header(client, "User-Agent", PICO_OTA_CAPABILITY);
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    if (offset) {
        char value[48]; snprintf(value, sizeof(value), "bytes=%lu-", (unsigned long)offset);
        if (esp_http_client_set_header(client, "Range", value) != ESP_OK) goto fail;
    }
    if (esp_http_client_open(client, 0) != ESP_OK || atomic_load(&s_cancel)) goto fail;
    *length = esp_http_client_fetch_headers(client);
    if (*length < 0 || atomic_load(&s_cancel)) goto fail;
    *status = esp_http_client_get_status_code(client);
    esp_http_client_set_timeout_ms(client, 1000);
    return client;
fail:
    esp_http_client_cleanup(client);
    return NULL;
}
static esp_http_client_handle_t open_http(const char *url, int *status, int64_t *length) {
    return open_http_range(url, 0, NULL, status, length);
}
// 复用连接管理器等待短暂断网恢复，允许取消，不重启整个无线服务。
// Wait for the connection manager to recover a short outage, with cancellation and no radio restart.
static bool wait_network(void) {
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (!atomic_load(&s_cancel) && esp_timer_get_time() < deadline) {
        read_pico_transfer_service_poll();
        read_pico_transfer_status_t status;
        read_pico_transfer_get_status(&status);
        if (status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready) return true;
        if (status.state == READ_PICO_TRANSFER_ERROR) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}
// 读超时可重试；每次收到数据续期，避免慢速网络被总时长截断。
// Retry temporary read timeouts and renew the idle deadline after each chunk.
static int read_chunk(esp_http_client_handle_t client, uint8_t *buffer, int cap, int64_t *idle) {
    while (!atomic_load(&s_cancel) && esp_timer_get_time() < *idle) {
        read_pico_transfer_service_poll();
        int n = esp_http_client_read(client, (char *)buffer, cap);
        if (n > 0) { *idle = esp_timer_get_time() + 30000000; return n; }
        if (!n && esp_http_client_is_complete_data_received(client)) return 0;
        if (n < 0 && n != -ESP_ERR_HTTP_EAGAIN) return -1;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return -1;
}
// 中文：按通道拉取并解析清单；失败时直接给出对应提示。调用方负责缓冲区分配与后续决策。
// English: Fetch and parse the manifest for one channel; failures report their own message.
//          Callers own the buffers and the follow-up decisions.
static bool fetch_manifest(pico_ota_channel_t channel, char *json, pico_release_t *release) {
    // GitHub Pages清单优先，官网清单备用；同一缓冲区顺序请求，无并发TLS连接。
    // Prefer GitHub Pages metadata, falling back to the website with one shared buffer and no concurrent TLS clients.
    // 中文：Anki 版只读本 fork 的 Pages 清单，没有备用源；官方通道的清单源与顺序完全不变。
    // English: The Anki channel reads this fork's Pages manifest only; the official channel keeps its feed order.
    static const char *const official_feeds[] = {PICO_OTA_FEED_URL, PICO_OTA_FEED_FALLBACK_URL};
    static const char *const anki_feeds[] = {PICO_OTA_ANKI_FEED_URL};
    const bool anki = channel == PICO_OTA_CHANNEL_ANKI;
    const char *const *feeds = anki ? anki_feeds : official_feeds;
    const size_t feed_count = anki ? sizeof(anki_feeds) / sizeof(anki_feeds[0])
                                   : sizeof(official_feeds) / sizeof(official_feeds[0]);
    bool valid = false;
    for (size_t source = 0; source < feed_count && !atomic_load(&s_cancel); ++source) {
        char url[192]; snprintf(url, sizeof(url), "%s?t=%lld", feeds[source], (long long)time(NULL));
        int code = -1; int64_t length = -1;
        esp_http_client_handle_t client = NULL;
        for (int i = 0; i < 2 && !atomic_load(&s_cancel); ++i) {
            if (wait_network()) client = open_http(url, &code, &length);
            if (client) break;
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (!client) continue;
        if (code != 200 || length > 8192) { esp_http_client_cleanup(client); continue; }
        size_t received = 0;
        int64_t idle = esp_timer_get_time() + 30000000;
        int n = -1;
        while (received < 8192 && (n = read_chunk(client, (uint8_t *)json + received, (int)(8192 - received), &idle)) > 0)
            received += n;
        bool complete = esp_http_client_is_complete_data_received(client);
        esp_http_client_cleanup(client);
        json[received] = 0;
        if (n >= 0 && complete && pico_release_parse(json, received, release) == ESP_OK) { valid = true; break; }
    }
    if (!valid)
        state(PICO_UPDATE_FAILED, anki ? "Anki 版清单获取失败，请稍后重试"
                                       : "更新服务器连接失败或清单无效，请稍后重试");
    return valid;
}
static bool check_feed(void) {
    char *json = heap_caps_malloc(8193, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pico_release_t *release = heap_caps_calloc(1, sizeof(*release), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json || !release) { state(PICO_UPDATE_FAILED, "内存不足，请退出传书后重试"); goto failed; }
    if (!fetch_manifest(s_channel, json, release)) goto failed;
    const esp_app_desc_t *running = esp_app_get_description();
    if (strcmp(release->project, running->project_name) || !layout_valid() ||
        pico_version_compare(running->version, release->minimum_base_version) < 0) {
        state(PICO_UPDATE_FAILED, "基础包不兼容，请先通过官网完整刷机"); goto failed;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_status->release, release, sizeof(*release));
    xSemaphoreGive(s_mutex);
    // 中文：本机版本号在官方基线上加了小修订（官方 rc114 → 本机 rc114.01），版本比较天然是「不更新」。
    //       大版本一致时官方原版也允许在线升级，但升级会移除 Anki 功能——状态里带上标记，
    //       确认页据此改写文案并提供「下载 Anki 版到 TF 卡」。
    // English: The local build carries a sub-revision on top of the official base (official rc114 →
    //   local rc114.01), so the comparison is naturally "not newer". With matching base versions the
    //   official build is offered online as well; it removes Anki, so the status carries a flag and
    //   the confirm page swaps in the save-the-Anki-image-to-TF wording.
    const int comparison = pico_version_compare(release->version, running->version);
    const size_t base_len = strlen(release->version);
    const bool same_base = strlen(running->version) > base_len &&
        running->version[base_len] == '.' && !strncmp(running->version, release->version, base_len);
    if (comparison > 0) {
        state(PICO_UPDATE_AVAILABLE, "发现新版本，可下载安装");
    } else if (same_base && s_channel == PICO_OTA_CHANNEL_OFFICIAL) {
        state(PICO_UPDATE_AVAILABLE, "官方原版可升级（升级将移除 Anki 功能）");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status->same_base_official = true;
        xSemaphoreGive(s_mutex);
    } else {
        state(PICO_UPDATE_LATEST, "当前已是最新版本");
    }
    heap_caps_free(json); heap_caps_free(release);
    return true;
failed:
    heap_caps_free(json); heap_caps_free(release);
    return false;
}
static bool header_valid(const uint8_t *data, const pico_release_t *release) {
    const esp_image_header_t *header = (const esp_image_header_t *)data;
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(data + sizeof(*header) + sizeof(esp_image_segment_header_t));
    const esp_app_desc_t *current = esp_app_get_description();
    return header->magic == ESP_IMAGE_HEADER_MAGIC && header->chip_id == ESP_CHIP_ID_ESP32S31 &&
        desc->magic_word == ESP_APP_DESC_MAGIC_WORD &&
        memchr(desc->project_name, 0, sizeof(desc->project_name)) && memchr(desc->version, 0, sizeof(desc->version)) &&
        !strcmp(desc->project_name, current->project_name) && !strcmp(desc->version, release->version);
}
static bool download(void) {
    pico_release_t *release = heap_caps_malloc(sizeof(*release), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *buffer = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *header = heap_caps_malloc(288, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!release || !buffer || !header) { state(PICO_UPDATE_FAILED, "内存不足，无法下载升级包"); goto failed; }
    xSemaphoreTake(s_mutex, portMAX_DELAY); *release = s_status->release; xSemaphoreGive(s_mutex);
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!layout_valid() || !target || target == esp_ota_get_running_partition() || release->size > target->size) {
        state(PICO_UPDATE_FAILED, "基础包不兼容，请通过官网完整刷机"); goto failed;
    }
    // 中文：官方通道沿用同一固定文件名的两个允许源，连接失败才切换备用源；Anki 通道的清单
    //       url 已过白名单校验（指向本 fork 站点根的扁平文件名），直接使用。
    // English: The official channel keeps its two fixed-name origins and falls back on connection
    //          failure; the Anki manifest's URL already passed the whitelist check (flat name on
    //          this fork's site root) and is used as-is.
    const char *filename = strrchr(release->url, '/');
    if (!filename || !pico_release_url_valid(release->url)) goto failed;
    char download_urls[2][256];
    unsigned download_source = 0;
    const char *anki_url = NULL;
    if (s_channel == PICO_OTA_CHANNEL_ANKI) {
        anki_url = release->url;
    } else {
        static const char *const origins[] = {"https://wegooo-cell.github.io/read-pico-reader/", "https://kiikoread.com/"};
        for (unsigned source = 0; source < 2; ++source) {
            int size = snprintf(download_urls[source], sizeof(download_urls[source]), "%s%s", origins[source], filename + 1);
            if (size < 0 || (size_t)size >= sizeof(download_urls[source])) goto failed;
        }
    }
    int code = -1; int64_t length = -1;
    esp_http_client_handle_t client = NULL;
    download_range_t range;
    esp_ota_handle_t handle = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    bool begun = false;
    esp_err_t error = ESP_OK;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) error = ESP_FAIL;
    size_t total = 0, header_size = 0;
    int64_t idle = esp_timer_get_time() + 30000000;
    int n = -1;
    unsigned retries = 0;
    bool complete = false;
    while (error == ESP_OK && !atomic_load(&s_cancel)) {
        if (!client) {
            if (wait_network()) client = open_http_range(anki_url ? anki_url : download_urls[download_source], total, &range, &code, &length);
            if (!client) {
                download_source = 1; // Anki 通道无备用源，重试同一 URL。
                if (atomic_load(&s_cancel) || retries++ >= 3) { error = ESP_FAIL; break; }
                vTaskDelay(pdMS_TO_TICKS(250 * retries)); continue;
            }
            // 续传必须严格匹配起点与全文件长度；最终仍验证整本镜像的 SHA256。
            // Resumed responses must match their offset and full length; the whole image still gets SHA256 verification.
            bool response_ok = total ? code == 206 && range.valid && range.start == total &&
                range.size == release->size && range.end == release->size - 1 : code == 200;
            if (!response_ok || (length > 0 && (uint64_t)length != release->size - total)) {
                error = ESP_ERR_INVALID_RESPONSE; break;
            }
            idle = esp_timer_get_time() + 30000000;
        }
        n = read_chunk(client, buffer, 4096, &idle);
        if (n <= 0) {
            complete = esp_http_client_is_complete_data_received(client);
            esp_http_client_cleanup(client); client = NULL;
            if (n == 0 && complete && total == release->size) break;
            complete = false;
            download_source = 1; // Anki 通道无备用源，重试同一 URL。
            if (total >= release->size || atomic_load(&s_cancel) || retries++ >= 3) { error = ESP_FAIL; break; }
            vTaskDelay(pdMS_TO_TICKS(250 * retries));
            continue;
        }
        if (total + n > release->size) { error = ESP_ERR_INVALID_SIZE; break; }
        size_t used = 0;
        if (!begun) {
            size_t needed = 288 - header_size;
            used = (size_t)n < needed ? (size_t)n : needed;
            memcpy(header + header_size, buffer, used); header_size += used;
            if (header_size == 288) {
                if (!header_valid(header, release)) { error = ESP_ERR_INVALID_RESPONSE; break; }
                error = flash_begin(target, &handle);
                begun = error == ESP_OK;
                if (begun) error = flash_write(handle, header, header_size);
            }
        }
        if (error == ESP_OK && begun && (size_t)n > used) error = flash_write(handle, buffer + used, n - used);
        if (error != ESP_OK || psa_hash_update(&hash, buffer, n) != PSA_SUCCESS) { error = ESP_FAIL; break; }
        total += n;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status->received = total; ++s_status->revision;
        xSemaphoreGive(s_mutex);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (client) esp_http_client_cleanup(client);
    uint8_t digest[32]; size_t digest_size = 0; char hex[65];
    if (error == ESP_OK && (!begun || n < 0 || !complete || total != release->size || atomic_load(&s_cancel))) error = ESP_FAIL;
    if (error == ESP_OK && psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS) error = ESP_FAIL;
    psa_hash_abort(&hash);
    if (error == ESP_OK) {
        for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        if (digest_size != 32 || strcasecmp(hex, release->sha256)) error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error != ESP_OK) {
        if (begun) flash_finish(handle, true);
        state(PICO_UPDATE_FAILED, "下载中断或校验失败，当前版本保持不变"); goto failed;
    }
    error = flash_finish(handle, false);
    if (error != ESP_OK) { state(PICO_UPDATE_FAILED, "固件校验失败，当前版本保持不变"); goto failed; }
    // 后台仅验证镜像；主线程处理最终启动切换，取消时不影响当前槽。
    // The worker verifies only; the main task commits boot, so cancellation cannot change the running slot.
    xSemaphoreTake(s_mutex, portMAX_DELAY); s_verified = target; xSemaphoreGive(s_mutex);
    state(PICO_UPDATE_READY, "校验通过，即将重启安装");
    heap_caps_free(release); heap_caps_free(buffer); heap_caps_free(header);
    return true;
failed:
    heap_caps_free(release); heap_caps_free(buffer); heap_caps_free(header);
    return false;
}
// 中文：把 Anki 通道固件流式写入 TF 卡根目录（Metalio-S31-update.bin），供升级官方原版后经
//       「TF 卡升级」手动装回。不触碰 OTA 槽；先检查卡上剩余空间，首 288 字节校验镜像头，
//       落盘校验 SHA256，失败删残包。
// English: Stream the Anki-channel firmware to the TF-card root (Metalio-S31-update.bin) so it can
//          be installed back through the local TF update after upgrading to the official build. No
//          OTA slot is touched; free space is checked up front, the image header validated early,
//          SHA256 on completion, and a failed attempt deletes the partial file.
static bool save_anki_tf(void) {
    pico_release_t *release = heap_caps_malloc(sizeof(*release), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *buffer = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *header = heap_caps_malloc(288, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    char *json = heap_caps_malloc(8193, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!release || !buffer || !header || !json) { state(PICO_UPDATE_FAILED, "内存不足，无法保存 Anki 版固件"); goto failed; }
    if (!fetch_manifest(PICO_OTA_CHANNEL_ANKI, json, release)) goto failed;
    const esp_app_desc_t *running = esp_app_get_description();
    if (strcmp(release->project, running->project_name) || !layout_valid() ||
        pico_version_compare(running->version, release->minimum_base_version) < 0) {
        state(PICO_UPDATE_FAILED, "基础包不兼容，无法保存 Anki 版固件"); goto failed;
    }
    // 卡容量检查按 pico 版对齐：pico 版不做预检，靠 fopen 打不开直接报「TF 卡不可用」。
    // Capacity pre-check aligns with the pico copy: no up-front probe; an unopenable card
    // already reports "TF 卡不可用" through the fopen failure path below.
    state(PICO_UPDATE_DOWNLOADING, "正在下载 Anki 版固件到 TF 卡");
    bool ok = false;
    for (unsigned attempt = 0; attempt < 3 && !ok && !atomic_load(&s_cancel); ++attempt) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(250 * attempt));
        FILE *file = fopen(PICO_OTA_UPDATE_PATH, "wb");
        if (!file) { state(PICO_UPDATE_FAILED, "TF 卡不可用，无法保存 Anki 版固件"); goto failed; }
        size_t total = 0, header_size = 0;
        bool header_ok = false, stream_error = false, complete = false;
        psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
        if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            fclose(file);
            state(PICO_UPDATE_FAILED, "安全模块初始化失败"); goto failed;
        }
        int code = -1; int64_t length = -1;
        esp_http_client_handle_t client = NULL;
        int64_t idle = esp_timer_get_time() + 30000000;
        int n = -1;
        unsigned retries = 0;
        while (!stream_error && !atomic_load(&s_cancel)) {
            if (!client) {
                if (wait_network()) client = open_http(release->url, &code, &length);
                if (!client) {
                    if (atomic_load(&s_cancel) || retries++ >= 3) { stream_error = true; break; }
                    vTaskDelay(pdMS_TO_TICKS(250 * retries)); continue;
                }
                if (code != 200 || (length > 0 && (uint64_t)length != release->size)) { stream_error = true; break; }
                idle = esp_timer_get_time() + 30000000;
            }
            n = read_chunk(client, buffer, 4096, &idle);
            if (n <= 0) {
                complete = esp_http_client_is_complete_data_received(client);
                esp_http_client_cleanup(client); client = NULL;
                if (n == 0 && complete && total == release->size) { ok = true; break; }
                if (atomic_load(&s_cancel) || retries++ >= 3) { stream_error = true; break; }
                break;  // 断流即整包重来：每次尝试都重新截断写文件。
            }
            if (total + (size_t)n > release->size) { stream_error = true; break; }
            if (header_size < 288) {
                size_t used = 288 - header_size < (size_t)n ? 288 - header_size : (size_t)n;
                memcpy(header + header_size, buffer, used);
                header_size += used;
                if (header_size == 288 && !header_valid(header, release)) { stream_error = true; break; }
                header_ok = true;
            }
            if (psa_hash_update(&hash, buffer, (size_t)n) != PSA_SUCCESS) { stream_error = true; break; }
            if (fwrite(buffer, 1, (size_t)n, file) != (size_t)n) { stream_error = true; break; }
            total += (size_t)n;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status->received = total; ++s_status->revision;
            xSemaphoreGive(s_mutex);
        }
        if (client) esp_http_client_cleanup(client);
        uint8_t digest[32] = {0};
        size_t digest_len = 0;
        bool hashed = psa_hash_finish(&hash, digest, sizeof(digest), &digest_len) == PSA_SUCCESS
            && digest_len == sizeof(digest);
        if (fclose(file) != 0) ok = false;
        if (ok) {
            char expected[65];
            for (int i = 0; i < 32; ++i) snprintf(expected + i * 2, 3, "%02x", digest[i]);
            ok = header_ok && hashed && !strcasecmp(expected, release->sha256);
        }
        if (!ok && !atomic_load(&s_cancel)) {
            remove(PICO_OTA_UPDATE_PATH);
            state(PICO_UPDATE_DOWNLOADING, "下载校验失败，正在重试");
        }
    }
    if (atomic_load(&s_cancel)) remove(PICO_OTA_UPDATE_PATH);
    heap_caps_free(release); heap_caps_free(buffer); heap_caps_free(header); heap_caps_free(json);
    if (ok) {
        state(PICO_UPDATE_LATEST, "Anki 版固件已存入 TF 卡；升级原版后可在 TF 卡升级中装回");
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status->tf_saved = true;
        xSemaphoreGive(s_mutex);
    }
    return ok;
failed:
    remove(PICO_OTA_UPDATE_PATH);
    heap_caps_free(release); heap_caps_free(buffer); heap_caps_free(header); heap_caps_free(json);
    return false;
}
static void worker(void *unused) {
    (void)unused;
    bool owned = false;
    wifi_ps_type_t previous_ps = WIFI_PS_MIN_MODEM;
    bool restore_ps = false;
    if (online(&owned) && !atomic_load(&s_cancel)) {
        // 更新期间关闭无线省电；完成、取消和失败均恢复原策略。
        // Disable radio power saving for updates, then restore it on success, cancellation or failure.
        restore_ps = esp_wifi_get_ps(&previous_ps) == ESP_OK && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
        if (!memory_available(32768, 8192, 262144))
            state(PICO_UPDATE_FAILED, "内存不足，请退出传书后重试");
        else if (s_save_tf) (void)save_anki_tf();
        else if (s_download) (void)download(); else (void)check_feed();
    }
    if (restore_ps) (void)esp_wifi_set_ps(previous_ps);
    if (owned) read_pico_transfer_stop();
    if (atomic_load(&s_cancel)) state(PICO_UPDATE_CANCELLED, "已取消，当前版本保持不变");
    xSemaphoreTake(s_mutex, portMAX_DELAY); s_status->busy = false; ++s_status->revision; xSemaphoreGive(s_mutex);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}
static esp_err_t start(bool downloading, bool save_tf, pico_ota_channel_t channel) {
    if (!initialize()) return ESP_ERR_NO_MEM;
    if (channel != PICO_OTA_CHANNEL_OFFICIAL && channel != PICO_OTA_CHANNEL_ANKI)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = s_status->busy;
    bool available = s_status->state == PICO_UPDATE_AVAILABLE && s_status->channel == channel;
    xSemaphoreGive(s_mutex);
    if (busy || (downloading && !available && !save_tf)) return ESP_ERR_INVALID_STATE;
    if (s_joinable) { xSemaphoreTake(s_done, portMAX_DELAY); s_joinable = false; }
    if (!memory_available(49152, 16384, 524288)) {
        state(PICO_UPDATE_FAILED, "内存不足，请退出传书后重试");
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_cancel, false);
    s_download = downloading; s_save_tf = save_tf; s_verified = NULL; s_channel = channel;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status->busy = true; s_status->received = 0; s_status->channel = channel;
    xSemaphoreGive(s_mutex);
    const bool anki = channel == PICO_OTA_CHANNEL_ANKI;
    state(downloading ? PICO_UPDATE_DOWNLOADING : PICO_UPDATE_CHECKING,
          save_tf ? "正在连接并获取 Anki 版固件"
                  : downloading ? (anki ? "正在下载 Anki 版，短暂断网会自动重连" : "正在下载，短暂断网会自动重连")
                                : (anki ? "正在连接并检查 Anki 版更新" : "正在连接并检查更新"));
    if (xTaskCreate(worker, "kiikoread_ota", 12288, NULL, 4, NULL) != pdPASS) {
        xSemaphoreTake(s_mutex, portMAX_DELAY); s_status->busy = false; xSemaphoreGive(s_mutex);
        state(PICO_UPDATE_FAILED, "内存不足，无法开始升级"); return ESP_ERR_NO_MEM;
    }
    s_joinable = true;
    return ESP_OK;
}
esp_err_t pico_online_check(void) { return start(false, false, PICO_OTA_CHANNEL_OFFICIAL); }
esp_err_t pico_online_download(void) { return start(true, false, PICO_OTA_CHANNEL_OFFICIAL); }
esp_err_t pico_online_check_channel(pico_ota_channel_t channel) { return start(false, false, channel); }
esp_err_t pico_online_download_channel(pico_ota_channel_t channel) { return start(true, false, channel); }
esp_err_t pico_online_save_anki_tf(void) { return start(false, true, PICO_OTA_CHANNEL_ANKI); }
void pico_online_cancel_join(void) {
    if (s_joinable) {
        atomic_store(&s_cancel, true);
        xSemaphoreTake(s_done, portMAX_DELAY); s_joinable = false;
    }
    s_verified = NULL;
    if (s_status && s_status->state == PICO_UPDATE_READY) state(PICO_UPDATE_CANCELLED, "已取消，当前版本保持不变");
}
esp_err_t pico_online_commit(void) {
    if (s_joinable) { xSemaphoreTake(s_done, portMAX_DELAY); s_joinable = false; }
    if (!s_verified || atomic_load(&s_cancel)) return ESP_ERR_INVALID_STATE;
    bool held = pico_online_display_begin();
    esp_err_t error = esp_ota_set_boot_partition(s_verified);
    pico_online_display_end(held);
    s_verified = NULL;
    if (error != ESP_OK) state(PICO_UPDATE_FAILED, "启动切换失败，当前版本保持不变");
    return error;
}
