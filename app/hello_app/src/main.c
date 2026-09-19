/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * main.c - Huangshan Running: watch OS app for SF32LB52 / CO5300 AMOLED
 *
 * Pages: watch face, run (live data), route preview, stats, settings.
 * Swipe left/right switches pages; double-tap / long-press trigger page
 * actions.  The route page delegates rendering to route_renderer (with
 * ePicasso GPU acceleration on the target, software renderer on the host
 * simulator).
 *
 * Tasks:
 *   main           init, GPX source selection, supervisor heartbeat
 *   render         touch + sensors -> page_render -> fb flush @60fps
 *   sensor         50 Hz IMU + magnetometer + ALS, raise-to-wake
 *   bt             BT-PAN polling / GPX receive
 */

#include <nuttx/config.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <nuttx/video/fb.h>
#include <nuttx/cache.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/buttons.h>

#include "gpx_parser.h"
#include "route_renderer.h"
#include "touch_handler.h"
#include "ai_agent.h"
#include "link.h"
#include "app_diag.h"
#include "devshot.h"
#include "sensor_manager.h"
#include "power_manager.h"
#include "dual_core.h"
#include "pages.h"
#include "kv_store.h"
#ifdef CONFIG_EXAMPLES_HUANGSHAN_RUNNING_USE_BT_PAN
#include "bt_pan.h"
#endif

static void splash_screen(void);   /* defined below render_task */

/* Some builds (host simulator) do not define CONFIG_FB_UPDATE; keep the
 * ioctl available so the flush path compiles everywhere. */
#ifndef FBIO_UPDATE
#  define FBIO_UPDATE _FBIOC(0x0007)
#endif

#define RENDER_TASK_STACKSIZE 16384
#define RENDER_TASK_PRIORITY  100
#define SENSOR_TASK_STACKSIZE 6144
#define SENSOR_TASK_PRIORITY  110
#define BT_TASK_STACKSIZE     4096
#define BT_TASK_PRIORITY      105
#define NET_TASK_STACKSIZE    6144
#define NET_TASK_PRIORITY     115

/* SLIP network link + static IP (see net_link.c) */
extern void net_link_start(void);
extern void net_link_dump_stats(void);
extern void ppp_link_start(void);
extern void ppp_link_polldiag(int seconds);
extern void ppp_link_tundiag(void);

#define FB_DEV   "/dev/fb0"
#define TOUCH_DEV "/dev/input0"

/* ------------------------------------------------------------------ *
 * globals
 * ------------------------------------------------------------------ */

static power_manager_t  g_pm;
static sensor_manager_t g_sensors;
static render_state_t   g_renderer;
static gpx_data_t       g_gpx;
static touch_state_t    g_touch;
static app_ctx_t        g_app;
#ifdef CONFIG_EXAMPLES_HUANGSHAN_RUNNING_USE_BT_PAN
static bt_pan_t         g_bt;
#endif
static volatile bool    g_running = true;

/* set while the PPP link carries binary frames: mutes periodic printers */
volatile bool g_net_silent = false;

#if HUANGSHAN_DEV_SHOT
bool g_print_silent = false;
#endif

/* ------------------------------------------------------------------ *
 * helpers
 * ------------------------------------------------------------------ */

static uint32_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static uint32_t get_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000000 + tv.tv_usec);
}

static void load_gpx(const char *path, gpx_data_t *gpx, bool log)
{
    if (gpx_parse_file(path, gpx) == 0 && gpx->loaded)
    {
        gpx_calc_bounds(gpx);
        gpx_calc_total_distance(gpx);
        renderer_set_gpx(&g_renderer, gpx);
        renderer_center_on_point(&g_renderer, gpx->center_lat,
                                 gpx->center_lon);
        /* zoom out so the whole route fits on screen (verification
         * and sane default for large tracks) */
        renderer_set_zoom(&g_renderer, ZOOM_MIN,
                          g_renderer.buf_width / 2,
                          g_renderer.buf_height / 2);
        if (log)
        {
            printf("[App] GPX loaded from %s: %u points, %.2f km, "
                   "+%.0f m / -%.0f m\n",
                   path, gpx->point_count,
                   gpx->total_distance_m / 1000.0,
                   (double)gpx->total_ascent_m,
                   (double)gpx->total_descent_m);
        }
    }
}

/* scan the SD card for the first *.gpx offline route */
static int load_from_sd(gpx_data_t *gpx)
{
    DIR *dir;
    struct dirent *ent;
    char path[128];

    dir = opendir("/data/tf");
    if (!dir)
    {
        printf("[App] No SD card at /data/tf (%d)\n", errno);
        return -1;
    }

    while ((ent = readdir(dir)) != NULL)
    {
        size_t len = strlen(ent->d_name);
        if (len > 4 && strcasecmp(ent->d_name + len - 4, ".gpx") == 0)
        {
            snprintf(path, sizeof(path), "/data/tf/%s", ent->d_name);
            printf("[App] Found offline route: %s\n", path);
            closedir(dir);
            load_gpx(path, gpx, true);
            return 0;
        }
    }

    closedir(dir);
    printf("[App] No .gpx on SD card\n");
    return -1;
}

/* ------------------------------------------------------------------ *
 * framebuffer flush
 * ------------------------------------------------------------------ */

static int g_fb_fd = -1;
#ifdef CONFIG_ARCH_SIM
static uint32_t *g_fb32;   /* host simulator: 32bpp X11 framebuffer */
#endif

