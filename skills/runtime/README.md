# 运行时 Skill（/data/agent/skills/）

这些是**设备运行时 Skill**：应用启动时会把它们写入手表 `/data/agent/skills/`（板子 `/data` 为 tmpfs，
故每次启动自动播种），随后由此加载并注册为可调用能力。

## 格式

```text
name: hydrate
trigger: 喝水,补水,hydrate,drink,water
action: notify Drink 200 ml of water
```

- `trigger`：逗号分隔的关键词（中英文均可）。用户在串口/网关输入的自由文本命中关键词即自动触发；
- `action`：仅限 Agent 已有的能力，技能文件无法执行任意代码：
  | action | 作用 |
  |---|---|
  | `notify <文本>` | 弹出通知卡片 |
  | `timer <分钟>` | 启动提醒倒计时 |
  | `status` | 播报跑步/传感器状态 |
  | `tip` | 经 PC 网关请 LLM 给一句提示 |
  | `http <url>` | 经 PC 网关取回 URL 内容（真实联网） |

## 使用

```text
!skills          # 列出已加载技能
!skill hydrate   # 手动执行
!skill reload    # 重新扫描目录（新增 .md 后无需重启）
天气             # 自由文本命中 trigger 即自动执行（无需 ! 前缀）
```

> 触发场景示例：跑者说"天气"→ 自动经网关取回实时天气并上屏；
> 说"喝水/补水"→ 弹出补水提醒；说"拉伸"→ 启动 5 分钟拉伸提醒。

## 实现

`app/hello_app/src/runtime_skill.c`（扫描 → 解析 name/trigger/action → 注册为工具 + 关键词触发），
能力通过 `rskill_ops_t` 回调注入，与 AI Agent 解耦。
