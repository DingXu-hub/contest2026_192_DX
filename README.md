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
| 提交内容 | 源码（本仓 `app/hello_app/src` 全量 C 源码）、AI Coding 日志（见 `logs/` 与 `AI-CODING-SETUP.md`）、本 README、技术报告与作品介绍（仓根 `*.docx/*.pdf`）、真机证据（`docs/evidence/`）、自建 Skill（`skills/`）、演示视频与硬件照片（另附） |

## 二、作品简介

**AI Agent（AI 硬件赛道要求：主动 + 执行）**：手表固件内实现 `src/ai_agent.c`，
在设备上运行 Agent（工具调用 + 主动引擎 + 串口 CLI 渠道）；LLM 出口由 PC 侧
`tools/llm_relay.py` 经串口中继到 DeepSeek（详见 `docs/AI-AGENT.md`）。
- 执行：`!start/!stop/!status/!timer/!tip` 真机调用跑步引擎、提醒与状态上报
- 主动：倒计时到点主动提醒；空闲 3 分钟主动生成教练提示并弹屏幕卡片


一块「从零自研」的跑步手表固件：表盘、跑步（实时死推轨迹）、路线预览、统计、设置与**相对方位**六页 UI，
物理按键翻页（KEY1/KEY2 短按）、触摸只用中轴大按钮，全程无长按、无四角控件。

核心亮点：
- **6 轴姿态（陀螺 + 加速度，官方 Fusion 链路）**：`src/attitude6.c` 薄封装 xioTechnologies/Fusion 的
  无磁力计路径（`FusionBiasUpdate` → `FusionAhrsUpdateNoMagnetometer` → `FusionQuaternionToEuler`，NED、
  `gyroscopeRange=2000 dps`、`accelerationRejection=15°/5 s`、每帧真实 dt）。滚转/俯仰由重力给绝对基准，
  航向为**相对方位**（无磁力计参与）并在 UI 标注 `6-AXIS REL` + 中轴 `SET 0` 归零。
- **合成真值自检**：`attitude6_selftest()`（设备上 `!att test`）用已知姿态与角速度生成输入，
  比对官方链路输出，9 个用例覆盖平放/左右倾/抬头/复合姿态/偏航积分/带倾角转弯/毛刺抑制 → 实测 **ALL PASS**。
  它同时固定了符号、单位与坐标系，并拓出了官方无磁路径“启动 3 s 内把航向强制归零”的行为。
- **毛刺迟滞过滤**（官方库没有对应机制）：本板陀螺有 18–30 dps 的单样本读毛刺，官方超量程检查要到
  ±1960 dps 才触发；单样本 >4×EMA+3 dps 丢弃，连续 3 个超阈则判为真实运动（150 ms 延迟）。
  真机 3 分钟静止实测 yaw 漂移 **+0.03~+0.10 °/min**（未过滤时 +1.2 °/min）。
- **无 GNSS 航迹**：重力投影偏航率（已去手臂摆动分量）+ 静止零速修正的航位推算；方位角随真实转向弯曲，
  死区 1°、顺时针为正；起跑以相对 yaw 为基准（无绝对北向）。
- **UI/交互细节**：极简深色、所有辅助文字 2× 放大（10×14px）、严格圆角安全区（四角及邻域禁放内容）、
  地图罗盘/比例尺居中轴、按键全导航、无长按。

## 四、构建与复现（评审可重建）

**板级目标**：`lckfb_huangshan_pi`（SF32LB52，openvela/NuttX）。本仓只含应用与板级覆盖，
其余（nuttx/sifli SDK/prebuilts）由 `contest2026_192_DX.xml` 的 `openvela.xml` 子清单获取。

```bash
# 0) 取代码（或直接用构建环境里已就绪的工作区）
repo init -u <openvela-manifest> -m contest2026_192_DX.xml && repo sync -c

# 1) 把本仓应用放进构建树（manifest 已 linkfile；手工方式）：
#    app/hello_app  ->  apps/examples/huangshan_running   （软链或拷贝）
#    板级覆盖见 board/huangshan_pi/README.md

# 2) 构建（VM 上验证过的命令）
export PATH=/home/openvela/prebuilts/cmake/linux-x86_64/bin:\
/home/openvela/prebuilts/build-tools/linux-x86_64/bin:\
/home/openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH
cd <build_dir> && cmake --build . --target nuttx \
  && arm-none-eabi-objcopy -O binary nuttx nuttx.bin

# 3) 烧录（Windows 侧；COM 口自动识别 CH340，开端口前勿拉 DTR/RTS）
./sftool.exe -c SF32LB52 -p COM4 -m nor -b 1000000 \
  --before default_reset --after no_reset write_flash "nuttx.bin@0x12010000"
```

**复现资源数字的关键板级开关**（详见 `board/huangshan_pi/README.md`）：`CONFIG_ALLSYMS` 关（省 125 KB）、
`GPX_MAX_TRACKPOINTS 1024`、`GPX_MAX_WAYPOINTS 128`、`RUN_TRACK_MAX 1024`、`CONFIG_DEBUG_NET*` 关。
据此可复现 **SRAM 81.8% → 38.2%（200,464 B / 512 KB）、Flash 34.6%**。

**PC 侧工具**：`tools/gateway.py`（帧协议 ⇄ 串口 ⇄ HTTP/LLM，逐字节节流写）、`tools/att_still.py`（静止漂移实测）、
`tools/attitude_test.py`、`tools/ui_check.py`、`tools/fill_report.py`。

## 五、目录说明

```text
app/hello_app/            手表固件源码（src/ 34 个 C/H + fusion/ Fusion AHRS）
skills/huangshan-watch-dev/  沉淀的开发 Skill（SKILL.md）
AI-CODING-SETUP.md        AI Coding 日志归集操作手册
logs/                     AI 会话日志（由官方采集器写入/按手册手动补充）
docs/                     （可放介绍文档草稿）
```

## 六、如实说明（不夸大）

- **航向是相对方位**：磁力计不参与航向路径（本机硬铁误差最大 27 µT、工作台磁场达地磁 3.7 倍，无法可信验证），
  故 UI 明确标注 `6-AXIS REL` 并提供中轴 `SET 0` 归零；官方无磁路径在启动 3 s 内会把航向强制归零，
  因此航向在开机数秒后才有意义；与手机磁罗盘的绝对北向对照**未完成**。
- 板载无 RTC/无 GPS/无心率：表盘时间采用演示偏移；轨迹为 IMU 航位推算精度，非米级。
- 板子无 WiFi，且仅一个 Type-C（CH340 串口，SoC USB 未引出）→ **LLM 网络出口经 PC 中继**
  （Agent/工具/主动逻辑全部在设备上运行）；SLIP/PPP 直连主机的代码保留在
  `src/net_link.c`/`src/ppp_link.c`，未作为交付路径。
- 电池芯片（AW32001）监测未接入（未装电池），电源管理按 USB 供电运行。
- 触屏 UI 依据真机截图（OCR 复核）迭代；演示前请自检各页。

## 七、相关官方文档

- 大赛总览 / 提交流程 / AI 日志手册见 openvela docs `dev-ai-contest-2026` 分支的 `contest_2026/`。
