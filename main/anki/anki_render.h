/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 卡片模板渲染。字段替换、条件段、cloze 与 hint，再把生成的 HTML
 *       交给 html_text 转成可排版的纯文本。
 * English: Anki card template rendering. Field substitution, conditional
 *          sections, cloze and hint, then hands the generated HTML to
 *          html_text for plain-text layout.
 *
 * 冻结：不执行脚本、不加载网络资源；type: 输入域与媒体取占位文本。
 * Frozen: never executes scripts or loads network resources; type: inputs and
 * media degrade to placeholder text.
 */
#pragma once

#include <stddef.h>

#include "anki_apkg.h"
#include "esp_err.h"

#define ANKI_CARD_TEXT_MAX (16U * 1024U)
/// 填空式正面的段落上限（一首诗的行数远小于此）。/ Paragraph cap for the
/// fill-in front; a poem's line count is far below this.
#define ANKI_FILLIN_PARAS 64

/// 卡片内容来源：导入结果与已载入卡组共用这一视图。
/// Card content source: import results and loaded decks share this view.
typedef struct {
    const anki_notetype_t* notetypes;
    int notetype_count;
    const anki_note_t* notes;
    int note_count;
} anki_collection_t;

/// 从导入结果建视图。/ Build a view from an import result.
void anki_collection_from_import(const anki_import_t* imp, anki_collection_t* out);

/// 渲染卡片正面（问题）为纯文本；out 由调用方持有并 NUL 结尾。
/// Render the card front (question) to plain text; out is caller-owned and NUL-terminated.
esp_err_t anki_render_front(const anki_collection_t* col, const anki_card_t* card, char* out,
                            size_t cap);

/// 渲染卡片背面（答案）；front_text 为已渲染正面，用于 {{FrontSide}}。
/// Render the card back (answer); front_text is the rendered front for {{FrontSide}}.
esp_err_t anki_render_back(const anki_collection_t* col, const anki_card_t* card,
                           const char* front_text, char* out, size_t cap);

/// 是否填空式卡片：字段里带「空条 + 隐藏答案」结构
/// （`<span class="d" style="width:…em"></span><span class="t">答案</span>`）。
/// 这类模板在 Anki 里靠 CSS 隐藏答案，纯文本渲染会把答案直接印在正面，所以
/// 正面改走 anki_render_fillin。
/// Fill-in card test: the fields carry the "blank bar + hidden answer" pair
/// used by recall templates. Anki hides the answer with CSS; plain-text
/// rendering would print it on the front, so such cards use anki_render_fillin.
bool anki_card_fillin(const anki_collection_t* col, const anki_card_t* card);

/// 填空式正面：逐段输出，reveal_mask 第 n 位为 1 的段落显示答案，其余画空条。
/// para_off 收到每段在 out 里的起始字节，para_blank 标记该段是否含空条，
/// para_count 返回段数（不超过 para_cap）。段与段之间以 '\n' 分隔。
/// Fill-in front: one entry per paragraph. Bit n of reveal_mask shows the nth
/// paragraph's answers; the rest stay blank bars. para_off receives each
/// paragraph's byte offset in out, para_blank flags whether it holds a blank,
/// and para_count returns the paragraph count (bounded by para_cap). Paragraphs
/// are separated by '\n'.
esp_err_t anki_render_fillin(const anki_collection_t* col, const anki_card_t* card,
                             uint64_t reveal_mask, char* out, size_t cap, int* para_off,
                             uint8_t* para_blank, int para_cap, int* para_count);
