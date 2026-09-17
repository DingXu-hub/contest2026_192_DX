#!/usr/bin/env python3
# fill_report.py - fill the official openvela contest report template with our content
# Usage: py -3 tools/fill_report.py <template.docx> <out.docx>
import copy
import io
import sys

from docx import Document
from docx.text.paragraph import Paragraph

TPL = sys.argv[1] if len(sys.argv) > 1 else \
    r"D:\Downloads\2026 首届 openvela AI 硬件开发者大赛 - 作品提交模板 (1).docx"
OUT = sys.argv[2] if len(sys.argv) > 2 else \
    r"D:\共享文件夹\contest2026_192_DX\技术报告_contest2026_192_DX.docx"


def set_text(para, text):
    """write text into a paragraph, keeping the first run's formatting"""
    if para.runs:
        para.runs[0].text = text
        for r in para.runs[1:]:
            r.text = ''
    else:
        para.add_run(text)


def fill(para, lines):
    """replace para with lines[0] and insert the rest after it (same style)"""
    set_text(para, lines[0])
    anchor = para
    for extra in lines[1:]:
        new_p = copy.deepcopy(anchor._p)
        anchor._p.addnext(new_p)
        np = Paragraph(new_p, anchor._parent)
        set_text(np, extra)
        anchor = np
    return anchor


doc = Document(TPL)
P = doc.paragraphs

# ---------------- 信息表 ----------------
T2 = doc.tables[2]
T2.cell(1, 1).text = "黄山派智能跑步手表（DX Watch）——端侧主动式 AI 运动助手"
T2.cell(2, 1).text = "contest2026_192_DX（DX 队）"
T2.cell(3, 1).text = ("晏宏旭（GitHub: DingXu-Hub）：硬件调试与接线、固件与 UI 实现、"
                      "AI Agent 与链路协议、磁力计校准算法、真机测试与文档；"
                      "AI 编码助手（opencode / pi）承担代码生成、官方库逐行比对、文档与证据整理")
T2.cell(4, 1).text = "AI 硬件产品创新；手表应用创新"

# ---------------- 摘要（≤300 字）----------------
abstract = (
    "本作品是一块基于 openvela/NuttX 与黄山派 SF32LB52 的智能跑步手表：6 页现代化 UI（表盘/跑步/地图/指南针/统计/设置）、"
    "端侧主动式 AI Agent（定时提醒与空闲自动问询，可执行 !start/!status/!timer 等工具并把结果以通知卡片上屏）、"
    "经 PC 网关的可靠联网（真实时间同步、HTTP 取数、LLM 问答）以及经物理自证的磁力计自校准。"
    "针对 CH340 链路“控制台 RX 等效 1 字节深”与 NSH 抢占控制台两个隐蔽根因，自研帧协议（CRC16+ACK/重传）并使 PC 侧逐字节节流写，"
    "命令零丢失；磁校准以最小二乘球面拟合并用“半径必须等于当地地磁总场 47.5 µT”做硬门限，"
    "修复后 Fusion 磁误差由 90° 降到 0–1°、静止航向不再漂移、|B| 朝向不变（35–56 µT）。"
    "资源上关闭 ALLSYMS 与缩减缓冲，SRAM 占用由 81.8% 降到 39.3%。"
)
fill(P[7], [abstract])

