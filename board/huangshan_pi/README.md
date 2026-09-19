# 板级覆盖：lckfb_huangshan_pi（黄山派 Pi / SF32LB52）

本目录是**板级改动清单**，用于让评审在本仓基础上复现我们的构建与资源数字。
官方骨架 `board/contest_board/` 保持不动（它是样例占位板）。

## 1. 目标板与构建路径（我们实际用的）

| 项 | 值 |
|---|---|
| 板级目录 | `vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/` |
| 应用落位 | `apps/examples/huangshan_running/`（= 本仓 `app/hello_app/`） |
| defconfig | `vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh/defconfig` |
| 构建目标 | `cmake --build . --target nuttx`（关掉 `CONFIG_ALLSYMS` 后不再是 `final_nuttx`） |
| 烧录 | `sftool.exe ... write_flash "nuttx.bin@0x12010000"` |

## 2. 必须加到 vendor defconfig 的开关

```conf
# 应用本身（Kconfig 符号名见 app/hello_app/Kconfig）
CONFIG_LVX_USE_DEMO_CONTEST2026_192_HELLO_APP=y

# 资源优化（合计省 ~228 KB SRAM / ~445 KB Flash；复现 SRAM 81.8% -> 38.2%）
# CONFIG_ALLSYMS is not set      # 符号表 g_allsyms 原占 125,736 B
# CONFIG_DEBUG_NET is not set    # 发布固件串口静默

# 控制台仲裁：NSH 不再抢 /dev/console，应用在 main() 里接管输入
CONFIG_NSH_ALTCONDEV=y
CONFIG_NSH_ALTSTDIN="/dev/null"
CONFIG_NSH_ALTSTDOUT="/dev/null"
CONFIG_NSH_ALTSTDERR="/dev/null"
```

## 3. 源码内的缓冲尺寸（不必改板级）

`GPX_MAX_TRACKPOINTS 4096→1024`、`GPX_MAX_WAYPOINTS 256→128`（`src/gpx_parser.h`）、
`RUN_TRACK_MAX 2048→1024`（`src/run_engine.h`）——这三项在应用源码里，随本仓一起提交。

## 4. 待补（VM 在线时导出的完整文件）

完整 defconfig 需从构建环境导出后放本目录（`configs/nsh/defconfig`）以保证逐字节可复现：

```bash
cp /home/openvela/vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh/defconfig \
   board/huangshan_pi/configs/nsh/defconfig
```

> 说明：本仓库不重复托管 SiFli vendor 树（由 manifest 获取），因此只固化"我们改动的那几行"，
> 以保证改动可审计、可复现；上表列出的行即为我们相对 vendor 默认值的全部板级差异。
