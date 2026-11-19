/* SPDX-License-Identifier: Apache-2.0
 * 中文：内建黑体的常用汉字位图补充，不依赖 TF 卡。/ English: Built-in common Han glyph supplement, independent of the card.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define UI_HANZI_BASE_PX 24
#define UI_HANZI_RECORD 77
#define UI_HANZI_AA_RECORD 293
// 现有GB2312补字在24px基准下统一前进17px。/ Shipped GB2312 advances are17px at24px base.
#define UI_HANZI_ADVANCE 17

// 外置全量补字的分发路径与期望属性；SHA-256 期望值固定在 ui_hanzi.c 内。
// Distributed full-supplement path and expected properties; the expected SHA-256 is pinned inside ui_hanzi.c.
#ifndef UI_HANZI_FLASH_FONT_PATH
#define UI_HANZI_FLASH_FONT_PATH "/flash/fonts/ui-hanzi.bin"
#endif
#define UI_HANZI_FONT_BYTES 688195u

/// 各缓存须零初始化且不跨任务共享，仅共用只读字库。/ Zero-init per-task caches; share immutable bytes only.
/// generation/font/packed 服务外置文件模式，源切换或释放时自动重建。
/// generation/font/packed serve the external file mode; rebuilt on source switches and release.
typedef struct {
    uint8_t *block; void *inflate; size_t cached, bytes; bool valid;
    uint32_t generation; FILE *font; uint8_t *packed; size_t packed_bytes;
} ui_hanzi_cache_t;
/// 从指定独立缓存读取一字。/ Read one glyph using the supplied independent cache.
bool ui_hanzi_get_cached(ui_hanzi_cache_t *cache, uint32_t cp, uint8_t record[UI_HANZI_RECORD]);
/// 返回16级覆盖率，PIF1按原黑白值展开。/ Return16-level coverage; expand legacy monochrome unchanged.
bool ui_hanzi_get_aa_cached(ui_hanzi_cache_t *cache,uint32_t cp,uint8_t record[UI_HANZI_AA_RECORD]);
/// 释放工作区并返回回收字节数。/ Release workspace and return reclaimed bytes.
size_t ui_hanzi_cache_release(ui_hanzi_cache_t *cache);
/// 只检查索引，不分配缓存。/ Index lookup without allocating a cache.
bool ui_hanzi_has(uint32_t cp);
/// 记录包含 x/y 偏移、宽高、字宽和 24×24 单色位图；只解压所在小块。
/// Record: x/y offsets, width/height, advance and a 24x24 mono bitmap; decompress its small block only.
bool ui_hanzi_get(uint32_t cp, uint8_t record[UI_HANZI_RECORD]);
/// 校验一个候选补字文件（头、长度与全文件 SHA-256），不改变当前数据源。
/// Validate a candidate supplement file (header, length and whole-file SHA-256) without switching sources.
bool ui_hanzi_font_file_valid(const char *path);
/// 重新探测外置补字并按需切换数据源；返回切换后是否启用外置全量版。
/// Re-probe the external supplement and switch sources if needed; returns whether the full external set is active.
bool ui_hanzi_source_refresh(void);
