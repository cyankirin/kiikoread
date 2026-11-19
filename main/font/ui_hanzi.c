/* SPDX-License-Identifier: Apache-2.0
 * 中文：按索引读取压缩的内建黑体汉字，各消费者仅缓存一个32字块，避免整份字体驻留。
 *   优先使用 /flash/fonts 下的外置全量补字（PIF4，含 393 个扩展字），装载时校验头、
 *   长度与全文件 SHA-256；缺失、截断或校验不符时回退内嵌 PIF1 基线（5637 个 GB2312
 *   字，位图像素与全量版一致），消费方接口不变。
 * English: Indexed compressed built-in Han glyphs; independent 32-glyph block caches avoid
 *   retaining a whole font. Prefer the external full supplement under /flash/fonts (PIF4,
 *   393 extra glyphs): header, length and whole-file SHA-256 are verified at load. A
 *   missing, truncated or mismatched file falls back to the embedded PIF1 baseline (5637
 *   GB2312 glyphs whose pixels match the full set) with the consumer API unchanged.
 */
#include "ui_hanzi.h"
#include "esp_heap_caps.h"
#include "miniz.h"
#include <stddef.h>
#include <string.h>
extern const uint8_t ui_hanzi_start[] asm("_binary_ui_hanzi_bin_start");
extern const uint8_t ui_hanzi_end[] asm("_binary_ui_hanzi_bin_end");
#define BLOCK 32u
// 编译期固定的外置全量补字期望 SHA-256（flash/fonts/ 分发文件），防半截写入或被替换。
// Compile-time expected SHA-256 of the distributed full supplement; guards partial writes and swaps.
static const uint8_t ui_hanzi_font_sha256[32] = {
    0x18, 0x4e, 0xea, 0x2a, 0x74, 0x70, 0x98, 0x25, 0x0b, 0x38, 0x9a, 0x58, 0x9b, 0x82, 0x44, 0x43,
    0x07, 0x93, 0xa0, 0x64, 0xf8, 0x37, 0xc2, 0x2a, 0x15, 0x50, 0xf3, 0xfc, 0xa4, 0xdd, 0x57, 0x1e};

