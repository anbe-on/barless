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

/* ---- indicator.c ---------------------------------------------------- */
int  indicator_run(void);
void hypr_info(void);
gboolean hypr_cursor(double *x, double *y);
GtkWidget *indicator_expand(GtkWidget *card, int top, int right);   /* the indicator window becomes the card ... */
void indicator_collapse(GtkWidget *card);        /* ... and back (destroys the card widget) */
void indicator_demo(void);                       /* --dashboard: open the card at start */

GtkWidget *indicator_row(void);
void indicator_recheck(void);
GtkWidget *indicator_overlay_begin(GtkWidget *stage, int *mw, int *mh, int *x0, int *y0, int *rw, int *rh);
void indicator_overlay_arrived(void);
void indicator_overlay_end(GtkWidget *stage);
char *barless_pidfile(void);

/* ---- launcher.c ----------------------------------------------------- */
void launcher_open(void);
void launcher_close(void);
void launcher_toggle(void);
int  launcher_dmenu(int argc, char **argv);

/* ---- card.c: the one expanding card ---------------------------------- */
void card_init(void);                            /* once: css, tray */
void card_open(void);                            /* grow (build the window) */
void card_close(void);                           /* animate away, then destroy the window */
gboolean card_is_closed(void);
GtkWidget *card_embed_new(int inner_w, const char *sections, void (*done)(void));   /* the card as a widget for the launcher */
void card_embed_free(void);
double card_embed_film(double te_ms, double *total, int *element_delay);
void card_embed_film_end(void);
int card_embed_expand_ms(void);
void card_set_standalone(void);
int  launcher_menu(int argc, char **argv);            /* --menu: the launcher in a process of its own */
char *accent_color(void);                        /* accent from config / theme (g_free) */

/* ---- tray.c --------------------------------------------------------- */
/* box == NULL: only act as the StatusNotifier watcher (no icons are loaded).
 * changed(n) is called with the number of visible tray items. */
void tray_init(GtkWidget *box, int icon_px, void (*changed)(int n));
void tray_set_origin(int x, int y);              /* screen position of the window holding the box */
GList *tray_get_widgets(void);                   /* list of GtkWidget* (buttons) for active tray items */

#endif
