#include "gpx_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EARTH_RADIUS_M  6371000.0
#define DEG_TO_RAD      0.017453292519943295
#define RAD_TO_DEG      57.29577951308232

static int parse_xml_value(const char *buf, const char *tag, char *out, size_t out_len)
{
    char open_tag[64];
    char close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(buf, open_tag);
    if (!start) {
        char alt_open[64];
        snprintf(alt_open, sizeof(alt_open), "<%s ", tag);
        start = strstr(buf, alt_open);
        if (start) {
            const char *end_attr = strchr(start, '>');
            if (!end_attr) return -1;
            start = end_attr + 1;
        } else {
            return -1;
        }
    } else {
        start += strlen(open_tag);
    }

    const char *end = strstr(start, close_tag);
    if (!end) return -1;

    size_t len = end - start;
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

static int parse_xml_attr(const char *buf, const char *tag, const char *attr,
                          char *out, size_t out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "<%s ", tag);
    const char *tag_start = strstr(buf, search);
    if (!tag_start) return -1;

    const char *tag_end = strchr(tag_start, '>');
    if (!tag_end) return -1;

    char attr_search[64];
    snprintf(attr_search, sizeof(attr_search), "%s=\"", attr);
    const char *attr_start = strstr(tag_start, attr_search);
    if (!attr_start || attr_start > tag_end) return -1;

    attr_start += strlen(attr_search);
    const char *attr_end = strchr(attr_start, '"');
    if (!attr_end || attr_end > tag_end) return -1;

    size_t len = attr_end - attr_start;
    if (len >= out_len) len = out_len - 1;
    memcpy(out, attr_start, len);
    out[len] = '\0';
    return 0;
}

static int parse_iso_time(const char *str, uint32_t *utc_out)
{
    struct tm tm_buf;
    memset(&tm_buf, 0, sizeof(tm_buf));
    int ms = 0;
    char tz_sign = 0;
    int tz_h = 0, tz_m = 0;

    if (sscanf(str, "%d-%d-%dT%d:%d:%d.%d%c%d:%d",
               &tm_buf.tm_year, &tm_buf.tm_mon, &tm_buf.tm_mday,
               &tm_buf.tm_hour, &tm_buf.tm_min, &tm_buf.tm_sec,
               &ms, &tz_sign, &tz_h, &tz_m) >= 6) {
        tm_buf.tm_year -= 1900;
        tm_buf.tm_mon -= 1;
        time_t t = mktime(&tm_buf);
        if (tz_sign == '+') t -= (tz_h * 3600 + tz_m * 60);
        if (tz_sign == '-') t += (tz_h * 3600 + tz_m * 60);
        *utc_out = (uint32_t)t;
        return 0;
    }

    if (sscanf(str, "%d-%d-%dT%d:%d:%dZ",
               &tm_buf.tm_year, &tm_buf.tm_mon, &tm_buf.tm_mday,
               &tm_buf.tm_hour, &tm_buf.tm_min, &tm_buf.tm_sec) == 6) {
        tm_buf.tm_year -= 1900;
        tm_buf.tm_mon -= 1;
        *utc_out = (uint32_t)mktime(&tm_buf);
        return 0;
    }

    return -1;
}

int gpx_parse_file(const char *path, gpx_data_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -errno;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 1024 * 1024) {
        fclose(f);
        return -1;
    }

    char *buf = (char *)malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        return -ENOMEM;
    }

    size_t read_len = fread(buf, 1, fsize, f);
    fclose(f);
    buf[read_len] = '\0';

    int ret = gpx_parse_buffer(buf, read_len, out);
    free(buf);
    return ret;
}