static uint32_t word(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// 紧凑 SHA-256（FIPS 180-4），仅用于启动时一次字体校验，避免给宿主测试引入 TLS 头依赖。
// Compact SHA-256 (FIPS 180-4), used once at startup so host tests need no TLS headers.
static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
typedef struct { uint32_t h[8]; uint64_t bits; uint8_t buf[64]; size_t fill; } ui_sha256_t;
static void ui_sha_init(ui_sha256_t *c) {
    static const uint32_t seed[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, seed, sizeof(seed)); c->bits = 0; c->fill = 0;
}
static uint32_t sha_ror(uint32_t v, unsigned n) { return v >> n | v << (32 - n); }
static void ui_sha_block(ui_sha256_t *c, const uint8_t *p) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = sha_ror(w[i - 15], 7) ^ sha_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = sha_ror(w[i - 2], 17) ^ sha_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], hh = c->h[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1 = sha_ror(e, 6) ^ sha_ror(e, 11) ^ sha_ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + s1 + ch + sha_k[i] + w[i];
        uint32_t s0 = sha_ror(a, 2) ^ sha_ror(a, 13) ^ sha_ror(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += hh;
}
static void ui_sha_update(ui_sha256_t *c, const uint8_t *data, size_t n) {
    c->bits += (uint64_t)n * 8;
    while (n) {
        size_t take = 64 - c->fill; if (take > n) take = n;
        memcpy(c->buf + c->fill, data, take); c->fill += take; data += take; n -= take;
        if (c->fill == 64) { ui_sha_block(c, c->buf); c->fill = 0; }
    }
}
static void ui_sha_final(ui_sha256_t *c, uint8_t out[32]) {
    uint64_t bits = c->bits;
    uint8_t pad = 0x80;
    ui_sha_update(c, &pad, 1);
    while (c->fill != 56) { pad = 0; ui_sha_update(c, &pad, 1); }
    uint8_t len[8];
    for (unsigned i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    ui_sha_update(c, len, 8);
    for (unsigned i = 0; i < 8; ++i) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
// 装载与源切换串行化；查找与块解码只读不可变数据，在锁外进行。
// Serialize loading and source switches; lookups and block decodes read immutable data lock-free.
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_lock_mem;
static SemaphoreHandle_t s_lock;
static void ui_hanzi_lock(void) {
    portENTER_CRITICAL(&s_init_mux);
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);
    portEXIT_CRITICAL(&s_init_mux);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void ui_hanzi_unlock(void) { xSemaphoreGive(s_lock); }
#else
// 宿主测试在首个调用中串行完成装载，之后数据只读，无需加锁。
// Host tests finish loading serially on the first call; data is read-only afterwards.
static void ui_hanzi_lock(void) {}
static void ui_hanzi_unlock(void) {}
#endif

typedef struct {
    const uint8_t *base;   // 内嵌载荷；外置文件模式为 NULL。/ Embedded payload; NULL in file mode.
    uint16_t *chars;       // 外置模式：码点升序索引（PSRAM）。/ File mode: sorted codepoint index in PSRAM.
    uint32_t *offsets;     // 外置模式：块偏移表（blocks+1 项）。/ File mode: block offset table (blocks+1 entries).
    const char *path;      // 外置模式：文件路径。/ File mode: file path.
    size_t bytes, count, blocks, data_offset, packed_max, record;
    bool ok;
} ui_hanzi_source_t;

static ui_hanzi_source_t s_sources[2];
static ui_hanzi_source_t *const s_embedded = &s_sources[0];
static ui_hanzi_source_t *const s_external = &s_sources[1];
static ui_hanzi_cache_t s_ui_cache;
static ui_hanzi_source_t *s_active;
static uint32_t s_generation = 1;
static unsigned s_external_retries;
#define UI_HANZI_EXTERNAL_RETRIES 4u

typedef enum { UI_HANZI_LOAD_BAD, UI_HANZI_LOAD_RETRY, UI_HANZI_LOAD_OK } ui_hanzi_load_t;

static void init_embedded(void) {
    ui_hanzi_source_t *s = s_embedded;
    memset(s, 0, sizeof(*s));
    s->base = ui_hanzi_start;
    s->bytes = (size_t)(ui_hanzi_end - ui_hanzi_start);
    s->ok = s->bytes >= 16 && (!memcmp(s->base, "PIF1", 4) || !memcmp(s->base, "PIF4", 4));
    if (s->ok) {
        s->count = word(s->base + 4);
        s->blocks = (s->count + BLOCK - 1) / BLOCK;
        s->record = memcmp(s->base, "PIF4", 4) ? UI_HANZI_RECORD : UI_HANZI_AA_RECORD;
        s->data_offset = 16 + s->count * 2 + (s->blocks + 1) * 4;
        s->ok = s->count > 0 && s->count <= 6763 && word(s->base + 8) == UI_HANZI_BASE_PX &&
                word(s->base + 12) == BLOCK && s->data_offset <= s->bytes;
    }
}

static bool verify_payload(FILE *f) {
    // 回卷到文件头对整份文件流式计算 SHA-256，并与编译期期望值比对。
    // Rewind and hash the whole file, then compare with the compile-time pin.
    ui_sha256_t ctx;
    ui_sha_init(&ctx);
    uint8_t *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) return false;
    uint8_t sum[32];
    bool ok = false;
    if (fseek(f, 0, SEEK_SET)) goto done;
    size_t left = UI_HANZI_FONT_BYTES;
    while (left) {
        size_t take = left < 4096 ? left : 4096;
        if (fread(chunk, 1, take, f) != take) goto done;
        ui_sha_update(&ctx, chunk, take);
        left -= take;
    }
    ui_sha_final(&ctx, sum);
    ok = !memcmp(sum, ui_hanzi_font_sha256, sizeof(sum));
done:
    heap_caps_free(chunk);
    return ok;
}

static bool valid_header(const uint8_t head[16]) {
    size_t count = word(head + 4), blocks = (count + BLOCK - 1) / BLOCK;
    return !memcmp(head, "PIF4", 4) && count > 0 && count <= 6763 &&
           word(head + 8) == UI_HANZI_BASE_PX && word(head + 12) == BLOCK &&
           16 + count * 2 + (blocks + 1) * 4 <= UI_HANZI_FONT_BYTES;
}

static ui_hanzi_load_t load_external(void) {
    // 只在互斥锁内、且外置源未发布时进入；旧索引数组从不回收，读者持有的旧指针始终有效。
    // Entered under the mutex with the external source unpublished; old index arrays are never
    // freed, so a reader's snapshot of them stays valid.
    FILE *f = fopen(UI_HANZI_FLASH_FONT_PATH, "rb");
    if (!f) return UI_HANZI_LOAD_BAD;
    ui_hanzi_load_t result = UI_HANZI_LOAD_BAD;
    uint16_t *chars = NULL;
    uint32_t *offsets = NULL;
    uint8_t head[16];
    if (fseek(f, 0, SEEK_END)) goto done;
    if (ftell(f) != (long)UI_HANZI_FONT_BYTES || fseek(f, 0, SEEK_SET)) goto done;
    if (fread(head, 1, sizeof(head), f) != sizeof(head) || !valid_header(head)) goto done;
    size_t count = word(head + 4), blocks = (count + BLOCK - 1) / BLOCK;
    chars = heap_caps_malloc(count * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    offsets = heap_caps_malloc((blocks + 1) * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chars || !offsets) { result = UI_HANZI_LOAD_RETRY; goto done; }
    if (fread(chars, 1, count * 2, f) != count * 2) goto done;
    if (fread(offsets, 1, (blocks + 1) * 4, f) != (blocks + 1) * 4) goto done;
    size_t data_offset = 16 + count * 2 + (blocks + 1) * 4;
    if (offsets[blocks] > UI_HANZI_FONT_BYTES - data_offset) goto done;
    size_t packed_max = 0;
    for (size_t i = 0; i < blocks; ++i) {
        if (offsets[i] > offsets[i + 1]) goto done;
        if (offsets[i + 1] - offsets[i] > packed_max) packed_max = offsets[i + 1] - offsets[i];
    }
    if (!verify_payload(f)) goto done;
    s_external->chars = chars; chars = NULL;
    s_external->offsets = offsets; offsets = NULL;
    s_external->path = UI_HANZI_FLASH_FONT_PATH;
    s_external->bytes = UI_HANZI_FONT_BYTES;
    s_external->count = count;
    s_external->blocks = blocks;
    s_external->record = UI_HANZI_AA_RECORD;
    s_external->data_offset = data_offset;
    s_external->packed_max = packed_max ? packed_max : 1;
    s_external->ok = true;
    result = UI_HANZI_LOAD_OK;
done:
    heap_caps_free(chars);
    heap_caps_free(offsets);
    fclose(f);
    return result;
}

static void ensure_locked(void) {
    if (!s_embedded->base) init_embedded();
    if (s_active) return;
    ui_hanzi_load_t result = load_external();
    if (result == UI_HANZI_LOAD_OK) s_active = s_external;
    else if (result == UI_HANZI_LOAD_RETRY && ++s_external_retries < UI_HANZI_EXTERNAL_RETRIES)
        s_active = NULL;  // 分配失败留到下次查找重试。/ Allocation failure retries on the next lookup.
    else {
        // 文件缺失或校验不符：本次启动保持内嵌基线；分配重试额度用尽同样定格。
        // Missing or mismatched file: keep the embedded baseline for this boot; retry budget
        // exhausted freezes that decision too.
        s_active = s_embedded;
        s_external_retries = 0;
    }
}

typedef struct { const ui_hanzi_source_t *source; uint32_t generation; } ui_hanzi_view_t;

static ui_hanzi_view_t ui_hanzi_view(void) {
    ui_hanzi_lock();
    ensure_locked();
    ui_hanzi_view_t view = {s_active ? s_active : s_embedded, s_generation};
    ui_hanzi_unlock();
    return view;
}

static int index_in(const ui_hanzi_source_t *s, uint32_t cp) {
    if (!s->ok || cp > 0xffff) return -1;
    const uint8_t *chars = s->base ? s->base + 16 : (const uint8_t *)s->chars;
    size_t low = 0, high = s->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        uint32_t candidate = chars[mid * 2] | (uint32_t)chars[mid * 2 + 1] << 8;
        if (candidate < cp) low = mid + 1; else high = mid;
    }
    return low < s->count && (chars[low * 2] | (uint32_t)chars[low * 2 + 1] << 8) == cp ? (int)low : -1;
}

bool ui_hanzi_has(uint32_t cp) { return index_in(ui_hanzi_view().source, cp) >= 0; }

bool ui_hanzi_font_file_valid(const char *path) {
    // 独立于当前数据源的完整校验：长度、头与全文件 SHA-256。
    // Full validation independent of the active source: length, header and whole-file SHA-256.
    if (!path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    uint8_t head[16];
    if (fseek(f, 0, SEEK_END)) goto done;
    if (ftell(f) != (long)UI_HANZI_FONT_BYTES || fseek(f, 0, SEEK_SET)) goto done;
    if (fread(head, 1, sizeof(head), f) != sizeof(head) || !valid_header(head)) goto done;
    ok = verify_payload(f);
done:
    fclose(f);
    return ok;
}

bool ui_hanzi_source_refresh(void) {
    ui_hanzi_lock();
    if (!s_embedded->base) init_embedded();
    if (s_active == s_external) {
        // 外置已启用：只复核文件本体，校验失败才退回内嵌基线。
        // External already active: re-verify the file itself; fall back only when it fails.
        if (!ui_hanzi_font_file_valid(UI_HANZI_FLASH_FONT_PATH)) { s_active = s_embedded; s_generation++; }
    } else {
        s_external_retries = 0;
        if (load_external() == UI_HANZI_LOAD_OK) { s_active = s_external; s_generation++; }
    }
    bool active = s_active == s_external;
    ui_hanzi_unlock();
    return active;
}

static const uint8_t *decode_record(ui_hanzi_cache_t *cache, uint32_t cp, size_t *record_size) {
    if (!cache || !record_size) return NULL;
    ui_hanzi_view_t view = ui_hanzi_view();
    const ui_hanzi_source_t *s = view.source;
    int index = index_in(s, cp);
    if (index < 0) return NULL;
    *record_size = s->record;
    size_t need = BLOCK * s->record;
    if (cache->generation != view.generation || cache->bytes != need) {
        // 数据源已切换：旧块缓冲按另一记录尺寸分配，整体重建。
        // Source switched: the old block buffer was sized for the other record format; rebuild.
        ui_hanzi_cache_release(cache);
        cache->generation = view.generation;
    }
    if (!cache->block) {
        cache->block = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (cache->block) cache->bytes = need;
    }
    if (!cache->inflate) cache->inflate = heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cache->block || !cache->inflate) return NULL;
    size_t block = (size_t)index / BLOCK;
    const uint8_t *input = NULL;
    size_t span = 0;
    if (s->base) {
        const uint8_t *offsets = s->base + 16 + s->count * 2;
        const uint8_t *data = offsets + (s->blocks + 1) * 4;
        size_t start = word(offsets + block * 4), end = word(offsets + (block + 1) * 4);
        if (end < start || end > s->bytes - (size_t)(data - s->base)) return NULL;
        input = data + start;
        span = end - start;
    } else {
        if (!cache->packed) {
            cache->packed = heap_caps_malloc(s->packed_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (cache->packed) cache->packed_bytes = s->packed_max;
        }
        if (!cache->packed) return NULL;
        span = s->offsets[block + 1] - s->offsets[block];
        if (!cache->font) {
            // 各缓存独占一个只读句柄，避免并发 fseek 竞争文件位置。
            // One read-only handle per cache keeps concurrent seeks off the shared file position.
            cache->font = fopen(s->path, "rb");
            if (!cache->font) return NULL;
        }
        if (fseek(cache->font, (long)(s->data_offset + s->offsets[block]), SEEK_SET) ||
            fread(cache->packed, 1, span, cache->font) != span) return NULL;
        input = cache->packed;
    }
    if (!cache->valid || cache->cached != block) {
        size_t entries = s->count - block * BLOCK; if (entries > BLOCK) entries = BLOCK;
        cache->valid = false;
        // 解压器也放 PSRAM；便利解压函数的大型栈变量不适合 UI 任务。
        // Keep the inflater in PSRAM too; the convenience decoder's large stack frame is unsuitable for UI tasks.
        tinfl_init((tinfl_decompressor *)cache->inflate);
        size_t input_bytes = span, output = need;
        tinfl_status status = tinfl_decompress(cache->inflate, input, &input_bytes, cache->block, cache->block, &output,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (status != TINFL_STATUS_DONE || output != entries * s->record || input_bytes != span) return NULL;
        cache->cached = block;
        cache->valid = true;
    }
    return cache->block + ((unsigned)index % BLOCK) * s->record;
}

bool ui_hanzi_get_cached(ui_hanzi_cache_t *cache,uint32_t cp,uint8_t record[UI_HANZI_RECORD]) {
    if(!record)return false;
    size_t record_size;
    const uint8_t *raw=decode_record(cache,cp,&record_size);if(!raw)return false;
    if(record_size==UI_HANZI_RECORD)memcpy(record,raw,UI_HANZI_RECORD);
    else {
        memcpy(record,raw,5);memset(record+5,0,72);
        for(unsigned i=0;i<576;++i)if(((raw[5+i/2]>>((i%2)*4))&15)>=6)record[5+i/8]|=1u<<(i%8);
    }
    return record[2]>0&&record[2]<=24&&record[3]>0&&record[3]<=24&&record[4]==UI_HANZI_ADVANCE;
}
bool ui_hanzi_get_aa_cached(ui_hanzi_cache_t *cache,uint32_t cp,uint8_t record[UI_HANZI_AA_RECORD]) {
    if(!record)return false;
    size_t record_size;
    const uint8_t *raw=decode_record(cache,cp,&record_size);if(!raw)return false;
    if(record_size==UI_HANZI_AA_RECORD)memcpy(record,raw,UI_HANZI_AA_RECORD);
    else {
        memcpy(record,raw,5);memset(record+5,0,288);
        for(unsigned i=0;i<576;++i)if((raw[5+i/8]>>(i%8))&1)record[5+i/2]|=15u<<((i%2)*4);
    }
    return record[2]>0&&record[2]<=24&&record[3]>0&&record[3]<=24&&record[4]==UI_HANZI_ADVANCE;
}

bool ui_hanzi_get(uint32_t cp, uint8_t record[UI_HANZI_RECORD]) {
    return ui_hanzi_get_cached(&s_ui_cache,cp,record);
}
size_t ui_hanzi_cache_release(ui_hanzi_cache_t *cache) {
    if (!cache) return 0;
    size_t bytes=cache->bytes+(cache->inflate?sizeof(tinfl_decompressor):0)+cache->packed_bytes;
    heap_caps_free(cache->block);heap_caps_free(cache->inflate);heap_caps_free(cache->packed);
    if (cache->font) fclose(cache->font);
    memset(cache,0,sizeof(*cache));
    return bytes;
}