# ---------------- 3.1 绪论 ----------------
fill(P[10], [
    "应用场景：户外跑步/健走人群需要一块“不用掏手机”的手表——实时配速、轨迹、方位与主动提醒；"
    "产业痛点：(1) 主流运动表要么依赖手机联网、要么价格高昂；(2) 端侧 AI 多为“被动问答”，缺少主动服务；"
    "(3) 低成本手表普遍没有可靠的地理方位（无 GNSS、磁力计易受干扰）。",
    "用户痛点（真机实测发现）：开发桌面存在 184 µT 强磁异常（地磁的 3.7 倍），"
    "在桌面上做磁校准会得到方向错误但“看似可用”的结果——这正是一般消费级产品指南针不可靠的典型原因。",
])
fill(P[11], [
    "① 无 RTC、无 WiFi、蓝牙为 BLE-only（实测无 BR/EDR）——只能靠单条 USB 串口同时承载日志、命令与联网；"
    "② 该串口控制台 RX 实测“等效 1 字节深”（63 字节突发仅 2–19 字符到达），且 NSH 交互 shell 抢占同一设备；"
    "③ 磁力计硬铁/软铁校准：平面 8 字无法约束 Z 轴，必须在三维翻滚下做球面拟合并给出物理可判定的验收；"
    "④ 端侧算力/内存有限：SRAM 初始占用 81.8%，需在不牺牲功能的前提下瘦身。",
])
fill(P[12], [
    "① “端侧主动 Agent + PC 中继”的务实联网架构：在硬件无网络能力的条件下，用自研帧协议把时间同步、HTTP 取数、LLM 问答"
    "交给 PC 网关，并如实标注数据出口，避免“假装板子联网”；",
    "② 校准的物理自证验收：以“拟合半径必须等于当地地磁总场（≈47.5 µT，IGRF）”作为硬门限，把“看起来能用”升级为“物理上正确”；"
    "并额外用运行时指标（Fusion 磁误差、|B| 朝向不变性）做闭环验证；",
    "③ 面向“坏链路”的工程加固：定位并解决 RX 1 字节深、NSH 抢占两个隐蔽根因；",
    "④ UI 现代化：自研圆润笔画数字、渐变弧、胶囊按钮与发丝卡片，替代陈旧七段数码管风格，"
    "在 390×450 圆角屏上严格遵守“四角零内容/无长按/仅 KEY 翻页/辅助文字 2×”规范。",
])

# ---------------- 3.2 系统方案设计 ----------------
fill(P[14], [
    "系统分四层（自下而上）：① 硬件层：黄山派核心板（SF32LB52 双核 + 8 MB PSRAM）、390×450 AMOLED（RGB565）、"
    "FT6146 触摸、LSM6DS3 IMU、MMC5603NJ 磁力计、LTR303 环境光、CH340 串口；"
    "② 系统层：openvela/NuttX（POSIX 任务、I2C/SPI/LCD 驱动、电源管理、KV/文件系统）；"
    "③ 应用层：渲染与 UI（自研工具层 + 帧缓冲直写 + ePicasso GPU 纯色填充）、传感器与九轴融合（Fusion v1.3.3）、"
    "跑步引擎（步数/配速/航位推算轨迹）、AI Agent（工具执行 + 主动引擎）；"
    "④ 链路与云侧：`link.c` 帧协议 ⇄ PC 网关（时间同步/HTTP 代理/LLM/文件通道预留）。",
    "数据流：IMU/磁/光 → 采样任务（含磁硬软铁校正与球面校准）→ Fusion 融合 → 航向/姿态 → UI 与跑步引擎；"
    "AI Agent 读同一份状态做主动判断，经帧协议与 PC 网关交换文本/HTTP 结果，最终以卡片形式回到 UI。",
])
fill(P[15], [
    "开发板选型：赛事指定黄山派 SF32LB52（含 AMOLED、IMU、磁力计、环境光、充电 IC 与 TF 卡），"
    "自带丰富传感器，适合做“可穿戴终端 + 端侧 AI”的完整闭环；PSRAM 为地图/轨迹提供缓冲空间。",
    "端/云职责划分：端侧负责实时性任务（采样、融合、渲染、UI、主动判断、工具执行）；"
    "云侧（经 PC 网关）只负责需要外部知识与算力的事（HTTP 取数、LLM 生成、时间基准）。",
    "断网/弱网降级：网关不在时链路自动降级为纯文本模式（AI 工具仍可用，串口终端可直接敲命令），"
    "时间退回演示日期，磁力计异常时融合自动切到“陀螺 + 加速度”并在 UI 显示 GYRO ONLY；"
    "与备选方案的对比：BLE-PAN 在本芯片上不可用（BLE-only 协议栈）、RNDIS 物理不可用（板子仅一个 Type-C=CH340）、"
    "因此在“单串口”约束下选择“应用层帧协议 + PC 网关”，实测命令零丢失、RTT 52–79 ms。",
])
fill(P[16], [
    "① 链路模块（link.c）：AA55 魔数 + CRC16(CCITT-FALSE) + 序号 + 长度 + 载荷 + 填充；通道复用 CONTROL/TEXT/HTTP/FILE/IP(预留)；"
    "逐帧 ACK，未收到则重传；非帧字节按行喂给 AI Agent，保证“裸串口终端仍可用”；",
    "② AI Agent 模块：工具集（!start/!stop/!status/!timer/!tip/!link/!time/!http/!mic/!help）+ 主动引擎"
    "（定时提醒、空闲 3 分钟主动请求 LLM）+ 通知卡片（1–3 行自适应、点按收起且不误触下方按钮）；",
    "③ 磁校准模块：三维翻滚采样 → 最小二乘球面拟合 → 三重验收（每轴跨度 ≥2000 counts、样本 ≥80、"
    "拟合半径 25–65 µT、RMS 偏差 ≤8%）→ 烘入固件或写入会话 KV；",
    "④ 渲染与交互模块：帧缓冲直写 + 页入场动画；页面跳转仅 KEY1/KEY2 短按，动作全部落在中轴大按钮，无长按手势。",
])

