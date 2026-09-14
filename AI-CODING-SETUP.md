# AI Coding 日志归集（本队操作手册）

官方采集器仅在 **openvela 工作区（存在 `.repo/`）** 内、会话结束时把对话自动写入本仓 `logs/<github_login>/<date>/<tool>__<sid>.jsonl`，
支持工具：`claude-code` / `opencode` / `codex` / `kiro`。

> 本队日常开发一部分在 Windows 上的 AI 工具进行（非 .repo 工作区，不会自动入仓）。
> 为保证「AI 开发」评分维度有据可查，请按下述步骤补足真实日志：

## 步骤（在 VM / 任意 openvela 工作区机器）

1. 确认有 `repo` 拉取的完整工作区（含 `.repo/`），demo 仓在 `contest2026_192_DX/`。
2. 安装采集器（一次）：
   ```bash
   cd /home/openvela/contest2026_192_DX
   bash ../.claude/skills/contest-log-collector/onboarding/install.sh \
     --team-id contest2026_192_DX \
     --github-login <你的 GitHub 用户名>
   ```
3. 自检：`bash ../.claude/skills/contest-log-collector/onboarding/verify-setup.sh`（应无 [FAIL]）。
4. 用任一受支持工具在工作区内真实开发/复盘 ≥1 个会话（例如：对着本仓 `app/hello_app/` 源码做一次「代码走读/提交说明」对话），正常退出工具。
5. 检查自动写入：`git status` 应看到 `logs/<你的登录名>/.../*.jsonl` 与 `manifest.json` 变更。
6. 随代码一起 `git add logs/ && git commit && git push`。

## 若已用其它工具（如本文档由 pi 驱动）

- 将 AI 助手的完整会话记录导出（JSONL/Markdown）存为 `logs/<login>/<date>/manual/pi__session-<id>.md` 并如实标注采集方式为 manual，
  作为补充证据；仍请按 1-6 用官方采集器至少产生一份标准 `jsonl`。

## 自查清单（提交前）

- [ ] `logs/your-github-login/` 示例目录已删除，真实 `<login>/` 存在
- [ ] `manifest.json` 的 team_id=`contest2026_192_DX`，github_login 正确
- [ ] 每个会话文件 event_count 与 manifest 一致，health=ok
- [ ] 提交后远程仓库能看到 logs 变更
