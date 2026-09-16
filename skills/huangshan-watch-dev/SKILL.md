---
name: huangshan-watch-dev
description: 黄山派 SF32LB52 智能手表固件开发流水线（仅改本地工作副本→同步 VM→编译→烧录→串口/截图验证）。When the user works on huangshan_running / contest2026_192_DX watch firmware, use this skill to keep the build-flash-verify loop correct and avoid known pitfalls.
---

# Huangshan Watch Dev (contest2026_192_DX)

沉淀「openvela 黄山派 SF32LB52 手表」从代码到真机的完整开发流程，避免每次重复踩坑。

## 触发词

huangshan / 黄山派 / SF32LB52 / 指南针 / compass / mag / MMC5603 / watch UI / 手表固件 / 烧录 / 轨迹 / 跑步页
同步 VM / 编译烧录 / serial 验证

## 拓扑速查

| 项 | 值 |
|---|---|
| 唯一合法工作副本 | `D:/共享文件夹/app/app/huangshan_running/`（两层 app/app，勿动 `_archive/` 与另一份 `app/huangshan_running`） |
| VM | 最近为 `dingxu@192.168.4.29`（DHCP 会变！不通先 `ip addr`/ARP 找新 IP，再改本文件），工程 `/home/openvela`，构建目录 `cmake_out/lckfb_huangshan_pi` |
| 串口 | COM4 @1Mbps 8N1（会变；主机侧必须**开端口前**置 `dtr=False/rts=False`，否则开端口即复位板子）（会随插拔变化，sftool 会报 Available ports）；打开端口即 RTS 复位 |
| 屏幕 | 390×450 AMOLED RGB565 `/dev/lcd0`；源码 UI 无 RTC → 表盘演示时间在 pages.c 有 demo 偏移 |
| 传感器 | LSM6DS3 IMU + MMC5603NJ 磁力计（I2C0x30）+ LTR303；磁→设备轴映射 `(-x,+y,-z)` |

## 开发流程（每次必做）

1. 只改 `D:/共享文件夹/app/app/huangshan_running/src/*.{c,h}` 与 `fusion/`。
2. 本地检查：`py -3 D:/共享文件夹/tools/check_balance.py <改的文件>`。
3. 同步到 VM（含 fusion 目录）：
   ```bash
   scp src/*.c *.h dingxu@VM:/home/openvela/apps/examples/huangshan_running/src/
   scp src/fusion/*.c *.h dingxu@VM:/home/openvela/apps/examples/huangshan_running/src/fusion/
   ```
4. VM 编译：
   ```bash
   export PATH=/home/openvela/prebuilts/.../cmake...:.../build-tools...:.../gcc/...arm-none-eabi/bin:$PATH
   cd /home/openvela/cmake_out/lckfb_huangshan_pi
   cmake --build . --target nuttx        # 关掉 CONFIG_ALLSYMS 后目标名是 nuttx（不再是 final_nuttx）
   arm-none-eabi-objcopy -O binary nuttx nuttx.bin
   ```
5. 取回并烧录：`ssh ... cat nuttx.bin > /c/sf/img/nuttx.bin`；`cd /c/sf && ./sftool.exe -c SF32LB52 -p COM3 -m nor -b 1000000 ... write_flash "C:/sf/img/nuttx.bin@0x12010000"`。
6. 串口验证（tools/capture_boot.py / serial_monitor.py）：查 hardfault、`[Mag] factory cal off=(-4169,1447,-5135)`（2026-09-15 球面拟合重标定后的值）、`[Fuse]`2Hz、`drp=`；UI 抓帧：`src/devshot.h` 置 1 → 每 20 s 自动 dump 一帧（置 2 才会翻页）→ `tools/listen_ui.py COMx <dir> <秒>`；发布固件必须置 0。

## 输出规范

- 每次代码改动给出：改了什么文件/原因/验证结果（编译 md5、串口关键行、截图 OCR）。
- 指南针/磁力计改动后必须看串口：`drp=`(0=磁参与)、`em`(磁残差)、factory off、`[AC]` 自动校准是否 APPLIED、yaw 是否稳定。
- UI 布局要求：四角不放控件、动作只用中轴大按钮、无长按手势、页面跳转只用 KEY1/KEY2 短按。
- 禁止直接改官方仓库编译树：生产仓库零改动，作品只放本仓 `app/hello_app/`，由 manifest 软链映射。

## 常见坑

- 输入 ui_text/ui_digit 的 buf_w 必须为真实帧宽 390（行距），列居中用 num_at_center/text_at_center；传列宽会花屏。
- 表盘时间无 RTC：演示靠 pages.c 的 `demo_off` 偏移。
- 磁被弱场/金属丢(`drp=1`)属正常降级（GYRO ONLY）；校准需 3D 翻滚 + 远离金属，覆盖不足会被拒绝。
- VM/串口 IP 与 COM 号会变，先探测再执行。
- 提交竞赛仓库前：移除临时调试代码（截图导出/自动翻页/静默打印），并把源码同步至 `contest2026_192_DX/app/hello_app/src`。

