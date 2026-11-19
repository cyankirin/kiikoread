/* SPDX-License-Identifier: Apache-2.0
 * 中文：外置全量补字的启动后台同步。挂载内置存储后校验 /flash/fonts/ui-hanzi.bin，
 *       缺失或校验不符时经 WiFi 从白名单站点静默下载固定版本文件；失败保持内嵌
 *       PIF1 降级，不阻塞 UI、不弹任何提示。只启动一个任务，正常开机文件有效时不
 *       连接 WiFi。
 * English: Startup background sync for the external full supplement. After mounting the
 *       internal storage it validates /flash/fonts/ui-hanzi.bin and, when missing or
 *       failing verification, silently downloads the version-pinned file over WiFi from
 *       an allowlisted origin. Failures keep the embedded PIF1 fallback without blocking
 *       or notifying the UI. At most one worker; boots with a valid file never touch WiFi.
 */
#pragma once

/// 启动后台补字同步；幂等，仅首个调用创建任务。
/// Start the background supplement sync; idempotent, only the first call spawns the worker.
void ui_hanzi_sync_start(void);
