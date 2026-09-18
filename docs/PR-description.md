# PR：黄山派智能跑步手表作品提交（DX 队）

> 说明：本文件是提交用 PR 描述，便于 fork 后直接粘贴。

**队伍**：`contest2026_192_DX`（DX 队，GitHub `DingXu-Hub`）
**目标分支**：`dev-ai-contest-2026`

## 提交内容

1. **作品源码**：`app/hello_app/`（39 个文件）
   - 6 页现代化 UI（表盘/跑步/地图/指南针/统计/设置），自研 UI 工具层（圆润笔画数字、渐变弧、胶囊按钮、发丝卡片）
   - **端侧 AI Agent**：工具执行（`!start/!stop/!status/!timer/!tip/!link/!time/!http/!mic/!help`）+ 主动引擎（定时提醒、空闲主动请求 LLM），结果以通知卡片上屏
   - **联网链路**：`link.c` 帧协议（CRC16 + ACK/重传 + 通道复用）+ PC 网关（时间同步 / HTTP 代理 / LLM）
   - **磁力计**：九轴融合（Fusion v1.3.3）+ 球面拟合自校准（三重验收）
2. **AI Coding 日志**：`logs/DingXu-Hub/`（官方采集器导出，5 个会话，manifest 齐备）
3. **自建 Skill**：`skills/huangshan-watch-dev/SKILL.md`
4. **真机证据**：`docs/evidence/`（实机截图、串口日志、测量数据与测量方式）
5. **技术文档**：`docs/`（技术报告草稿、作品介绍、指南针病因分析、联网根因、麦克风方案、交接文档）

## 关键实测数据（均附依据）

| 项目 | 结果 | 依据 |
|---|---|---|
| 资源 | Flash 34.6%、SRAM 38.2%（优化前 81.8%） | 构建内存报告 + `nm --size-sort` |
| 磁校准 | Fusion 磁误差 90°→0–1°；\|B\| 朝向不变（35–56 µT） | `docs/evidence/measurements/*.json` + 串口日志 |
| 6 轴姿态 | `!att test` 9/9 PASS；静止 3 分钟 yaw 漂移 +0.03~+0.10 °/min | `tools/att_still.py` 输出、`docs/evidence/measurements/att-stillness-3min.log` |
| 链路 | ACK 6/6、RTT 52–79 ms、时间同步同秒、HTTP 200 | `docs/evidence/serial-logs/*.log` |
| 渲染 | 目标 60 fps，实测 15–33 fps | 串口 `[Render] fps=` |

## 最新提交（2026-09-17 晚）

**6 轴姿态改用官方 Fusion 链路**：`attitude6.c` 从自写欧拉角互补滤波改为薄封装官方无磁路径
（`FusionBiasUpdate` → `FusionAhrsUpdateNoMagnetometer` → `FusionQuaternionToEuler`，NED、
`gyroscopeRange=2000 dps`、`accelerationRejection=15°/5 s`、每帧真实 dt）；唯一保留的自研部分是
**输入毛刺迟滞过滤**（官方超量程检查在 ±1960 dps 附近才触发，抓不到本板 18–30 dps 单样本读毛刺）。
合成真值自检按官方链路重建（9 用例 ALL PASS），并因此抓到并修掉自写过滤会“吞掉从静止开始的真实转动”的缺陷。
同时把 6 轴航向接入跑步航位推算（死区 4°→1°、符号修正为顺时针为正、起跑基准取相对 yaw），
轨迹随真实转向弯曲；演示数据写入一条 150 点 / 3417 m 闭环供截图。

## 如实说明（不足）

- 无 RTC：时间依赖网关同步，重启回演示日期；
- 麦克风 bring-up 未完成（内部 codec 厂商启动序列不在公开 SDK，已排除 12 组时钟配置与通道映射，寄存器配置正确但无转换数据）；
- 轨迹为 IMU 航位推算（无 GNSS）；电池监测未实现。
