# contest2026_192_DX — 黄山派智能跑步手表

> openvela AI 硬件开发者大赛（2026）参赛作品（队伍 192）

## 一、作品信息表

| 项目 | 内容 |
|---|---|
| 作品名称 | 黄山派智能跑步手表（Huangshan Running） |
| 参赛赛道 | AI 硬件产品创新（如评审建议赛道不符，可改） |
| 队伍名称 / 编号 | DX队 / contest2026_192_DX |
| 队长 | 晏宏旭（全队唯一成员，负责硬件/嵌入式/算法/UI/AI 协作全流程） |
| 开发板 | LCKFB 黄山派 Pi（SF32LB52，openvela/NuttX，官方申领板） |
| 屏幕 / 传感器 | 390×450 AMOLED；LSM6DS3 IMU、MMC5603NJ 磁力计、LTR303 光感、FT6146 触摸 |
| 运行方式 | NuttX 应用 `hello_app`（contest2026_192_DX 通过 manifest 软链到 packages/demos），详见 `app/hello_app/README.md` |
| 提交内容 | 源码（本仓 `app/hello_app/src` 全量 C 源码）、AI Coding 日志（见 `logs/` 与 `AI-CODING-SETUP.md`）、本 README、演示视频（另附） |

## 二、作品简介

**AI Agent（AI 硬件赛道要求：主动 + 执行）**：手表固件内实现 `src/ai_agent.c`，
在设备上运行 Agent（工具调用 + 主动引擎 + 串口 CLI 渠道）；LLM 出口由 PC 侧
`tools/llm_relay.py` 经串口中继到 DeepSeek（详见 `docs/AI-AGENT.md`）。
- 执行：`!start/!stop/!status/!timer/!tip` 真机调用跑步引擎、提醒与状态上报
- 主动：倒计时到点主动提醒；空闲 3 分钟主动生成教练提示并弹屏幕卡片


一块「从零自研」的跑步手表固件：表盘、跑步（实时死推轨迹）、路线预览、统计、设置与**磁力计指南针**六页 UI，
物理按键翻页（KEY1/KEY2 短按）、触摸只用中轴大按钮，全程无长按、无四角控件。

核心亮点：
- **9 轴姿态航向**：xioTechnologies/Fusion AHRS（NED）融合 IMU+磁力计；磁力计底层按数据手册寄存器重写
  （20-bit 解包、AUTO_SR 每次去磁、饱和样本丢弃），弱场/强干扰下自动降级纯陀螺并在开机窗口锚定真北。
- **程序内自动校准**：无需手势，运动中后台做 3D 覆盖门控的球心拟合收敛硬磁偏移（串口 `[AC]` 可观测），
  另有 15s figure-8 全屏校准引导（倒计时+XYZ 覆盖条）；支持磁偏角（真北）调节。
- **无 GNSS 航迹**：重力投影偏航率 + 静止回零偏置的自研航位推算（dead reckoning），跑步页实时渲染轨迹。
- **UI/交互细节**：极简深色、所有辅助文字 2× 放大（10×14px）、严格圆角安全区（四角及邻域禁放内容）、
  地图罗盘/比例尺居中轴、演示时钟（无 RTC 偏移）、按键全导航、无长按。

## 三、目录说明

```text
app/hello_app/            手表固件源码（src/ 34 个 C/H + fusion/ Fusion AHRS）
skills/huangshan-watch-dev/  沉淀的开发 Skill（SKILL.md）
AI-CODING-SETUP.md        AI Coding 日志归集操作手册
logs/                     AI 会话日志（由官方采集器写入/按手册手动补充）
docs/                     （可放介绍文档草稿）
```

## 四、如实说明（不夸大）

- 指南针为**磁场绝对方向 + 陀螺短时维持**：靠近金属/大电流时磁场畸变会被门控识别并降级为 GYRO ONLY（读数为相对航向），
  这是该类传感器的物理边界；干净环境下校准后与手机可对照。
- 板载无 RTC/无 GPS/无心率：表盘时间采用演示偏移；轨迹为 IMU 航位推算精度，非米级。
- 板子无 WiFi，且仅一个 Type-C（CH340 串口，SoC USB 未引出）→ **LLM 网络出口经 PC 中继**
  （Agent/工具/主动逻辑全部在设备上运行）；SLIP/PPP 直连主机的代码保留在
  `src/net_link.c`/`src/ppp_link.c`，未作为交付路径。
- 电池芯片（AW32001）监测未接入（未装电池），电源管理按 USB 供电运行。
- 触屏 UI 依据真机截图（OCR 复核）迭代；演示前请自检各页。

## 五、相关官方文档

- 大赛总览 / 提交流程 / AI 日志手册见 openvela docs `dev-ai-contest-2026` 分支的 `contest_2026/`。