int gpx_parse_buffer(const char *buf, size_t len, gpx_data_t *out)
{
    if (!buf || !out) return -EINVAL;

    memset(out, 0, sizeof(gpx_data_t));
    out->min_elevation = 99999.0f;
    out->max_elevation = -99999.0f;

    const char *trkseg_start = strstr(buf, "<trkseg>");
    if (!trkseg_start) return -1;

    const char *trkseg_end = strstr(trkseg_start, "</trkseg>");
    if (!trkseg_end) return -1;

    const char *pos = trkseg_start;
    while (pos < trkseg_end && out->point_count < GPX_MAX_TRACKPOINTS) {
        const char *trkpt = strstr(pos, "<trkpt ");
        if (!trkpt || trkpt >= trkseg_end) break;

        char lat_str[32], lon_str[32];
        if (parse_xml_attr(trkpt, "trkpt", "lat", lat_str, sizeof(lat_str)) != 0)
            { pos = trkpt + 1; continue; }
        if (parse_xml_attr(trkpt, "trkpt", "lon", lon_str, sizeof(lon_str)) != 0)
            { pos = trkpt + 1; continue; }

        double lat = atof(lat_str);
        double lon = atof(lon_str);

        float ele = 0.0f;
        char ele_str[32];
        if (parse_xml_value(trkpt, "ele", ele_str, sizeof(ele_str)) == 0) {
            ele = atof(ele_str);
        }

        uint32_t time_utc = 0;
        char time_str[64];
        if (parse_xml_value(trkpt, "time", time_str, sizeof(time_str)) == 0) {
            parse_iso_time(time_str, &time_utc);
        }

        gpx_trackpoint_t *pt = &out->points[out->point_count++];
        pt->lat = lat;
        pt->lon = lon;
        pt->ele = ele;
        pt->time_utc = time_utc;

        if (ele < out->min_elevation) out->min_elevation = ele;
        if (ele > out->max_elevation) out->max_elevation = ele;

        pos = strchr(trkpt + 1, '>');
        if (pos) pos++;
    }

    const char *wpt_pos = buf;
    while (out->waypoint_count < GPX_MAX_WAYPOINTS) {
        const char *wpt = strstr(wpt_pos, "<wpt ");
        if (!wpt) break;

        char lat_str[32], lon_str[32], name_str[GPX_MAX_NAME_LEN];
        if (parse_xml_attr(wpt, "wpt", "lat", lat_str, sizeof(lat_str)) == 0 &&
            parse_xml_attr(wpt, "wpt", "lon", lon_str, sizeof(lon_str)) == 0) {

            gpx_waypoint_t *wp = &out->waypoints[out->waypoint_count++];
            wp->lat = atof(lat_str);
            wp->lon = atof(lon_str);

            if (parse_xml_value(wpt, "name", name_str, sizeof(name_str)) == 0) {
                strncpy(wp->name, name_str, GPX_MAX_NAME_LEN - 1);
            }
        }

        wpt_pos = strchr(wpt + 1, '>');
        if (wpt_pos) wpt_pos++;
    }

    out->loaded = true;
    return 0;
}

