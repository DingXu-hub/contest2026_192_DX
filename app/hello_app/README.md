# hello_app — 黄山派智能跑步手表（应用源码）

NuttX 应用，清单 `contest2026_192_DX.xml` 映射到 openvela 的
`packages/demos/contest2026_192_hello_app`（config：`LVX_USE_DEMO_CONTEST2026_192_HELLO_APP`）。

## 构建（应用部分）

```bash
# 工程全量已 repo sync 后，按 openvela 板级流程配置并启用
# CONFIG_LVX_USE_DEMO_CONTEST2026_192_HELLO_APP=y（黄山派 Pi 或 ARCH_SIM）
# 应用自身源集合见 CMakeLists.txt（含 src/fusion/FusionAhrs.c）
```

## 运行

- 真机（黄山派 Pi / SF32LB52）：应用启动即显示表盘；KEY1=上一页、KEY2=下一页；
  跑步页中轴 [START/PAUSE/STOP] 按钮，右上角内容详见 UI。
- 主机模拟器（ARCH_SIM）：编译 sim 目标可无头运行。

## 目录

- `src/*.c/h` 应用代码（main / pages / sensor / ui / route / run / gpx / bt 等）
- `src/fusion/` xioTechnologies/Fusion AHRS（本仓 fork 带 DSH 本地补丁：headingAnchored 等）
- 关键行为与已知边界见仓库根 README「四、如实说明」。
