# 性能与测量数据（含测量方式与佐证）

> 所有数据均为**本机真机实测**，附测量命令或日志文件，便于评审核对。
> 日志文件位于 `docs/evidence/`。

## 1. 资源占用（编译器链接输出）

测量方式：VM 构建目录执行
`export PATH=/home/openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH && arm-none-eabi-size nuttx`
以及构建时的内存区域报告（`Memory region / flash / sram`）。

| 指标 | 数值 | 说明 |
|---|---|---|
| Flash 占用 | **5,800,444 B / 16 MB = 34.6%** | 发布版 `nuttx.bin`（含全部 UI、Fusion、AI agent、链路层） |
| SRAM 占用 | **205,840 B / 512 KB = 39.3%** | 优化前为 428,800 B（81.8%） |
| BSS 削减 | **≈228 KB** | 关 `CONFIG_ALLSYMS`（`g_allsyms` 125,736 B）+ GPX 缓冲 4096→1024 点（`g_gpx` 137,312 B）+ 航迹环 2048→1024（`run.track` 32 KB） |
| 测量依据 | `nm --size-sort` 输出 | `g_allsyms`/`g_gpx`/`g_app` 的符号大小 |

## 2. 磁力计校准（本项目的核心硬件问题）

测量方式：`APP_DIAG_VERBOSE=1` 构建，串口抓 `[MagS]`/`[Fuse]` 行；
翻滚采样原始 20bit 计数（0.5 s 一条），PC 端做最小二乘球面拟合。

| 指标 | 修复前 | 修复后 | 佐证文件 |
|---|---|---|---|
| 磁场总强度 \|B\|（同一姿势） | 4.5–83.4 µT（σ=20.0） | **35–56 µT**（σ=0.17 静止时） | `measurements/mag-tilt-109samples.json`、`mag-tilt-after-fix.json` |
| Fusion 磁误差 `em` | **90°**（长期） | **0–1°** | 串口 `[Fuse] ... em=` 日志 |
| 静止 yaw | 自行漂移 0°→175° | 稳定 | 串口 `[Fuse] yaw=` 日志 |
| 拟合球心/半径 | 旧值 (−3048,3101,−9498) | **(−4169,+1447,−5135) counts，半径 47.5 µT** | 拟合脚本输出（半径=当地地磁总场，31°N） |
| 桌面磁异常 | — | **184 µT（地磁 3.7 倍）** | 串口 `[MagS]` 日志 |

结论：\`|B|` 是与朝向无关的标量，修复后在不同朝向下稳定在 35–56 µT，且拟合半径等于当地地磁总场——这是校准正确的物理自证。

## 3. 串口链路（联网功能）

测量方式：`tools/link_test.py`（帧协议自测）与 `tools/gateway.py`（网关）。

| 指标 | 数值 | 佐证文件 |
|---|---|---|
| 帧 ACK 成功率 | 6/6（含 WHOAMI/PING/文本命令） | `serial-logs/gateway-ping-link.log` |
| PING→PONG 往返 | **52–79 ms** | `serial-logs/gateway-ping-link.log` |
| 时间同步 | 与 PC 同秒：`clock set to 2026-09-15 17:17:26 (tz=UTC-8:00)` | `serial-logs/gateway-time-sync-http.log` |
| 真实互联网访问 | `!http https://api.github.com/zen` → **HTTP 200** + 正文 | `serial-logs/gateway-time-sync-http.log` |
| LLM 通道 | `?question` → `@LLMREQ` → DeepSeek → 回传 | 同上（本轮未带 key 时返回 `(no api key)`） |
| 控制台 RX 特性 | 63 字节突发仅 2–19 字符到达；**逐字节 1 ms 间隔 63/63** | 实验记录（`tools/link_test.py`、见 `docs/联网功能与串口根因-2026-09-15.md`） |

> 说明：PC→手表方向因 CH340/控制台 RX"等效 1 字节深"，网关采用**逐字节节流写**（1.2 ms/字节 ≈ 830 B/s），
> 上层再叠加 CRC16 + ACK + 重传，因此**应用层不会丢命令**；设备→PC 方向不受限（实测曾跑通 526 KB 帧传输）。

## 4. 渲染与功耗

| 指标 | 数值 | 测量方式 |
|---|---|---|
| 渲染帧率 | 目标 60 fps；实测 15–33 fps（按页面/动画变化） | 串口 `[Render] fps=`（`APP_DIAG_VERBOSE=1`） |
| 每帧清屏写量 | 390×450×2 B = **351 KB/帧** | 分辨率与像素格式（RGB565）计算 |
| 功耗估计 | `[Render] ... est=21mA`（屏幕点亮、中等刷新） | 串口日志 + `power_manager` 估算模型 |

## 5. 麦克风 bring-up（进行中，如实记录）

| 项目 | 状态 | 佐证 |
|---|---|---|
| 内部 AUDCODEC 寄存器配置 | ✅ 已完成（`CFG=1` ADC 使能、`ADC_CFG=0x00000a41`、`CH0/CH1_CFG` 已配、PLL 锁定、BG 开启、DMA circular 已武装） | `serial-logs/mic-bringup-registers.log` |
| 采样数据 | ❌ 未产生（`ADC_CH0/CH1_ENTRY` 恒 0、DMA 无请求） | 同上 |
| 已排除 | RCC 模块时钟未开、PM 休眠门控、DMA 未武装、时钟配置(Tried 12 组)、数字通道 0/1 映射、崩溃/中断风暴 | `docs/麦克风硬件与实现方案.md` §6 |

## 6. 数据可复现性

- 磁力计拟合：`docs/evidence/measurements/*.json`（原始 20bit 计数）+ `docs/指南针病因分析-2026-09-15.md` 中的方法说明
- 链路：`tools/link_test.py`、`tools/gateway.py`、`tools/mic_test.py`（仓库内，可直接运行复现）
- 固件资源：VM 构建日志（`Memory region` 报告）与 `arm-none-eabi-size`