double gpx_point_distance(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * DEG_TO_RAD;
    double dlon = (lon2 - lon1) * DEG_TO_RAD;
    double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
               cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
               sin(dlon / 2.0) * sin(dlon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return EARTH_RADIUS_M * c;
}

void gpx_calc_bounds(gpx_data_t *gpx)
{
    if (!gpx || gpx->point_count == 0) return;

    gpx->lat_min = gpx->lat_max = gpx->points[0].lat;
    gpx->lon_min = gpx->lon_max = gpx->points[0].lon;

    for (uint16_t i = 1; i < gpx->point_count; i++) {
        if (gpx->points[i].lat < gpx->lat_min) gpx->lat_min = gpx->points[i].lat;
        if (gpx->points[i].lat > gpx->lat_max) gpx->lat_max = gpx->points[i].lat;
        if (gpx->points[i].lon < gpx->lon_min) gpx->lon_min = gpx->points[i].lon;
        if (gpx->points[i].lon > gpx->lon_max) gpx->lon_max = gpx->points[i].lon;
    }

    gpx->center_lat = (gpx->lat_min + gpx->lat_max) / 2.0;
    gpx->center_lon = (gpx->lon_min + gpx->lon_max) / 2.0;
}

void gpx_calc_total_distance(gpx_data_t *gpx)
{
    if (!gpx || gpx->point_count < 2) return;

    gpx->total_distance_m = 0.0;
    gpx->total_ascent_m = 0.0f;
    gpx->total_descent_m = 0.0f;

    for (uint16_t i = 1; i < gpx->point_count; i++) {
        double d = gpx_point_distance(
            gpx->points[i-1].lat, gpx->points[i-1].lon,
            gpx->points[i].lat, gpx->points[i].lon);
        gpx->total_distance_m += d;

        float ele_diff = gpx->points[i].ele - gpx->points[i-1].ele;
        if (ele_diff > 0) gpx->total_ascent_m += ele_diff;
        if (ele_diff < 0) gpx->total_descent_m += (-ele_diff);
    }
}

float gpx_calc_gradient(uint16_t idx, gpx_data_t *gpx)
{
    if (!gpx || idx == 0 || idx >= gpx->point_count) return 0.0f;

    double d = gpx_point_distance(
        gpx->points[idx-1].lat, gpx->points[idx-1].lon,
        gpx->points[idx].lat, gpx->points[idx].lon);

    if (d < 0.5) return 0.0f;

    return (gpx->points[idx].ele - gpx->points[idx-1].ele) / (float)d * 100.0f;
}

uint8_t gpx_get_color_for_elevation(float ele, float min_ele, float max_ele)
{
    float range = max_ele - min_ele;
    if (range < 1.0f) return 0x33;

    float t = (ele - min_ele) / range;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    uint8_t r = (uint8_t)(t * 255.0f);
    uint8_t g = (uint8_t)((1.0f - t) * 200.0f + 55.0f);
    uint8_t b = (uint8_t)((1.0f - t) * 255.0f);
    return (r << 5) | (g << 2) | (b >> 3);
}

uint8_t gpx_get_color_for_gradient(float grad_pct)
{
    if (grad_pct < 0.0f) return 0x1F;
    if (grad_pct < 3.0f) return 0xE0;
    if (grad_pct < 6.0f) return 0xFC;
    if (grad_pct < 10.0f) return 0x1C;
    return 0x03;
}

uint8_t gpx_get_color_for_distance(double dist_km, double total_km)
{
    if (total_km < 0.001) return 0xE0;
    double t = dist_km / total_km;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    if (t < 0.25) return 0xE0;
    if (t < 0.50) return 0xFC;
    if (t < 0.75) return 0x1F;
    return 0x03;
}

int gpx_append_point(gpx_data_t *gpx, double lat, double lon, float ele, uint32_t time)
{
    if (!gpx || gpx->point_count >= GPX_MAX_TRACKPOINTS) return -1;

    gpx_trackpoint_t *pt = &gpx->points[gpx->point_count++];
    pt->lat = lat;
    pt->lon = lon;
    pt->ele = ele;
    pt->time_utc = time;

    return 0;
}

void gpx_free(gpx_data_t *gpx)
{
    if (gpx) memset(gpx, 0, sizeof(gpx_data_t));
}

/* Mercator projection helpers (shared with renderer) */
static inline double mercator_lon_to_x(double lon, double center_lon)
{
    return (lon - center_lon) / 360.0;
}

static inline double mercator_lat_to_y(double lat, double center_lat)
{
    double lat_rad = lat * DEG_TO_RAD;
    double center_rad = center_lat * DEG_TO_RAD;
    double y = log(tan(M_PI / 4.0 + lat_rad / 2.0));
    double y_center = log(tan(M_PI / 4.0 + center_rad / 2.0));
    return (y - y_center) / (2.0 * M_PI);
}
