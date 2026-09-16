# 真机开发证据（Evidence）

本目录归集**真机（黄山派 SF32LB52 手表）**的开发证据，对应大赛自查第 ④ 条
（真机照片/录屏/串口日志）与第 ① 条中"性能数据要有依据"的要求。

```
docs/evidence/
├── screenshots/     实机截图（390×450 面板抓帧）
├── serial-logs/     串口日志（启动、链路、时间同步、HTTP 代理、麦克风寄存器）
└── measurements/    测量数据与说明（含测量方式、原始数据、可复现脚本位置）
```

## 1. screenshots/ — 实机界面抓帧

| 文件 | 内容 | 备注 |
|---|---|---|
| `ui-01-watch.png` | 表盘：步数渐变弧环绕时钟 + 三列指标卡 | UI v4.0（AMOLED 深黑 + 圆润笔画数字） |
| `ui-02-run.png` | 跑步页：状态 pill + 大计时 + 距离/配速双卡 + 胶囊操作键 | |
| `ui-03-map.png` | 地图页：全屏地图 + 顶部悬浮状态胶囊 | 离线 GPX 轨迹 |
| `ui-04-compass.png` | 指南针：5°/10°/30° 三级刻度环 + 圆润方位数字 + 风向玫瑰 | 磁校准修复后 |
| `ui-05-stats.png` | 统计页：三张汇总卡 + 最近记录（含 5 条演示记录） | |
| `ui-06-settings.png` | 设置页：四张可点卡片 + 关于卡 | |
| `ui-07-ai-card.png` | **AI 通知卡片**（工具执行结果上屏） | 3 行自动折行 + 强调色边条 |
| `ai-card-live.png` | AI 卡片实机抓帧（另一帧） | |

抓帧方式：`src/devshot.h` 置 1 构建调试固件 → `tools/listen_ui.py <COM> <dir> <秒>`
（每 20 s dump 一帧 PPM，1 Mbaud 下约 5.3 s/帧）。详见 `docs/交接文档.md`。

## 2. serial-logs/ — 串口日志

| 文件 | 内容 |
|---|---|
| `boot-release-optimized.log` | 发布版启动日志：传感器就绪、GPX 载入、AI agent 就绪、资源优化后的启动 |
| `gateway-ping-link.log` | 帧链路：`SET_TIME` / `PONG uptime ... rtt=52 ms` / `gateway=present rx_frames=6 rx_bad=0` |
| `gateway-time-sync-http.log` | 时间同步（含时区 `tz=UTC-8:00`）+ `!http https://api.github.com/zen` → `HTTP 200` |
| `mic-bringup-registers.log` | 麦克风 bring-up：codec 寄存器转储（已配置但无转换数据，如实记录） |

## 3. measurements/ — 测量数据

| 文件 | 内容 |
|---|---|
| `README.md` | **测量方法 + 全部性能数据及依据**（Flash/SRAM、磁校准、链路 RTT、时间同步、帧率） |
| `mag-tilt-109samples.json` | 109 样本 3D 翻滚原始磁计数（用于最小二乘球面拟合） |
| `mag-tilt-after-fix.json` | 修复后同姿势采样（\|B\| 离散度对照） |

## 4. 复现入口（仓库内脚本）

| 脚本 | 用途 |
|---|---|
| `tools/link_test.py` | 帧协议自测（PING/命令/ACK/RTT 统计） |
| `tools/gateway.py` | PC 网关：帧编解码 + 时间同步 + HTTP 代理 + LLM + 日志解复用 |
| `tools/mic_test.py` | 麦克风在线调试（`!mic regs/poll/sweep`） |
| `tools/listen_ui.py` / `tools/ui_check.py` | 实机抓帧与量化检查（主色/ASCII 缩略核对） |

## 5. 需另外补充（人工）

- **实机照片/录屏**：按 `docs/演示视频脚本.md` 拍摄（≤5 分钟，重点演示 AI 主动提醒 + 工具执行 + 联网取数）。
- 视频建议先执行 `!timer 1` 以便在镜头内触发"主动提醒"。