# ---------------- 3.3 核心算法与技术原理 ----------------
fill(P[18], [
    "端侧模型：本作品定位“端侧主动 Agent + 云端大模型中继”。端侧运行的是状态机式 Agent（主动引擎 + 工具执行），"
    "不加载神经网络权重，无 NPU 占用；算力占用可忽略（AI 任务栈 6 KB，空闲周期 200 ms 轮询）。",
    "云端模型：经 PC 网关调用对话模型（当前为 DeepSeek；大赛鼓励的 MiMo 只需在网关侧替换 endpoint 与鉴权头即可切换），"
    "接口为 HTTPS POST（OpenAI 兼容 `chat/completions`），鉴权使用 API Key，仅在 PC 侧命令行传入、不入库；"
    "网关对请求做长度与内容裁剪（≤110 字符）以适配手表屏幕。",
    "说明：本作品未在端侧部署量化模型，故未涉及框架/量化/端侧推理耗时；相关指标见 3.5 的链路与响应时延实测。",
])
fill(P[19], [
    "① 九轴姿态融合：采用 xioTechnologies/Fusion v1.3.3（NED 约定），单位核对为陀螺 °/s、加速度 g、磁场 µT；"
    "由于采样循环并非恒定频率（19–33 Hz），每帧用真实 dt 调用 FusionAhrsSetSamplePeriod() 以保证积分正确——"
    "这也是本项目**不照抄**上游 v1.3.3 的 startup 重构（其前提是采样周期只设一次）的原因；同时手工移植了上游 NaN 防护。",
    "② 磁硬铁/软铁校正：先减硬铁偏移，再按各轴半跨度做软铁比例归一；场强 |B| 作为不变量用于异常门控"
    "（20/80 µT 判可疑、28–65 µT 恢复），环境磁干扰交由 Fusion 的磁拒绝机制处理。",
    "③ 校准求解：球面方程 x²+y²+z²=2ax+2by+2cz+d 的代数最小二乘给出初值，再用几何（正交距离）精化；"
    "验收含“半径 = 当地地磁总场”的物理门限与 RMS 质量门限，避免把“桌面 184 µT 异常场”标成地磁。",
    "④ 主动决策：定时器到期或空闲超时触发；触发后按“先本地工具、再云端问答”的顺序执行，并把行为写入 @AI/@TOOL 日志便于复核。",
])
fill(P[20], [
    "落地的 openvela 能力：① 图形——`/dev/fb0` 帧缓冲直写 + `/dev/lcd0` 背光/电源控制 + ePicasso GPU 纯色填充与地图合成；"
    "② AI——端侧 Agent（任务内工具执行 + 主动引擎）与人机交互卡片；③ 系统——POSIX 多任务、"
    "I2C（IMU/磁/光/触摸）、按键与触摸输入、电源管理（ACTIVE/IDLE/SLEEP）、KV 存储、以及 NuttX 串口控制台。",
    "对 openvela 的优化与改进建议：",
    "① 串口控制台 RX 行为：实测“背靠背写入互相覆盖、等效 1 字节深”，建议在文档中明确该特性并给出“逐字节节流写”的推荐用法；"
    "本项目已用该结论把 PC 侧工具全部改造；",
    "② 控制台所有权：`CONFIG_NSH_ALTCONDEV` 在本板未能让出 console 给应用，建议提供“应用独占 console”的官方配置/示例，"
    "本项目暂以应用启动时 SIGSTOP nsh 任务的方式接管；",
    "③ 资源占用：`CONFIG_ALLSYMS` 会常驻约 125 KB（g_allsyms），建议在发布配置中默认关闭；"
    "④ 内部 AUDCODEC 缺少公开的 bring-up 示例（见 3.7 不足），建议在 SDK 中补一个最小录音/放音示例。",
])

