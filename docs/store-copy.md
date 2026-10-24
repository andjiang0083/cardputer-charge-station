# M5Burner 商店文案（待用户确认后再提交）

平台字段：

| 字段 | 值 |
|---|---|
| 名称 | `Charge Station` |
| 设备 | Cardputer / Cardputer-Adv |
| 分类 | Utility |
| 版本 | `1.0.11` |
| 可见性 | Public |
| 安装形态 | **merged 全镜像（商店烧 0x0）** —— 覆盖式安装，会清掉原有 Launcher/配置（商店固件常态） |
| 封面 | `releases/cover-1280x720-cardputer.png`（1280×720，已过 cover-gate） |
| 项目链接 | https://github.com/andjiang0083/cardputer-charge-station |

---

## 描述（中文）

把 Cardputer 变成一台会告诉你「到底在不在充」的充电座。
插上电、熄屏、放一夜 —— 它只做两件事：把能省的电全省给电池，把真相画在屏幕上。

一眼看懂：橙黄 = 正在充 · 绿 = 已充满 · 红 = 接着但没电流 · 灰 = 没在充
40 分钟电压曲线 · 按任意键 3 秒读数 · 整夜记录可插回电脑回读
中英双语开机自选 · 灭屏守候 · 还有多久充满

本版新增：净充电电流估算（`~6x mA` 量级，按斜率反推）、`9` 键清空整夜记录。

⚠️ 开关必须拨到 ON。OFF 是断开电池，物理上就充不进去 —— 这是「充不进电」最常见的原因 ⚠️

github.com/andjiang0083/cardputer-charge-station
想保留你原来的 Launcher 和配置？从 GitHub Release 取 app 版 bin，用 M5Launcher 安装即可。

---

## Description (English)

Turn your Cardputer into a charging dock that tells you the truth.
Plug it in, let the screen go dark, leave it overnight — it does two things only: give every spare milliamp to the battery, and draw the truth on screen.

Amber = charging · Green = full · Red = plugged but no current · Grey = not charging
40-minute voltage sparkline · one key press for a 3 s readout · night-long log you can read back over USB
Bilingual UI (中文 / English), picked on first boot · dark-screen standby · time-to-full estimate

Now with an estimated net charge current and a one-key log clear.

⚠️ The power switch must be ON. OFF disconnects the battery — nothing can charge, no matter how good the charger ⚠️

github.com/andjiang0083/cardputer-charge-station
Want to keep your existing Launcher and settings? Grab the app build from GitHub Releases and install it through M5Launcher.

---

## 本版说明（versionDescription）

**中文**
`v1.0.11 Adds 净充电电流估算 + 9 键清空整夜记录；Fixes 选语言页按 1/2 无反应、页内缺 [2] 标签`

**English**
`v1.0.11 Adds estimated net charge current and one-key log clear; Fixes the language picker ignoring 1/2 and missing its [2] label`

---

## 自检（机器可判部分）

- 描述非空、中英双语各自成立 ✅
- 首句是画面不是原理 ✅
- 有 Now with / Adds 制造更新感 ✅
- ⚠️ 前置条件只写一条会导致翻车的事（开关必须 ON）✅
- 不含版本号在封面、不含硬件参数承诺（500mA / 实测电流一律不提）✅
- 每条 changelog 非空、用户视角 ✅