static void fb_flush(void)
{
#ifdef CONFIG_ARCH_SIM
    /* The sim X11 framebuffer is 32bpp ARGB8888; the renderer always
     * works in RGB565, so convert each frame.  (390x450 is nothing for
     * the host CPU.) */
    if (g_fb32 && g_renderer.cbuf)
    {
        int w = g_renderer.buf_width;
        int h = g_renderer.buf_height;
        for (int y = 0; y < h; y++)
        {
            const pixel_t *srow = g_renderer.cbuf + (size_t)y * w;
            uint32_t *drow = g_fb32 + (size_t)y * w;
            for (int x = 0; x < w; x++)
            {
                pixel_t px = srow[x];
                uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
                uint8_t g = (uint8_t)(((px >> 5) & 0x3F) * 255 / 63);
                uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);
                drow[x] = 0xFF000000u | ((uint32_t)r << 16) |
                          ((uint32_t)g << 8) | b;
            }
        }
    }
    return;
#endif

    if (g_fb_fd < 0)
        return;

    /* /dev/fb0 is a shadow framebuffer: FBIO_UPDATE pushes the dirty
     * region to the panel through the LCD putarea() path. */
    struct fb_area_s area;
    area.x = 0;
    area.y = 0;
    area.w = g_renderer.buf_width;
    area.h = g_renderer.buf_height;
    ioctl(g_fb_fd, FBIO_UPDATE, (unsigned long)&area);
}

#ifdef CONFIG_ARCH_SIM
/* Host simulator: export the framebuffer as a PPM so rendering can be
 * inspected headless (scp the file back and view it). */
static void sim_dump_frame(void)
{
    static uint32_t last_dump_ms;
    uint32_t now = get_time_ms();

    if (now - last_dump_ms < 800)
        return;
    last_dump_ms = now;

    char fname[48];
    snprintf(fname, sizeof(fname), "/data/frame_%d_%u.ppm",
             g_app.ui.current, (unsigned)now);
    FILE *f = fopen(fname, "w");
    if (!f)
        return;

    int w = g_renderer.buf_width;
    int h = g_renderer.buf_height;
    fprintf(f, "P6\n%d %d\n255\n", w, h);

    for (int y = 0; y < h; y++)
    {
        const pixel_t *row = g_renderer.cbuf + (size_t)y * w;
        for (int x = 0; x < w; x++)
        {
            pixel_t px = row[x];
            uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
            uint8_t g = (uint8_t)(((px >> 5) & 0x3F) * 255 / 63);
            uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);
            fputc(r, f);
            fputc(g, f);
            fputc(b, f);
        }
    }
    fclose(f);
    printf("[SIM] frame dumped page=%d (%dx%d)\n",
           g_app.ui.current, w, h);
}
#endif /* CONFIG_ARCH_SIM */

/* ------------------------------------------------------------------ *
 * touch -> page routing + route-page gestures
 * ------------------------------------------------------------------ */

static void on_touch_event(touch_event_t event, void *user_data)
{
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    render_state_t *rs = ctx->renderer;

    pm_report_activity(&g_pm);

    /* swipe switches pages; page-level actions happen first */
    page_handle_touch(ctx, event);

    /* map gestures (pan / pinch) act on the map: standalone route page
     * or the PAGE_RUN map sub-view */
    if (ctx->ui.current == PAGE_ROUTE ||
        (ctx->ui.current == PAGE_RUN && ctx->ui.run_map_view))
    {
        switch (event)
        {
        case TOUCH_EV_PAN_BEGIN:
        case TOUCH_EV_PINCH_BEGIN:
            g_pm.gpu_active = true;
            break;

        case TOUCH_EV_PAN_UPDATE:
            renderer_pan(rs,
                         g_touch.points[0].x - g_touch.pan_start_x,
                         g_touch.points[0].y - g_touch.pan_start_y);
            g_touch.pan_start_x = g_touch.points[0].x;
            g_touch.pan_start_y = g_touch.points[0].y;
            break;

        case TOUCH_EV_PINCH_UPDATE:
        {
            float scale = g_touch.pinch_distance /
                          (g_touch.pinch_start_distance > 1.0f ?
                           g_touch.pinch_start_distance : 1.0f);
            float new_zoom = rs->viewport.zoom * scale;
            if (new_zoom < ZOOM_MIN) new_zoom = ZOOM_MIN;
            if (new_zoom > ZOOM_MAX) new_zoom = ZOOM_MAX;
            renderer_set_gesture_scale(rs, new_zoom / rs->viewport.zoom);
            g_touch.pinch_start_distance = g_touch.pinch_distance;
            break;
        }

        case TOUCH_EV_PINCH_END:
        case TOUCH_EV_PAN_END:
            renderer_commit_gesture(rs);
            g_pm.gpu_active = false;
            break;

        default:
            break;
        }
    }
}


/* ------------------------------------------------------------------ *
 * console takeover
 *
 * /etc/init.d/rcS only does "huangshan_run &", then NSH's interactive
 * session keeps reading /dev/console and competes for the input bytes -
 * which is why single commands were randomly swallowed.  Once the app is
 * up the shell has done its job, so the app suspends it and the framed
 * link owns the port exclusively.
 * ------------------------------------------------------------------ */

#include <nuttx/sched.h>

static pid_t g_shell_pid;