# ---------------- 3.4 系统实现 ----------------
fill(P[22], [
    "固件为 openvela/NuttX 应用（`app/hello_app/`，58 个源文件、约 1.28 万行），任务划分：",
    "`huangshan_render`（渲染与 UI）、`huangshan_sensor`（采样 + 融合 + 磁校准）、`huangshan_key`（KEY 翻页）、"
    "`huangshan_link`（独占 console 的帧协议收发与保活）、`huangshan_ai`（主动引擎与工具执行）；"
    "启动脚本 `/etc/init.d/rcS` 仅拉起应用，应用再接管 console 输入。",
    "关键实现约束：发布固件关闭诊断输出（`APP_DIAG_VERBOSE=0`），抓帧/自动翻页等调试代码由 `devshot.h` 开关编译隔离，"
    "确保提交源码与发布固件一致且可复现。",
])
fill(P[23], [
    "① 采样流：20 ms 周期读 IMU（I2C）→ 触发磁测量（TM_M｜Auto_SR，轮询 STATUS bit6）→ 硬软铁校正 → 全局门控；",
    "② 融合流：每帧 dt → FusionAhrsUpdate（磁可信时）或 UpdateNoMagnetometer（磁被拒时）→ 欧拉角 → UI 航向；",
    "③ AI 流：串口/网关文本 → 行解析 → 工具执行或 LLM 请求（经帧协议 → 网关 → 云端）→ 结果以卡片上屏；",
    "④ 磁校准流：用户在指南针页点 CALIBRATE → 15 s 三维翻滚采样（LSQ 累加）→ 三重验收 → 通过则写入会话 KV 并立即生效。",
])
fill(P[24], [
    "硬件平台：黄山派 SF32LB52 核心板（官方开发板）+ 板载 AMOLED/触摸/IMU/磁力计/环境光；外接仅 USB-C（CH340 串口，供电+调试）。",
    "是否完成全新硬件平台适配或驱动开发：**是（部分）**——① 磁力计 MMC5603NJ：自行实现 I2C 驱动与 20-bit 解包、"
    "SET/RESET 时序、AUTO_SR 单次测量、饱和/自检位处理，并实现球面拟合自校准（驱动层 + 算法层合计约 500 行）；"
    "② 内部 AUDCODEC 麦克风采集：已完成寄存器配置与 DMA 通道搭建（详见 3.7 不足）；"
    "③ 显示/触摸/IMU/环境光沿用板级驱动，但完成了帧缓冲直写与 GPU 合成路径的适配。",
    "适配难点与解决：磁力计硬铁偏移在平放时无法约束 Z 轴、板子附近 184 µT 磁异常、控制台 RX 丢字节、NSH 抢占 console，"
    "均已在 3.1–3.3 说明并给出可复现的验证方式。关键 BOM 即核心板自带器件，无额外外设。",
])
fill(P[25], [
    "应用/交互端：手表本体 6 页 UI（表盘/跑步[数据⇄地图]/轨迹/指南针/统计/设置），"
    "设计语言为深黑 AMOLED + 每页强调色 + 圆润笔画数字 + 渐变弧 + 胶囊按钮 + 发丝卡片；"
    "AI 通知卡片按文本 1–3 行自适应并支持点按收起；",
    "PC 端配套 `tools/gateway.py`（帧编解码自检、时间同步、HTTP 代理、LLM、日志解复用）与若干调试工具"
    "（`link_test.py`/`mic_test.py`/`listen_ui.py`/`fit_mag_sphere.py`），构成“手表 + 网关”的最小可用系统。",
])
fill(P[26], [
    "自建 Skill：`skills/huangshan-watch-dev/SKILL.md`（本仓，99 行）——沉淀“本地改→VM 编译→烧录→串口/截图验证”全流程、"
    "硬件拓扑与全部踩坑（含 RX 1 字节深、NSH 抢占、PM 门控 codec 时钟、构建目标更名等），供后续任何 AI 会话直接复用。",
    "运行时 Skill（AI 硬件赛道要求说明）：当前版本为**端侧 Agent 以“工具集”形式实现能力**"
    "（!start/!stop/!status/!timer/!tip/!link/!time/!http/!mic/!help），尚未实现从 `/data/agent/skills/` 目录动态加载 Skill；"
    "此项为本作品与赛道要求的偏差，已列入 3.7 的未来工作（计划实现：启动时扫描该目录，解析 name/触发词/动作脚本并注册为可调用工具）。",
])

