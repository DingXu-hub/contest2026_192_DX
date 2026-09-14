---
name: huangshan-watch-dev
description: 黄山派 SF32LB52 智能手表固件开发流水线（仅改本地工作副本→同步 VM→编译→烧录→串口/截图验证）。When the user works on huangshan_running / contest2026_192_DX watch firmware, use this skill to keep the build-flash-verify loop correct and avoid known pitfalls.
---

# Huangshan Watch Dev (contest2026_192_DX)

沉淀「openvela 黄山派 SF32LB52 手表」从代码到真机的完整开发流程，避免每次重复踩坑。

## 触发词

huangshan / 黄山派 / SF32LB52 / 指南针 / compass / mag / MMC5603 / watch UI / 手表固件 / 烧录 / 轨迹 / 跑步页
同步 VM / 编译烧录 / serial 验证

## 拓扑速查

| 项 | 值 |
|---|---|
| 唯一合法工作副本 | `D:/共享文件夹/app/app/huangshan_running/`（两层 app/app，勿动 `_archive/` 与另一份 `app/huangshan_running`） |
| VM | `dingxu@192.168.1.9`（DHCP 可能变！不通先 `ip addr` 找新 IP），工程 `/home/openvela`，构建目录 `cmake_out/lckfb_huangshan_pi` |
| 串口 | COM3 @1Mbps 8N1（会随插拔变化，sftool 会报 Available ports）；打开端口即 RTS 复位 |
| 屏幕 | 390×450 AMOLED RGB565 `/dev/lcd0`；源码 UI 无 RTC → 表盘演示时间在 pages.c 有 demo 偏移 |
| 传感器 | LSM6DS3 IMU + MMC5603NJ 磁力计（I2C0x30）+ LTR303；磁→设备轴映射 `(-x,+y,-z)` |

## 开发流程（每次必做）

1. 只改 `D:/共享文件夹/app/app/huangshan_running/src/*.{c,h}` 与 `fusion/`。
2. 本地检查：`py -3 D:/共享文件夹/tools/check_balance.py <改的文件>`。
3. 同步到 VM（含 fusion 目录）：
   ```bash
   scp src/*.c *.h dingxu@VM:/home/openvela/apps/examples/huangshan_running/src/
   scp src/fusion/*.c *.h dingxu@VM:/home/openvela/apps/examples/huangshan_running/src/fusion/
   ```
4. VM 编译：
   ```bash
   export PATH=/home/openvela/prebuilts/.../cmake...:.../build-tools...:.../gcc/...arm-none-eabi/bin:$PATH
   cd /home/openvela/cmake_out/lckfb_huangshan_pi
   cmake --build . --target final_nuttx
   arm-none-eabi-objcopy -O binary final_nuttx nuttx.bin
   ```
5. 取回并烧录：`ssh ... cat nuttx.bin > /c/sf/img/nuttx.bin`；`cd /c/sf && ./sftool.exe -c SF32LB52 -p COM3 -m nor -b 1000000 ... write_flash "C:/sf/img/nuttx.bin@0x12010000"`。
6. 串口验证（tools/capture_boot.py / serial_monitor.py）：查 hardfault、`[Mag] factory cal off=(-3048,3101,-9498)`、`[Fuse]`2Hz、`drp=`；UI 逐页可用 tools/shot_ui.py+listen_ui.py（需要固件含一次性截图导出，已随 v3.3 移除，勿在演示固件上期待）。

## 输出规范

- 每次代码改动给出：改了什么文件/原因/验证结果（编译 md5、串口关键行、截图 OCR）。
- 指南针/磁力计改动后必须看串口：`drp=`(0=磁参与)、`em`(磁残差)、factory off、`[AC]` 自动校准是否 APPLIED、yaw 是否稳定。
- UI 布局要求：四角不放控件、动作只用中轴大按钮、无长按手势、页面跳转只用 KEY1/KEY2 短按。
- 禁止直接改官方仓库编译树：生产仓库零改动，作品只放本仓 `app/hello_app/`，由 manifest 软链映射。

## 常见坑

- 输入 ui_text/ui_digit 的 buf_w 必须为真实帧宽 390（行距），列居中用 num_at_center/text_at_center；传列宽会花屏。
- 表盘时间无 RTC：演示靠 pages.c 的 `demo_off` 偏移。
- 磁被弱场/金属丢(`drp=1`)属正常降级（GYRO ONLY）；校准需 3D 翻滚 + 远离金属，覆盖不足会被拒绝。
- VM/串口 IP 与 COM 号会变，先探测再执行。
- 提交竞赛仓库前：移除临时调试代码（截图导出/自动翻页/静默打印），并把源码同步至 `contest2026_192_DX/app/hello_app/src`。

## UI 硬性规范（评审需求）

- **四角圆角区禁放任何内容**：可见区为圆角矩形（半径≈56px）。底部宽元素（按钮/信息行/列表）纵向必须结束在 y≤390；四角象限（x<56 或 x>334 且 y<56 或 y>394）不得有像素。渲染器自带角落元素也要挪：地图罗盘=`renderer_draw_compass` 已移到**顶部中央**，比例尺移到**底部中央**（y=H-150），海拔条上移到 H-172。
- **字号下限**：辅助文字一律 `ui_text_scaled(...,2,...)`（10×14px）；原生 5×7 太小。新增了 `text2_center/text2_at/text2_at_center` 辅助。
- **交互**：四角无控件；动作用中轴大按钮；**无长按**；页面跳转仅 KEY1/KEY2 短按。
- **验证方式**：开发版 `src/devshot.h` 把 `HUANGSHAN_DEV_SHOT` 置 1 → 板子每 5s 自动翻页+导出一帧 PPM → `tools/listen_ui.py COM3 photos 70` 存 PNG → 用 vision 技能 OCR 复核「四角是否有内容/字号/重叠」。**发布构建必须置 0**（当前已置 0）。