## UI 硬性规范（评审需求）

- **四角圆角区禁放任何内容**：可见区为圆角矩形（半径≈56px）。底部宽元素（按钮/信息行/列表）纵向必须结束在 y≤390；四角象限（x<56 或 x>334 且 y<56 或 y>394）不得有像素。渲染器自带角落元素也要挪：地图罗盘=`renderer_draw_compass` 已移到**顶部中央**，比例尺移到**底部中央**（y=H-150），海拔条上移到 H-172。
- **字号下限**：辅助文字一律 `ui_text_scaled(...,2,...)`（10×14px）；原生 5×7 太小。新增了 `text2_center/text2_at/text2_at_center` 辅助。
- **交互**：四角无控件；动作用中轴大按钮；**无长按**；页面跳转仅 KEY1/KEY2 短按。
- **验证方式**：开发版 `src/devshot.h` 把 `HUANGSHAN_DEV_SHOT` 置 1 → 板子每 5s 自动翻页+导出一帧 PPM → `tools/listen_ui.py COM3 photos 70` 存 PNG → 用 vision 技能 OCR 复核「四角是否有内容/字号/重叠」。**发布构建必须置 0**（当前已置 0）。

## 关键新知识（2026-09-15 实测沉淀，务必先读）

### 1. 串口控制台 RX 只有"1 字节深"
PC→设备方向背靠背写入会互相覆盖：实测 63 字节突发仅 2~19 个字符到达；4 字节一组时"每块只活最后 1 个"；
**逐字节间隔 1 ms 发送则 63/63 零丢失**。因此所有 PC 侧工具（`tools/gateway.py`、`link_test.py`、`mic_test.py`）
都必须 `write_paced()`（1.2 ms/字节），上层再叠 CRC16+ACK+重传。设备→PC 方向不受限（曾跑通 526 KB 帧）。

### 2. NSH 抢占控制台
`/etc/init.d/rcS` 只做 `huangshan_run &`，之后 NSH 交互 shell 仍读 `/dev/console`，与应用抢字节。
应用启动时 `console_takeover()` 用 `nxsched_foreach()` 找 `nsh` 任务并 `kill(pid, SIGSTOP)`（`SIGCONT` 可恢复）。

### 3. 资源与诊断开关
- `src/app_diag.h`：`APP_DIAG_VERBOSE`（0=发布静默，1=打印 `[MagS]`/`[Fuse]`/`[Alive]`/`[Render]`）。
- 板级 defconfig 关了 `CONFIG_ALLSYMS`（省 125 KB）+ GPX/轨迹缓冲缩容（共省 ~228 KB SRAM）。
- 发布固件：`HUANGSHAN_DEV_SHOT 0` + `APP_DIAG_VERBOSE 0` + `APP_DEMO_SEED 0`。

### 4. 电源管理会门控 codec 时钟
`power_manager` 在 IDLE/SLEEP 会关外设时钟；录音等长任务必须 `pm_report_activity()`
（`mic_audcodec.c` 通过 `mic_set_activity_hook()` 在采集路径自动保活）。

### 5. 联网 = 帧协议 + PC 网关（不是板载 IP）
板子无 WiFi、蓝牙栈为 BLE-only（无 BR/EDR → BT-PAN 不可能）→ 唯一链路是 CH340 串口。
`src/link.c`（帧+CRC+ACK/重传+通道）+ `tools/gateway.py`（时间同步/HTTP 代理/LLM/文件通道预留）；
设备命令：`!link` `!time` `!http <url>` `?问题`。

### 6. 麦克风（板载 MEMS）bring-up 现状
内部 AUDCODEC 的 HAL **已编进 arch 库可直接调用**（无需搬源码/Kconfig）；已补 RCC 模块时钟、
`bf0_enable_pll()`、`Clear_All_Channel`、`ADCPath_Volume`，寄存器状态正确、DMA 已武装，
但 **codec 仍不产生转换数据**（`ADC_CH0/CH1_ENTRY` 恒 0）。调试工具：`!mic regs|poll|sweep|ch0|ch1`。
公开 SDK 无内部 codec 驱动（只有 DA7212）→ 需向 SiFli/立创索要 bring-up 片段。详见 `docs/麦克风硬件与实现方案.md`。

### 7. 提交材料位置（本仓）
`docs/作品介绍.md`、`docs/evidence/`（截图/串口日志/测量数据）、`docs/指南针病因分析-*.md`、
`docs/联网功能与串口根因-*.md`、`skills/huangshan-watch-dev/`、`logs/DingXu-Hub/`（AI Coding 日志）。