# ---------------- 3.5 系统测试与结果分析 ----------------
fill(P[28], [
    "测试环境：黄山派 SF32LB52 手表（390×450 AMOLED）+ Windows PC（COM4，CH340，1 Mbaud，pyserial；"
    "PC 侧工具全部采用逐字节节流写）；固件为发布版（`APP_DIAG_VERBOSE=0`、`HUANGSHAN_DEV_SHOT=0`）；"
    "构建：VM Ubuntu 上 arm-none-eabi-gcc + CMake/Ninja；评测所用原始数据、串口日志与截图见 `docs/evidence/`。",
])
fill(P[29], [
    "功能测试项（实测结果）：",
    "① 六页 UI 渲染与翻页（KEY1/KEY2）：通过，抓帧见 `docs/evidence/screenshots/`；"
    "② 跑步计量（步数/配速/距离/轨迹）：通过（步数由加速度峰值检测、轨迹为航位推算）；"
    "③ 指南针与磁校准：通过——修复后 Fusion 磁误差 90°→0–1°、静止航向不漂移、|B| 朝向不变；"
    "④ 端侧 AI Agent：通过——工具执行（!status 等）与主动提醒（!timer 到期自动提醒）实测可用，结果以卡片上屏；"
    "⑤ 联网：通过——时间同步（含时区）、`!http https://api.github.com/zen` 返回 HTTP 200 与正文、`?问题` 走 LLM 通道；"
    "⑥ 麦克风：**未通过**（寄存器配置与 DMA 就绪，但 codec 不产生转换数据，见 3.7）。",
])
fill(P[30], [
    "性能实测数据（均附测量方式与日志）：",
    "· 资源：Flash 5,800,444 B / 16 MB = 34.6%；SRAM 205,840 B / 512 KB = 39.3%（优化前 428,800 B = 81.8%）；"
    "BSS 削减约 228 KB（ALLSYMS 125,736 B + GPX 缓冲 137,312 B + 航迹环 32 KB）；",
    "· 渲染：目标 60 fps；实测 15–33 fps（随页面与动画变化，日志 `[Render] fps=`）；每帧清屏写量 390×450×2 B = 351 KB；",
    "· 链路：帧 ACK 6/6；PING→PONG 往返 52–79 ms；时间同步与 PC 同秒（±1 s）；HTTP 取数（GitHub）≈1 s 级；",
    "· 功耗估计：屏幕点亮、中等刷新下约 21 mA（`[Render] est=21mA`，基于板级电源模型的估算值）；",
    "· 磁校准：|B| 同姿势标准差 20.0 → 0.17 µT；不同朝向 4.5–83.4 → 35–56 µT；拟合半径 47.5 µT 与当地地磁总场一致。",
])
fill(P[31], [
    "可靠性/稳定性：",
    "① 命令链路：引入帧协议（CRC16+ACK+重传）后，连续 5 次 PING 与多次工具命令全部被确认（`rx_bad≤1`，未出现命令丢失）；"
    "② 长时间运行：连续运行 ≥1 小时（含渲染、采样、AI 空闲问询）未出现 hardfault 或重启，"
    "内存/帧率无异常波动（无泄漏迹象：长时间运行后 SRAM 占用稳定）；",
    "③ 异常兜底：磁力计被强场污染（桌面 184 µT）时自动降级为陀螺模式并在 UI 标注 GYRO ONLY，"
    "磁校准不合格（覆盖不足/半径异常/RMS 超限）时保留上一次校准并明确提示；"
    "④ 误报/漏检说明：本作品非安全/健康类产品，无识别准确率类指标；步数与配速为估算值，"
    "在 3.7 中已如实说明其误差来源（无 GNSS，航位推算累积误差）。",
])

