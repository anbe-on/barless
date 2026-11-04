#ifndef BARLESS_H
#define BARLESS_H

#include <gtk/gtk.h>
#include <gtk-layer-shell/gtk-layer-shell.h>

/* ---- common.c: config, css, sensor readers ------------------------- */
const char *cfg_dir(void);                       /* ~/.config/barless */
void     cfg_load(void);                         /* creates default files on first run */
int      cfg_int (const char *grp, const char *key, int def);
double   cfg_dbl (const char *grp, const char *key, double def);
gboolean cfg_bool(const char *grp, const char *key, gboolean def);
char    *cfg_str (const char *grp, const char *key, const char *def);   /* g_free() it */
char   **cfg_groups(void);                       /* g_strfreev() it */

void     css_init(void);                         /* load style.css, hot-reload on change */

const char *sysfs_root(void);                    /* "/sys", or $PANELLESS_SYSFS for testing */

typedef struct {
    gboolean present;
    int      cap;                                /* 0..100 */
    gboolean charging, discharging;
    double   watts;                              /* magnitude, >= 0 */
    char     rem[16];                            /* "h:mm" or "" */
} BatInfo;
void        bat_read(BatInfo *b);
int         temp_read(void);                     /* deg C, -1 when there is no sensor */
const char *temp_sensor_path(void);
int         cpu_read(void);                      /* % since the previous call */
void        net_read(double *down_bps, double *up_bps);   /* since the previous call */
void        fmt_speed(double bps, char *out, size_t len);

/* Persistent history ring buffer across dashboard restarts */
#define HIST_CAPACITY 600                        /* 10 minutes at 1 sample/sec */

typedef enum {
    HIST_METRIC_NET,
    HIST_METRIC_CPU,
    HIST_METRIC_TEMP,
    HIST_METRIC_BATTERY
} HistMetric;

void history_init(void);
void history_tick(void);                         /* sample and record once per second */
int  history_get(HistMetric metric, double *p1, double *p2, int max_samples);
gboolean history_latest(double *cpu, double *temp, double *net_down, double *net_up, BatInfo *bat);

/* ---- indicator.c ---------------------------------------------------- */
int  indicator_run(void);
void hypr_info(void);
gboolean hypr_cursor(double *x, double *y);

/* ---- dashboard.c ---------------------------------------------------- */
int  dashboard_run(gboolean spawned);

/* ---- tray.c --------------------------------------------------------- */
/* box == NULL: only act as the StatusNotifier watcher (no icons are loaded).
 * changed(n) is called with the number of visible tray items. */
void tray_init(GtkWidget *box, int icon_px, void (*changed)(int n));
void tray_set_origin(int x, int y);              /* screen position of the window holding the box */

#endif
