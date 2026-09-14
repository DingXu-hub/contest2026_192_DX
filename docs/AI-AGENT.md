# 设备侧 AI Agent（主动 + 执行）— 说明与演示

本作品在 AI 硬件赛道的要求下实现了一个**运行在手表上**的 AI Agent：
它不只是"问一句答一句"，而是**能主动发起行为**，并且**能真正执行设备上的操作**。

```
手表 (openvela/NuttX, 本仓 app/hello_app)          Windows PC
┌───────────────────────────────┐                ┌──────────────────────┐
│ ai_agent.c  AI Agent 任务     │  串口 1Mbps    │ tools/llm_relay.py   │
│  ├ 工具(执行)  !start !stop   │ ◄────────────► │  ├ 转发 LLM 请求     │
│  │  !status !timer N !tip     │   ASCII 行协议  │  │  (DeepSeek HTTPS) │
│  ├ 主动引擎: 定时/空闲触发     │                │  └ 人工输入命令      │
│  └ CLI 渠道: /dev/console     │                └──────────────────────┘
└───────────────────────────────┘
```

## 1. 为什么 LLM 走 PC 中继（如实说明）

板子只有一个 Type-C 口（CH340 串口，SoC 的 USB D+/D- 未引出），无 WiFi；
板载 NuttX 侧尝试过 SLIP/PPP 直连主机（代码保留在 `src/net_link.c`、`src/ppp_link.c`），
但受限于虚拟化 USB 直通与内核 netdev 细节未能端到端稳定联通。
因此 LLM 的网络出口由 PC 侧脚本完成（**设备侧 Agent、工具、主动逻辑全部真机运行**），
这在报告中作为明确的已知限制说明，不做夸大。

## 2. 交互协议（ASCII 行协议）

| 方向 | 消息 | 含义 |
|---|---|---|
| PC → 手表 | `?<问题>` | 向 Agent 提问（会带设备状态，转给 LLM） |
| PC → 手表 | `!<工具> [参数]` | 直接执行设备工具（不经 LLM） |
| PC → 手表 | `@LLMRESP <id> <text>` | 中继回传的 LLM 答案 |
| 手表 → PC | `@LLMREQ <id> <prompt>` | Agent 发起的 LLM 请求 |
| 手表 → PC | `@TOOL <name> <result>` | 工具执行日志 |
| 手表 → PC | `@AI <event>` | 主动事件（如 `timer-due`、`proactive-ask`） |

## 3. 设备工具（"执行"能力）

| 工具 | 作用 |
|---|---|
| `!start` | 启动/继续一次跑步（真正调用 run_engine） |
| `!stop` | 结束并保存本次跑步 |
| `!status` | 上报运行状态 + 传感器/指南针状态 |
| `!timer <分钟>` | 设置倒计时提醒（到点由 Agent **主动**弹出） |
| `!tip` | 请求一条跑步姿势建议（走 LLM） |
| `!help` | 工具列表 |

## 4. 主动能力

- **倒计时提醒**：`!timer 5` 后 5 分钟，Agent 主动弹卡片并上报 `@AI timer-due`。
- **空闲教练**：设备空闲 3 分钟后，Agent 主动请求 LLM 生成一句激励/训练提示
  （`@AI proactive-ask` → `@LLMREQ` → 屏幕卡片）。
- 所有主动通知都会在表盘弹出卡片（8 秒，`pages.c: draw_ai_card()`）。

## 5. 运行演示（Windows）

```bash
py -3 tools/llm_relay.py auto <DEEPSEEK_API_KEY>
```
（Key 只作为命令行参数传入，不写入任何文件）

然后直接输入：

```
!status                                  # 设备执行：弹出状态卡片
!start                                   # 设备执行：开始跑步
!timer 2                                 # 2 分钟后主动提醒
?帮我看看现在状态，给一句建议             # 走 DeepSeek，答案上屏
!help
```

## 6. 已知限制

- 烧录后若需复位，请手动按板子复位键（CH340 的 RTS 复位偶发不生效）。
- NuttX 控制台输入为小批量投递，设备侧已用"poll 后循环读到空"处理；
  交互式手敲命令最稳定。
- 语言：中英文均可（LLM 侧 system prompt 要求单行、≤80 字符，便于表盘显示）。