# ---------------- 3.6 AI-Native 开发说明 ----------------
fill(P[33], [
    "开发方式：全流程 AI-Native。使用 opencode（模型 kimi-k3）与 pi 编码助手完成需求拆解、代码生成、官方库逐行比对、"
    "真机调试脚本编写与文档整理；AI 会话日志由大赛采集器自动落盘到本仓 `logs/DingXu-Hub/`（5 个会话、217 事件，"
    "官方 `validate-log.py` 校验 ALL OK）。",
    "效率提升（可核对）：① 逐行比对 xioTechnologies/Fusion v1.3.3 与 Adafruit_MMC56x3、并核对 MMC5603NJ 数据手册，"
    "定位“硬铁偏移被人为当作吸收地磁垂直分量的旋钮”这一根因；② 通过设备端逐帧诊断 + PC 端回显测量，"
    "把“串口时好时坏”定位为“控制台 RX 等效 1 字节深 + NSH 抢占 console”；③ 自动生成抓帧/量化 UI 检查脚本，"
    "把 6 页 UI 的布局与字号复核自动化；④ 在线扫参工具（`!mic sweep`）把硬件 bring-up 从反复烧录变成一条命令出表。",
    "遇到的问题与解决：AI 生成的初版帧协议在 PC 侧 CRC 计算错误（只算帧头）导致设备端全部拒收，"
    "通过设备端寄存器/帧级诊断输出暴露该问题并修复；此外 AI 曾把 Z 硬铁偏移按“平放 mz≈0”调参，"
    "经物理不变量 |B| 检验与球面拟合纠正——这也是本作品“证据优先、可复现”开发原则的由来。",
])
T3 = doc.tables[3]
T3.cell(1, 1).text = ("约 85%（口径：本仓新增约 1.28 万行源码与文档中，由 AI 生成或 AI 主导修改的比例；"
                      "人工负责需求取舍、硬件接线、真机验证、参数裁定与最终复核）")
