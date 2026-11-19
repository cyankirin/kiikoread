# 双板同步发布 runbook（pico-anki + metalio-anki）

> 中文：本 fork 维护两块板的 Anki 版固件——Pico（`boards/pico`，4 MiB 槽）与 Metalio S31
>       （`boards/metalio47`，双 5 MiB 槽）。上游每次发新基线时，两块板按同一份合并、同一次
>       CI 构建一起升级，并把在线刷机与 OTA 推送同时更新。本文是固定步骤清单。
> English: This fork ships Anki builds for two boards — Pico and Metalio S31. Every upstream
>          baseline is merged once and both boards are released together, with web flashing and
>          OTA pushes updated in the same pass.

## 线上拓扑（截至 rc114.02 / rc108.02）

| 站点 / 仓库 | 内容 |
| --- | --- |
| `cyankirin.github.io/kiikoread`（主站） | `/kiikoread-anki/` Pico 刷机页；`/metalio-anki/` S31 专用刷机页；`anki-update.json`（Pico OTA）；`metalio-anki-s31-update.json`（S31 OTA）；Releases 存两板镜像资产 |
| `cyankirin.github.io/kiikoreader-anki`（遗留镜像仓） | 存量设备的 Pico OTA 静态快照（不同步新版本） |
| 本 fork 仓库 | CI 双板构建（artifact `firmware-pico` / `firmware-metalio47`）；OTA 代码双通道（官方 / Anki）；站点暂存脚本与完整性闸门 |

设备端 OTA feed（`main/ota_online.h`）：两板的 Anki 通道都指向主站对应清单；官方通道沿用
上游 `update.json` / `metalio-s31-update.json`。同基线原版升级 + 「Anki 版固件存 TF 卡」
功能两板都已具备（S31 自 rc114.02 起）。

## 每次上游合并的固定步骤

1. **合并上游基线**：`git fetch origin --tags && git merge v0.3.3-rcXXX`。冲突时保留
   fork 特性（Anki、设置分组二级菜单、答案反白、OTA 双通道）。
2. **双板同时升版**：`boards/pico/board.json` 与 `boards/metalio47/board.json` 各升一位
   （官方 `rcXXX` → 本地 `rcXXX.01`，bugfix 依次 `.02`、`.03`）。`docs/anki-changelog.md`
   首条版本必须等于 Pico board.json（闸门拦截）。
3. **钉定基线复核**：对照上游 tag 重算 `tools/fixtures/pr59-driver-baseline.json` 的钉定
   文件，漂移者按合并字节登记（逐个 `git cat-file` 核对与 tag 完全一致）。
4. **宿主测试适配**：上游常不随代码更新 harness，按 CI 失败逐个适配（file_names /
   book_ui / tap_dispatch / fullscreen / reader_metadata / ota_ui）。本地用物化树
   （`board_source_view.py --board <board> --tests`）复跑。
5. **推送并等待 CI 双 job 全绿**（`build (pico)` + `build (metalio47)`）。

## Pico 发布（pico-anki）

1. 取 `firmware-pico` artifact：校验 ESP 镜像 magic、内嵌版本号等于 board.json、大小
   < 4 MiB；写入 `firmware/Read_Pico-<版本>.bin`（参考 `build/fetch_rc*.py`）。
2. 更新 `flash/anki/releases.json` 目录（当前 / 上一 / 两个可回退版本；manifest 路径必须
   与版本一一对应）。
3. GitHub Release：tag `anki-rcXXX.NN` + 上传镜像资产 + 正文（大小 / SHA256 / 分区余量）。
4. Pages 自动部署后，OTA feed 即为主站 `anki-update.json`。

## Metalio 发布（metalio-anki）

1. 取 `firmware-metalio47` artifact，重命名为 `kiikoread-metalio47-<版本>.bin`。
2. 在 **kiikoread 仓**发 Release：tag `metalio-s31-v<版本>`，资产含 app 镜像 +
   bootloader / partitions / ota_data（命名见 `flash/metalio-anki/release-assets.json`）。
3. 更新本仓 `flash/metalio-anki/`：`manifest.json`（版本 + 四部件 size/sha256）、
   `release-assets.json`（tag + 资产名 + size/sha256 + historical 归档）、`releases.json`
   （刷机页版本目录）、`flash/metalio-anki-s31-update.json`（OTA feed：url/size/sha256/
   minimum_base_version）。
4. 推送后 Publish 工作流部署 `/metalio-anki/` 与 feed；注意 `*.bin` 的 .gitignore 白名单
   （归档 boot 部件需 `!` 例外）。

## 发布后线上终验（双板都做）

- OTA 清单：版本 / size / sha256 与实际镜像一致；`notes` 含「官方/上游」提示词与「修复」。
- 镜像：Range 206、magic `e907024f`、`ver@48` 等于版本号、**全量 SHA256 与清单一致**。
- 刷机页：index 200、releases.json 目录含新版本、manifest 各部件 200。
- 真机：OTA 双通道检查 / 下载 / 同基线弹窗 / TF 备份装回。

## 闸门备忘（发布被拦时先查这里）

- changelog 首条版本 ≠ pico board.json → 拦。
- OTA notes 缺「官方/上游」提示词或「修复」→ 探针失败。
- feed 的 manifest 路径与版本不对应 → 暂存闸门拒绝（Pages 失败）。
- 镜像内 Anki 签名缺失（`app_anki`）→ metalio-anki 站点闸门拒绝（防误发原版）。
