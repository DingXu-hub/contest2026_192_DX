include $(APPDIR)/Make.defs

PRIORITY  = $(CONFIG_EXAMPLES_HUANGSHAN_RUNNING_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_HUANGSHAN_RUNNING_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_HUANGSHAN_RUNNING)

CSRCS  = main.c
CSRCS += gpx_parser.c
CSRCS += route_renderer.c
CSRCS += gpu_epic.c
CSRCS += touch_handler.c
CSRCS += sensor_manager.c
CSRCS += mag_mmc5603.c
CSRCS += als_ltr303.c
CSRCS += lcd_control.c
CSRCS += power_manager.c
CSRCS += dual_core.c

ifeq ($(CONFIG_EXAMPLES_HUANGSHAN_RUNNING_USE_BT_PAN),y)
CSRCS += bt_pan.c
endif

MAINSRC = main.c

CFLAGS += -std=c11 -O2 -Wall -Wextra
CFLAGS += -I$(APPDIR)/huangshan_running/src

include $(APPDIR)/Application.mk