T3.cell(2, 1).text = ("opencode（opencode-go / kimi-k3）、pi 编码助手、StepFun step-3.7-flash（截图 OCR 复核）、"
                      "LibreOffice（文档导出）")
T3.cell(3, 1).text = "未使用 MCP（以仓库内自研 Python 工具脚本替代：gateway/link_test/mic_test/listen_ui/fit_mag_sphere）"
T3.cell(4, 1).text = ("使用：stepfun-vision-skill（截图 OCR 复核）、word-docx（生成技术文档）、pdf-tools、clonedeps（克隆官方库比对）、"
                      "reflect/simplify/verification-planning 等通用技能；新增沉淀：huangshan-watch-dev（本仓 skills/）")
T3.cell(5, 1).text = "未统计（会话日志已导出至 logs/DingXu-Hub/，含 5 个会话 / 217 事件，可据此复核）"

# ---------------- 3.7 总结与展望 ----------------
fill(P[35], [
    "① 完成一套可运行、可复现的手表固件：6 页现代化 UI、九轴融合与磁自校准、端侧主动 AI Agent、"
    "经 PC 网关的可靠联网（时间同步/HTTP/LLM），并在真机上逐项验证；",
    "② 解决了两个隐蔽的链路根因（控制台 RX 1 字节深、NSH 抢占 console），命令零丢失；"
    "③ 磁力计校准以物理不变量做验收，Fusion 磁误差 90°→0–1°；④ 资源占用 SRAM 81.8%→39.3%；"
    "⑤ 沉淀 1 个自建 Skill、5 个真实 AI Coding 会话日志与 15 份真机证据（截图/串口/测量数据）。",
])
fill(P[36], [
    "目标受众：户外跑步/健走人群（18–45 岁，一二线城市为主）、以及需要“低依赖、可离线”智能穿戴的用户；"
    "同时可作为 openvela 手表类应用的开发模板（UI 工具层、传感器融合、磁校准、串口网关均可复用）。",
    "商业价值与规模化潜力：硬件为国产 SF32LB52 方案，成本可控；端侧主动 AI + 手机网关的架构无需蜂窝/WiFi，"
    "降低了整机 BOM 与功耗；磁校准与链路加固方案可直接移植到其他穿戴产品（手环、儿童手表、骑行码表）。"
    "潜在的商业模式：整机销售 + 运动数据订阅（云端训练计划），或向品牌方提供固件/算法授权。",
])
fill(P[37], [
    "① 无 RTC：时间依赖 PC 网关同步，重启后回到演示日期（可加低成本 RTC 或首次联网后写 KV + 定时校准）；",
    "② 麦克风未打通：内部 AUDCODEC 寄存器配置与 DMA 通道已就绪，但 codec 不产生转换数据；"
    "已排除 RCC 时钟门控、PM 休眠、DMA 武装、12 组时钟配置与数字通道 0/1 映射等因素，"
    "根因疑为厂商驱动的最后一步“启动”（公开 SDK 仅含外部 codec 驱动 DA7212，无内部 codec 示例）；"
    "下一步拟向 SiFli/立创索取内部 codec bring-up 片段，或改用 PDM 数字麦路径；",
    "③ 轨迹为 IMU 航位推算（无 GNSS），长距离累积误差较大，可通过地图匹配或偶尔的手机定位校正改善；",
    "④ 运行时 Skill 未实现：计划在 `/data/agent/skills/` 下实现 Skill 动态加载（解析 name/触发词/动作并注册为工具）；",
    "⑤ 电池监测未实现（AW32001 在 I2C 上，驱动未写）；⑥ 端侧模型：未来可在支持 NPU 的芯片上落地量化模型，"
    "把“主动判断”从规则升级为本地小模型推理。",
])

doc.save(OUT)
print("saved:", OUT)
