#ifndef __GPX_PARSER_H
#define __GPX_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define GPX_MAX_TRACKPOINTS  1024   /* 4096 -> 1024: BSS saved ~100 KB */
#define GPX_MAX_WAYPOINTS     128
#define GPX_MAX_NAME_LEN      128

typedef struct {
    double lat;
    double lon;
    float  ele;
    uint32_t time_utc;
} gpx_trackpoint_t;

typedef struct {
    double lat;
    double lon;
    float  ele;
    char   name[GPX_MAX_NAME_LEN];
} gpx_waypoint_t;

typedef struct {
    gpx_trackpoint_t points[GPX_MAX_TRACKPOINTS];
    uint16_t point_count;
    gpx_waypoint_t   waypoints[GPX_MAX_WAYPOINTS];
    uint16_t waypoint_count;
    double lat_min, lat_max;
    double lon_min, lon_max;
    double center_lat, center_lon;
    double total_distance_m;
    float  total_ascent_m;
    float  total_descent_m;
    float  max_elevation;
    float  min_elevation;
    bool   loaded;
} gpx_data_t;

int  gpx_parse_file(const char *path, gpx_data_t *out);
int  gpx_parse_buffer(const char *buf, size_t len, gpx_data_t *out);
int  gpx_append_point(gpx_data_t *gpx, double lat, double lon, float ele, uint32_t time);
void gpx_free(gpx_data_t *gpx);
void gpx_calc_bounds(gpx_data_t *gpx);
double gpx_point_distance(double lat1, double lon1, double lat2, double lon2);
void  gpx_calc_total_distance(gpx_data_t *gpx);
float gpx_calc_gradient(uint16_t idx, gpx_data_t *gpx);

uint8_t gpx_get_color_for_elevation(float ele, float min_ele, float max_ele);
uint8_t gpx_get_color_for_gradient(float grad_pct);
uint8_t gpx_get_color_for_distance(double dist_km, double total_km);

#endif
