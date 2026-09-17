cmake_minimum_required(VERSION 3.16)

if(CONFIG_EXAMPLES_HUANGSHAN_RUNNING)

set(SRCS
    src/main.c
    src/gpx_parser.c
    src/route_renderer.c
    src/gpu_epic.c
    src/touch_handler.c
    src/sensor_manager.c
    src/madgwick.c
    src/mag_mmc5603.c
    src/als_ltr303.c
    src/lcd_control.c
    src/power_manager.c
    src/dual_core.c
    src/ui.c
    src/run_engine.c
    src/kv_store.c
    src/pages.c
    src/net_link.c
    src/link.c
    src/mic_audcodec.c
    src/runtime_skill.c
    src/ai_agent.c
    src/ppp_link.c
    src/fusion/FusionAhrs.c
)

if(CONFIG_EXAMPLES_HUANGSHAN_RUNNING_USE_BT_PAN)
    list(APPEND SRCS src/bt_pan.c)
endif()

set(HUANGSHAN_INCDIRS ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The ePicasso (EPIC) GPU wrapper needs the chip HAL headers.
if(CONFIG_EXAMPLES_HUANGSHAN_RUNNING_GPU_ACCEL AND DEFINED NUTTX_CHIP_ABS_DIR)
  list(APPEND HUANGSHAN_INCDIRS
    ${NUTTX_CHIP_ABS_DIR}/include
    ${NUTTX_CHIP_ABS_DIR}/../drivers/Include
    ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/Include
    ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/sf32lb52x
    ${NUTTX_CHIP_ABS_DIR}/../external/CMSIS/Include
    ${NUTTX_CHIP_ABS_DIR}/../boards/include/config/sf32lb52x)
endif()

set(HUANGSHAN_DEFS)
if(CONFIG_EXAMPLES_HUANGSHAN_RUNNING_GPU_ACCEL AND DEFINED NUTTX_CHIP_ABS_DIR)
  list(APPEND HUANGSHAN_DEFS
    SOC_BF0_HCPU
    SF32LB52X
    HAL_TICK_PER_SECOND=1000
    USE_HAL_DRIVER)
endif()

nuttx_add_application(
  NAME    huangshan_run
  SRCS    ${SRCS}
  STACKSIZE ${CONFIG_EXAMPLES_HUANGSHAN_RUNNING_STACKSIZE}
  PRIORITY  ${CONFIG_EXAMPLES_HUANGSHAN_RUNNING_PRIORITY}
  MODULE   huangshan_running
  INCLUDE_DIRECTORIES ${HUANGSHAN_INCDIRS}
  COMPILE_FLAGS "-std=c11;-O2;-Wall;-Wextra"
  DEFINITIONS ${HUANGSHAN_DEFS}
)

endif() # CONFIG_EXAMPLES_HUANGSHAN_RUNNING