static void find_shell(FAR struct tcb_s *tcb, FAR void *arg)
{
    (void)arg;
#if CONFIG_TASK_NAME_SIZE > 0
    if (tcb->pid == 0 || tcb->pid == getpid())
        return;
    if (strncmp(tcb->name, "nsh", 3) == 0 ||
        strncmp(tcb->name, "/bin/nsh", 8) == 0)
        g_shell_pid = tcb->pid;
#endif
}

static void console_takeover(void)
{
    g_shell_pid = 0;
    nxsched_foreach(find_shell, NULL);
    if (g_shell_pid > 0)
    {
        /* SIGSTOP: the shell stops reading; SIGCONT would bring it back */
        if (kill(g_shell_pid, SIGSTOP) == 0)
            printf("[App] console takeover: nsh pid=%d suspended\n",
                   (int)g_shell_pid);
        else
            printf("[App] console takeover: suspend pid=%d failed errno=%d\n",
                   (int)g_shell_pid, errno);
    }
    else
        printf("[App] console takeover: no shell task found\n");
}

/* ------------------------------------------------------------------ *
 * render task
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * render task
 * ------------------------------------------------------------------ */

#if HUANGSHAN_DEV_SHOT
static void shot_dump(void)
{
    const pixel_t *cb = g_renderer.cbuf;
    int w = g_renderer.buf_width, h = g_renderer.buf_height;
    int y, x;

    if (!cb)
        return;
    g_print_silent = true;
    fflush(stdout);
    printf("P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++)
    {
        const pixel_t *row = cb + (size_t)y * w;
        for (x = 0; x < w; x++)
        {
            pixel_t p = row[x];
            putchar((int)((p >> 8) & 0xF8));
            putchar((int)((p >> 3) & 0xFC));
            putchar((int)((p << 3) & 0xF8));
        }
    }
    fflush(stdout);
    g_print_silent = false;
}
#endif

static int net_task(int argc, char *argv[])
{
    /* Silence the periodic console printers: SLIP frames share this UART
     * with the console, and log text interleaved into a frame corrupts it. */
    g_net_silent = true;
    sleep(6);
    net_link_start();      /* SLIP + static IP + route + probe (blocking) */
    return 0;
}

static int net_stat_task(int argc, char *argv[])
{
    /* keep the SLIP interface counters visible: print them every 5 s (the
     * output goes into the SLIP link while it is up, so read the console
     * after stopping slattach to inspect them) */
    for (;;)
    {
        sleep(5);
        net_link_dump_stats();
    }
    return 0;
}

static int ai_task(int argc, char *argv[])
{
    ai_agent_set_context(&g_app);
    ai_agent_task();          /* never returns */
    return 0;
}

static int render_task(int argc, char *argv[])
{
    uint32_t stats_ms = get_time_ms();
    uint32_t frame_count = 0;
    float    measured_fps = 0.0f;

    printf("[Render] task started, target %d fps\n",
           CONFIG_EXAMPLES_HUANGSHAN_RUNNING_FPS_TARGET);

    /* /dev/fb0 is registered asynchronously by the LCD bringup task */
    for (int attempt = 0; attempt < 10; attempt++)
    {
        g_fb_fd = open(FB_DEV, O_RDWR);
        if (g_fb_fd >= 0)
            break;
        usleep(500 * 1000);
    }

    if (g_fb_fd >= 0)
    {
        struct fb_videoinfo_s vinfo;
        struct fb_planeinfo_s pinfo;

        if (ioctl(g_fb_fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) == 0)
        {
            g_renderer.buf_width = vinfo.xres;
            g_renderer.buf_height = vinfo.yres;
            printf("[Render] fb %dx%d fmt=%d\n",
                   vinfo.xres, vinfo.yres, vinfo.fmt);
        }
        if (ioctl(g_fb_fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) == 0)
        {
#ifdef CONFIG_ARCH_SIM
            /* The sim X11 fb is 32bpp: mmap it separately for the final
             * blit, keep the renderer on a 16bpp RGB565 buffer. */
            g_fb32 = (uint32_t *)mmap(NULL, pinfo.fblen,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED, g_fb_fd, 0);
            g_renderer.cbuf = (pixel_t *)calloc(
                g_renderer.buf_width * g_renderer.buf_height,
                sizeof(pixel_t));
            printf("[Render] sim: 32bpp fb + 16bpp render buffer\n");
#else
            g_renderer.cbuf = (pixel_t *)mmap(NULL, pinfo.fblen,
                                               PROT_READ | PROT_WRITE,
                                               MAP_SHARED, g_fb_fd, 0);
            if (g_renderer.cbuf == MAP_FAILED)
            {
                printf("[Render] mmap failed\n");
                g_renderer.cbuf = NULL;
            }
            else
            {
                printf("[Render] fb mmap %p (%u bytes)\n",
                       (void *)g_renderer.cbuf, (unsigned)pinfo.fblen);
            }
#endif
        }
    }
    else
    {
        printf("[Render] cannot open %s\n", FB_DEV);
    }

    if (!g_renderer.cbuf)
    {
        g_renderer.cbuf = (pixel_t *)calloc(
            g_renderer.buf_width * g_renderer.buf_height, sizeof(pixel_t));
    }

    touch_init(TOUCH_DEV);
    touch_set_callback(on_touch_event, &g_app);
    touch_state_reset(&g_touch);
    g_touch.surf_w = g_renderer.buf_width;
    g_touch.surf_h = g_renderer.buf_height;

    /* boot splash: logo + progress bar so the user knows the watch
     * is booting instead of staring at a blank panel */
    splash_screen();

    while (g_running)
    {
        uint32_t frame_start = get_time_ms();

        touch_poll(&g_touch);

#if HUANGSHAN_DEV_SHOT
        {
            static uint32_t dms;
            /* one 390x450 PPM frame is ~526 KB -> >5 s on the 1 Mbaud
             * console, so leave a wide gap or frames arrive torn */
            if (frame_start - dms >= 20000 && g_renderer.cbuf &&
                g_pm.screen_on)
            {
                dms = frame_start;
                shot_dump();
#if HUANGSHAN_DEV_SHOT > 1
                page_next(&g_app);      /* 2 = cycle pages, 1 = stay put */
#endif
            }
        }
#endif

        /* compass auto-rotation drives the route map; frozen while the
         * runner moves */
        if (!sensor_view_locked(&g_sensors))
            renderer_set_rotation(&g_renderer,
                                  sensor_get_heading(&g_sensors));

        /* panel re-powered (raise-to-wake / touch wake): force one
         * repaint so the display never sits on a stale or black frame */
        {
            static bool was_on;
            if (g_pm.screen_on && !was_on)
                g_app.ui.dirty = true;
            was_on = g_pm.screen_on;
        }

        if (g_pm.screen_on)
        {
#ifdef CONFIG_ARCH_SIM
            /* auto demo: watch 5s -> double-tap starts a run on the run
             * page (20s, synthetic steps) -> long-press finishes it and
             * saves history -> then cycle through all pages. */
            {
                static uint32_t demo_ms;
                uint32_t gap = 3000;

                if (frame_start - demo_ms >= gap)
                {
                    demo_ms = frame_start;
                    page_next(&g_app);
                    printf("[DEMO] page -> %d\n", g_app.ui.current);
                }
            }
#endif
            page_tick(&g_app, get_time_ms());
            {
                int drew = page_render(&g_app, get_time_ms());
                /* flush only when something was drawn: idle pages keep
                 * their last frame in the panel GRAM, and a full-screen
                 * QSPI push costs a big slice of the frame budget */
                if (drew)
                    fb_flush();
#ifdef CONFIG_ARCH_SIM
                if (drew)
                    sim_dump_frame();
#endif
            }
        }

        /* drain the small-core IPC queue (sensor/BT events) */
        dual_core_process_queue();

        pm_update(&g_pm);

        frame_count++;
        if (frame_start - stats_ms >= 2000)
        {
            measured_fps = (float)frame_count * 1000.0f /
                           (float)(frame_start - stats_ms);
            frame_count = 0;
#if APP_DIAG_VERBOSE
            printf("[Render] fps=%.0f page=%d est=%dmA bl=%d%% gpu=%d\n",
                   measured_fps, g_app.ui.current,
                   g_pm.current_ma_estimate, g_pm.backlight_pct,
                   g_renderer.gpu_ok);
#endif
            stats_ms = frame_start;
        }

        /* frame pacing */
        uint32_t target_fps = g_pm.target_fps ? g_pm.target_fps : 30;
        uint32_t interval_ms = 1000 / target_fps;
        uint32_t elapsed = get_time_ms() - frame_start;
        if (elapsed < interval_ms)
            usleep((interval_ms - elapsed) * 1000);
        else if (!g_pm.screen_on)
            usleep(100 * 1000);
    }

    if (g_fb_fd >= 0)
        close(g_fb_fd);
    renderer_deinit(&g_renderer);
    return 0;
}

/* ------------------------------------------------------------------ *
 * sensor task
 * ------------------------------------------------------------------ */

static int sensor_task(int argc, char *argv[])
{
    printf("[Sensor] task started\n");
    sensor_init(&g_sensors);

    while (g_running)
    {
        sensor_update(&g_sensors);

        /* raise-to-wake: IMU wrist lift */
        if (!g_pm.screen_on && sensor_detect_raise(&g_sensors))
            pm_wake(&g_pm);

        /* ALS -> backlight target (or manual brightness) */
        if (g_app.brightness_mode == 0)
            pm_set_backlight(&g_pm, sensor_get_backlight(&g_sensors));
        else
            pm_set_backlight(&g_pm, (uint8_t)g_app.brightness_mode);

        /* forward to the IPC queue (as the small core would) */
        {
            sensor_imu_data_t imu_msg;
            sensor_mag_data_t mag_msg;
            sensor_als_data_t als_msg;

            memset(&imu_msg, 0, sizeof(imu_msg));
            imu_msg.accel_x = g_sensors.imu.ax;
            imu_msg.accel_y = g_sensors.imu.ay;
            imu_msg.accel_z = g_sensors.imu.az;
            imu_msg.gyro_x = g_sensors.imu.gx;
            imu_msg.gyro_y = g_sensors.imu.gy;
            imu_msg.gyro_z = g_sensors.imu.gz;
            imu_msg.raise_detected = g_sensors.imu.raise_detected;
            imu_msg.view_locked = g_sensors.imu.view_locked;
            dual_core_send_sensor(SENSOR_MSG_IMU, &imu_msg,
                                  sizeof(imu_msg));

            memset(&mag_msg, 0, sizeof(mag_msg));
            mag_msg.heading_deg = g_sensors.heading_deg;
            mag_msg.mag_x = g_sensors.mag.x_raw;
            mag_msg.mag_y = g_sensors.mag.y_raw;
            mag_msg.mag_z = g_sensors.mag.z_raw;
            dual_core_send_sensor(SENSOR_MSG_MAG, &mag_msg,
                                  sizeof(mag_msg));

            memset(&als_msg, 0, sizeof(als_msg));
            als_msg.lux = g_sensors.als.lux;
            als_msg.backlight_pct = g_sensors.backlight_pct;
            dual_core_send_sensor(SENSOR_MSG_ALS, &als_msg,
                                  sizeof(als_msg));
        }

        usleep(SENSOR_SAMPLE_INTERVAL_MS * 1000);
    }

    sensor_deinit(&g_sensors);
    return 0;
}

/* ------------------------------------------------------------------ *
 * BT-PAN task
 * ------------------------------------------------------------------ */

#ifdef CONFIG_EXAMPLES_HUANGSHAN_RUNNING_USE_BT_PAN
static void on_bt_gpx_data(const char *data, uint16_t len, bool complete,
                           void *user_data)
{
    gpx_data_t *gpx = (gpx_data_t *)user_data;

    if (!complete)
        return;

#ifdef CONFIG_ARCH_SIM
    /* simulator: the local /data/huangshan.gpx route (e.g. the circle
     * test track) must win over the BT mock's built-in sample, so the
     * verification route stays on screen. */
    printf("[App] SIM: BT-PAN route ignored (keeping local gpx)\n");
    return;
#endif

    if (gpx_parse_buffer(data, len, gpx) == 0 && gpx->loaded)
    {
        gpx_calc_bounds(gpx);
        gpx_calc_total_distance(gpx);
        renderer_set_gpx(&g_renderer, gpx);
        renderer_center_on_point(&g_renderer, gpx->center_lat,
                                 gpx->center_lon);
        renderer_force_redraw(&g_renderer);
        printf("[App] GPX via BT-PAN: %u points, %.2f km\n",
               gpx->point_count, gpx->total_distance_m / 1000.0);
    }
    else
    {
        printf("[App] BT-PAN GPX parse failed\n");
    }
}

static int bt_task(int argc, char *argv[])
{
    printf("[BT] task started\n");
    bt_pan_init(&g_bt);
    bt_pan_connect_phone(&g_bt, NULL);
    bt_pan_fetch_route(&g_bt, "pan://phone/route/current.gpx",
                       on_bt_gpx_data, &g_gpx);

    while (g_running)
    {
        bt_pan_poll(&g_bt);
        usleep(100000);
    }

    bt_pan_deinit(&g_bt);
    return 0;
}
#endif /* CONFIG_EXAMPLES_HUANGSHAN_RUNNING_USE_BT_PAN */

/* ------------------------------------------------------------------ *
 * physical key navigation:
 *   KEY1 (PA34, /dev/gpio0): short press = back to watch face
 *   KEY2 (PA43, /dev/gpio1): short press = next page,
 *                            long press = page action (start/stop run,
 *                            toggle profile, compass cal)
 *   any press wakes the display.  GPIO level is polled directly so it
 *   works even if the GPIO interrupt path is unavailable.
 * ------------------------------------------------------------------ */

static int key_task(int argc, char *argv[])
{
    int fd1 = open("/dev/gpio0", O_RDONLY);   /* KEY1 PA34 */
    int fd2 = open("/dev/gpio1", O_RDONLY);   /* KEY2 PA43 */
    bool last1 = false, last2 = false;
    uint32_t press1_ms = 0, press2_ms = 0;

    {
        char b1 = '0', b2 = '0';
        if (fd1 >= 0)
        {
            lseek(fd1, 0, SEEK_SET);
            read(fd1, &b1, 1);
        }
        if (fd2 >= 0)
        {
            lseek(fd2, 0, SEEK_SET);
            read(fd2, &b2, 1);
        }
        printf("[Key] task started fd1=%d fd2=%d init K1=%c K2=%c\n",
               fd1, fd2, b1, b2);
    }

    while (g_running)
    {
        char b1 = '0', b2 = '0';
        bool k1, k2;
        uint32_t now = get_time_ms();

        if (fd1 >= 0)
        {
            lseek(fd1, 0, SEEK_SET);
            if (read(fd1, &b1, 1) != 1)
                b1 = '0';
        }
        if (fd2 >= 0)
        {
            lseek(fd2, 0, SEEK_SET);
            if (read(fd2, &b2, 1) != 1)
                b2 = '0';
        }
        k1 = (b1 == '1');
        k2 = (b2 == '1');

        /* navigation is keys-only and long-press is disabled: KEY2 short
         * = next page, KEY1 short = previous page (cyclic). */
        if (k2 && !last2)
        {
            press2_ms = now;
            pm_report_activity(&g_pm);
            if (!g_pm.screen_on)
                pm_wake(&g_pm);
        }
        else if (!k2 && last2)
        {
            if (now - press2_ms < 800)
            {
                page_next(&g_app);
                printf("[Key] KEY2 next -> %d\n", g_app.ui.current);
            }
        }

        if (k1 && !last1)
        {
            press1_ms = now;
            pm_report_activity(&g_pm);
            if (!g_pm.screen_on)
                pm_wake(&g_pm);
        }
        else if (!k1 && last1)
        {
            if (now - press1_ms < 800)
            {
                page_prev(&g_app);
                printf("[Key] KEY1 prev -> %d\n", g_app.ui.current);
            }
        }

        last1 = k1;
        last2 = k2;
        usleep(20000);
    }

    if (fd1 >= 0)
        close(fd1);
    if (fd2 >= 0)
        close(fd2);
    return 0;
}

/* read one byte from the touch controller over the raw I2C bus (diag) */
static int touch_diag_read_reg(uint8_t reg)
{
    struct i2c_msg_s msgs[2];
    struct i2c_transfer_s xfer;
    uint8_t val = 0;
    int fd = open("/dev/i2c0", O_RDWR);

    if (fd < 0)
        return -1;

    msgs[0].frequency = 400000;
    msgs[0].addr = 0x38;
    msgs[0].flags = 0;
    msgs[0].buffer = &reg;
    msgs[0].length = 1;

    msgs[1].frequency = 400000;
    msgs[1].addr = 0x38;
    msgs[1].flags = I2C_M_READ;
    msgs[1].buffer = &val;
    msgs[1].length = 1;

    xfer.msgv = msgs;
    xfer.msgc = 2;

    if (ioctl(fd, I2CIOC_TRANSFER, (unsigned long)&xfer) < 0)
    {
        close(fd);
        return -1;
    }
    close(fd);
    return val;
}

/* FT6146 diagnostic: raw 16-byte dump from 0x00 + id */
static void touch_diag(int *mode, int *td, int *idh, int *raw0, int *raw2,
                       int *raw4, int *raw8)
{
    uint8_t buf[16];
    struct i2c_msg_s msgs[2];
    struct i2c_transfer_s xfer;
    uint8_t reg = 0x00;
    int fd = open("/dev/i2c0", O_RDWR);

    *mode = -1; *td = -1; *idh = -1;
    *raw0 = *raw2 = *raw4 = *raw8 = -1;

    if (fd < 0)
        return;

    msgs[0].frequency = 400000;
    msgs[0].addr = 0x38;
    msgs[0].flags = 0;
    msgs[0].buffer = &reg;
    msgs[0].length = 1;

    msgs[1].frequency = 400000;
    msgs[1].addr = 0x38;
    msgs[1].flags = I2C_M_READ;
    msgs[1].buffer = buf;
    msgs[1].length = sizeof(buf);

    xfer.msgv = msgs;
    xfer.msgc = 2;

    if (ioctl(fd, I2CIOC_TRANSFER, (unsigned long)&xfer) < 0)
    {
        close(fd);
        return;
    }
    close(fd);

    *mode = buf[0x00];
    *td   = buf[0x02] & 0x0f;
    *idh  = touch_diag_read_reg(0xa3);
    *raw0 = buf[0];
    *raw2 = buf[2];
    *raw4 = buf[4];
    *raw8 = buf[8];
}

/* ------------------------------------------------------------------ *
 * boot splash screen (logo + progress bar)
 * ------------------------------------------------------------------ */

static void splash_screen(void)
{
    int w = g_renderer.buf_width;
    int h = g_renderer.buf_height;
    uint32_t t0 = get_time_ms();
    const uint32_t dur = 1400;   /* ~1.4 s so the boot state is visible */

    if (!g_renderer.cbuf || w <= 0 || h <= 0)
        return;

    while (get_time_ms() - t0 < dur)
    {
        uint32_t now = get_time_ms();
        float t = (float)(now - t0) / (float)dur;
        int i;

        for (i = 0; i < w * h; i++)
            g_renderer.cbuf[i] = UI_BG;

        /* logo */
        ui_text(g_renderer.cbuf, w, h,
                (w - 9 * 6) / 2, h / 2 - 60, "HUANGSHAN", UI_ACCENT);
        ui_text(g_renderer.cbuf, w, h,
                (w - 3 * 6) / 2, h / 2 - 48, "RUN", UI_TEXT);
        ui_icon(g_renderer.cbuf, w, h, w / 2 - 8, h / 2 - 84,
                UI_ICON_RUN, UI_ACCENT);

        /* progress bar */
        int bx = (w - 220) / 2;
        int by = h / 2 + 20;
        ui_bar(g_renderer.cbuf, w, h, bx, by, 220, 6, t,
               UI_ACCENT, 0x2124);

        fb_flush();
        usleep(30000);
    }

    /* back to black before the first real frame */
    for (int i = 0; i < w * h; i++)
        g_renderer.cbuf[i] = UI_BG;
    fb_flush();
}

/* ------------------------------------------------------------------ *
 * entry point
 * ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    /* NSH's own console is muted (stdin /dev/null, stdout/stderr /dev/null)
     * so the UART stays clean for PPP.  Point our own output back at the
     * real console so the board logs remain visible. */
    {
        int cfd = open("/dev/console", O_WRONLY);
        if (cfd >= 0)
        {
            dup2(cfd, 1);
            dup2(cfd, 2);
            if (cfd > 2)
                close(cfd);
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);

#ifdef CONFIG_ARCH_SIM
    /* Host simulator: map /data to the host /tmp directory so the app
     * can read the GPX route and export render frames to the host. */
    mkdir("/data", 0755);
    {
        int mr = mount("", "/data", "hostfs", 0, "fs=/tmp");
        printf("[SIM] hostfs mount ret=%d\n", mr);
    }
#endif

    printf("[Huangshan] watch OS starting...\n");

    memset(&g_pm, 0, sizeof(g_pm));
    memset(&g_sensors, 0, sizeof(g_sensors));
    memset(&g_renderer, 0, sizeof(g_renderer));
    memset(&g_gpx, 0, sizeof(g_gpx));

    renderer_init(&g_renderer);
    pm_init(&g_pm);

#if APP_DEMO_SEED
    /* Probe configuration check: /data is a tmpfs mounted by the board
     * bringup; the KV store lives there (see kv_store.c). */
#endif
    dual_core_init();
    dual_core_set_sensor_callback(NULL);
    dual_core_set_bt_callback(NULL);

    app_ctx_init(&g_app, &g_renderer, &g_sensors, &g_pm, &g_gpx);

    /* ---- GPX source selection ---- */
    const char *gpx_path = (argc > 1) ? argv[1] : NULL;

#ifdef CONFIG_EXAMPLES_HUANGSHAN_RUNNING_USE_SDCARD
    if (!gpx_path)
        load_from_sd(&g_gpx);
#endif

    if (gpx_path && (strcmp(gpx_path, "-") != 0))
    {
        load_gpx(gpx_path, &g_gpx, true);
    }

    /* last-resort local route: romfs on target, host file on sim */
    if (!g_gpx.loaded)
    {
#ifdef CONFIG_ARCH_SIM
        load_gpx("/data/huangshan.gpx", &g_gpx, true);
#else
        load_gpx("/etc/data/gpx/sample.gpx", &g_gpx, true);
#endif
    }

    if (!g_gpx.loaded)
    {
        printf("[App] No local GPX yet; BT-PAN will deliver the route\n");
    }

    /* ---- tasks ---- */
    task_create("huangshan_render", RENDER_TASK_PRIORITY,
                RENDER_TASK_STACKSIZE, render_task, NULL);
    task_create("huangshan_sensor", SENSOR_TASK_PRIORITY,
                SENSOR_TASK_STACKSIZE, sensor_task, NULL);
#ifdef CONFIG_EXAMPLES_HUANGSHAN_RUNNING_USE_BT_PAN
    task_create("huangshan_bt", BT_TASK_PRIORITY,
                BT_TASK_STACKSIZE, bt_task, NULL);
#endif
    task_create("huangshan_key", SENSOR_TASK_PRIORITY,
                SENSOR_TASK_STACKSIZE, key_task, NULL);
    /* Network tasks are DISABLED for the AI-agent (serial relay) mode:
     * slip_initialize() would open /dev/console and steal the input bytes
     * that the AI agent's CLI channel needs.  Re-enable when the kernel
     * netdev issue is resolved. */
#if 0
    task_create("huangshan_net", NET_TASK_PRIORITY,
                NET_TASK_STACKSIZE, net_task, NULL);
    task_create("huangshan_nst", NET_TASK_PRIORITY + 1,
                NET_TASK_STACKSIZE, net_stat_task, NULL);
#endif
    /* the framed link owns /dev/console; every text line it decodes goes
     * to the AI agent, so the PC gateway can talk in frames while a bare
     * serial terminal still works with raw lines */
    console_takeover();
    link_set_text_sink(ai_agent_feed_line);
    task_create("huangshan_link", 120, 8192, link_task, NULL);   /* the AI self-test runs here */
    task_create("huangshan_ai", 120, 6144, ai_task, NULL);

#if APP_DEMO_SEED
    /* One-shot demo seed (default off): gives the stats page a few
     * plausible runs for screenshots/video, and parks the UI on that page.
     * Switch APP_DEMO_SEED on in app_diag.h, flash once, then flash the
     * normal build again (the watch stores the KV in RAM, so the entries
     * live until the next reboot). */
    if (kv_history_count() == 0)
    {
        static const struct { float m; uint32_t s; float asc; } demo[] = {
            { 5020.0f, 1685u, 42.0f },
            { 3120.0f, 1042u, 18.0f },
            { 7410.0f, 2510u, 96.0f },
            { 2050.0f,  690u,  9.0f },
            { 6330.0f, 2140u, 61.0f },
        };
        int di;

        for (di = 0; di < (int)(sizeof(demo) / sizeof(demo[0])); di++)
            kv_history_push(demo[di].m, demo[di].s, demo[di].asc);
        printf("[App] demo seed: %d runs in the history\n",
               kv_history_count());

        /* Give the map page a route with the shape a real run produces: a
         * closed loop, because a dead-reckoned trail is only a straight line
         * when nothing is steered.  Live runs always use the real trail from
         * run_engine.c; this is DEMO DATA for screenshots (see
         * docs/交接文档.md). */
        {
            enum { DEMO_TRACK_N = 150 };
            float px = 0.0f, py = 0.0f, dist = 0.0f;
            int i;

            g_app.run.track_count = 0;
            for (i = 0; i < DEMO_TRACK_N; i++)
            {
                float t = (float)i / (float)(DEMO_TRACK_N - 1);
                float a = t * 6.2831853f;
                /* ellipse + harmonics: a loop, not a perfect Oval */
                float x = 620.0f * cosf(a) + 55.0f * cosf(3.0f * a);
                float y = 430.0f * sinf(a) + 40.0f * sinf(2.0f * a);
                run_track_point_t *tp = &g_app.run.track[i];

                if (i > 0)
                    dist += sqrtf((x - px) * (x - px) + (y - py) * (y - py));
                px = x;
                py = y;

                tp->x = x;
                tp->y = y;
                tp->pace_s_per_km = 320.0f + 45.0f * sinf(2.5f * a);
                tp->heading_deg = 0.0f;
                g_app.run.track_count++;
            }
            /* bearing of each point = direction to the next one (north = 0,
             * clockwise positive), the same convention run_engine uses */
            for (i = 0; i + 1 < g_app.run.track_count; i++)
            {
                float dx = g_app.run.track[i + 1].x - g_app.run.track[i].x;
                float dy = g_app.run.track[i + 1].y - g_app.run.track[i].y;
                float b = atan2f(dx, dy) * 57.2957795f;
                g_app.run.track[i].heading_deg = b < 0.0f ? b + 360.0f : b;
            }
            g_app.run.track[g_app.run.track_count - 1].heading_deg =
                g_app.run.track[g_app.run.track_count - 2].heading_deg;

            g_app.run.state = RUN_FINISHED;
            g_app.run.distance_m = dist;
            g_app.run.running_ms = (uint32_t)(dist / 2.65f * 1000.0f);
            g_app.run.steps = (uint32_t)(dist / 0.75f);
            g_app.run.cadence_spm = 168.0f;
            g_app.run.avg_pace_s_per_km = 265.0f;
            g_app.run.pace_s_per_km = 265.0f;
            g_app.run.pos_x = px;
            g_app.run.pos_y = py;
            g_app.run.heading_deg = g_app.run.track[0].heading_deg;
            g_app.run_saved = true;      /* do not push the demo into history */
            printf("[App] demo track: %d points, %.0f m loop\n",
                   g_app.run.track_count, (double)dist);
        }
        page_set(&g_app, PAGE_STATS);
    }
#endif

#if APP_DEMO_RUN
    /* Demo build: start a simulated run immediately and park the UI on the run
     * page.  Steps come from the steady cadence synthesised in run_engine.c and
     * the bearing turns at a constant rate, so the dead-reckoned trail closes
     * into a circle - for screenshots/video when a real outdoor run is not
     * possible.  The trail is SIMULATED (say so in the video); flash the normal
     * build afterwards. */
    run_start(&g_app.run);
    g_app.run.heading_deg = 0.0f;      /* relative bearing, anchored at boot */
    g_app.run_saved = true;            /* never push the simulated run into history */
    renderer_reset_live_fit(&g_renderer);
    page_set(&g_app, PAGE_RUN);
    printf("[App] demo run: %d spm + %.1f deg/s turn -> circular trail\n",
           APP_DEMO_RUN_SPM, (double)APP_DEMO_RUN_YAW_DPS);
#endif

    /* supervisor heartbeat */
    uint32_t hb = get_time_ms();
    uint32_t touch_check_ms = 0;
    while (g_running)
    {
        sleep(1);

        /* immediate touch detection: report the instant the FT6146
         * touch-count register goes non-zero */
        if (get_time_ms() - touch_check_ms >= 500)
        {
            int m, t, id, r0, r2, r4, r8;
            touch_check_ms = get_time_ms();
            touch_diag(&m, &t, &id, &r0, &r2, &r4, &r8);
            if (t > 0)
            {
#if APP_DIAG_VERBOSE
                printf("[TOUCH!] td=%d raw=[%02x %02x %02x %02x]\n",
                       t, r0 & 0xff, r2 & 0xff, r4 & 0xff, r8 & 0xff);
#endif
            }
        }

        if (get_time_ms() - hb >= 5000)
        {
            uint32_t downs = 0;
            uint32_t samples = touch_get_stats(&downs);
            int dmode = -1, dtd = -1, didh = -1;
            int r0 = -1, r2 = -1, r4 = -1, r8 = -1;
            touch_diag(&dmode, &dtd, &didh, &r0, &r2, &r4, &r8);
            {
                uint32_t tm = 0, tu = 0;
                uint32_t lf = touch_get_flagstats(&tm, &tu);
                uint32_t np = touch_get_npoints();
#if APP_DIAG_VERBOSE
                if (!g_net_silent)
                    printf("[Alive] st=%d scr=%d page=%d imu=%d "
                       "a=(%.2f,%.2f,%.2f) gz=%.1f pit=%.1f raise=%d "
                       "mag=(%d,%d,%d) h=%.0f lux=%d "
                       "tp(m=%d td=%d id=%d r:[%02x %02x %02x %02x]) "
                       "t=%u/%u D%d/M%lu/U%lu f=%lu n=%lu bl=%d%%\n",
                   g_pm.state, g_pm.screen_on, g_app.ui.current,
                   g_sensors.imu.present,
                   g_sensors.imu.ax, g_sensors.imu.ay, g_sensors.imu.az,
                   g_sensors.imu.gz,
                   g_sensors.imu.pitch,
                   g_sensors.imu.raise_detected,
                   g_sensors.mag.x_raw, g_sensors.mag.y_raw,
                   g_sensors.mag.z_raw,
                   g_sensors.heading_deg,
                   g_sensors.als.filtered_lux,
                   dmode, dtd, didh, r0 & 0xff, r2 & 0xff, r4 & 0xff,
                   r8 & 0xff,
                   (unsigned)samples, (unsigned)downs,
                   (unsigned)downs, (unsigned long)tm, (unsigned long)tu,
                   (unsigned long)lf, (unsigned long)np,
                   g_pm.backlight_pct);
#endif
            }
            hb = get_time_ms();
        }
    }

    return 0;
}
