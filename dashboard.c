/* dashboard.c - the empty-workspace dashboard. THIS IS YOUR PLAYGROUND.
 *
 * It runs as its own process (panelless --dashboard): the always-on indicator
 * starts it when the active Hyprland workspace has no windows and stops it
 * when a window appears, so everything here costs nothing while you work.
 *
 * To iterate on the design without touching the indicator:
 *     make && ./panelless --dashboard
 * Styling is style.css (reloads on save). Buttons come from config.ini.
 *
 * Everything lives in ONE container (the GtkBox passed to build_dashboard()).
 * Add widgets there. Give them a name (#id) or a class (.class) and style them
 * in style.css. */
#include "panelless.h"
#include <glib-unix.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/prctl.h>

/* widgets that dash_tick() refreshes */
static GtkWidget *w_clock, *w_date, *w_cpu, *w_temp, *w_net, *w_bat;
static GtkWidget *tray_card = NULL, *tray_box = NULL;
static GtkWidget *w_netchart = NULL, *w_cpuchart = NULL, *w_tempchart = NULL, *w_batchart = NULL;

typedef struct {
    HistMetric metric;
    int seconds;
    char name[32];
} ChartInfo;

static GtkAlign align_cfg(const char *key, const char *def) {
    char *v = cfg_str("dashboard", key, def);
    GtkAlign a = GTK_ALIGN_START;
    if (!g_ascii_strcasecmp(v, "center") || !g_ascii_strcasecmp(v, "middle")) a = GTK_ALIGN_CENTER;
    else if (!g_ascii_strcasecmp(v, "right") || !g_ascii_strcasecmp(v, "end")) a = GTK_ALIGN_END;
    g_free(v);
    return a;
}

static void on_tray_changed(int n) {
    if (tray_card) gtk_widget_set_visible(tray_card, n > 0);
}

static void on_button_clicked(GtkButton *b, gpointer cmd) {
    (void)b;
    GError *e = NULL;
    if (!g_spawn_command_line_async((const char *)cmd, &e)) {
        g_printerr("panelless: cannot run '%s': %s\n", (const char *)cmd, e->message);
        g_error_free(e);
    }
}

static void free_cmd(gpointer d, GClosure *c) { (void)c; g_free(d); }

/* a "card": rounded block with a style class you can restyle */
static GtkWidget *new_card(GtkWidget *root, GtkWidget *content) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");
    gtk_container_add(GTK_CONTAINER(card), content);
    gtk_box_pack_start(GTK_BOX(root), card, FALSE, FALSE, 0);
    return card;
}

static GtkWidget *stat_row(GtkWidget *grid, int row, const char *key, GtkWidget **val) {
    GtkWidget *k = gtk_label_new(key);
    *val = gtk_label_new("-");
    gtk_style_context_add_class(gtk_widget_get_style_context(k), "stat-key");
    gtk_style_context_add_class(gtk_widget_get_style_context(*val), "stat-val");
    gtk_widget_set_halign(k, align_cfg("key_align", "left"));
    gtk_widget_set_halign(*val, align_cfg("value_align", "right"));
    gtk_widget_set_hexpand(k, TRUE);
    gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), *val, 1, row, 1, 1);
    return k;
}

/* Universal chart drawing (net, cpu, temp, battery). Colour = CSS `color` of widget */
static gboolean chart_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    ChartInfo *ci = (ChartInfo *)d;
    if (!ci) return FALSE;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    GdkRGBA c;
    gtk_style_context_get_color(gtk_widget_get_style_context(w), gtk_widget_get_state_flags(w), &c);

    int n = ci->seconds;
    if (n < 5) n = 5;
    if (n > HIST_CAPACITY) n = HIST_CAPACITY;

    double p1[HIST_CAPACITY], p2[HIST_CAPACITY];
    int count = history_get(ci->metric, p1, p2, n);

    double max = 1.0;
    if (ci->metric == HIST_METRIC_NET) {
        max = 10 * 1024;                          /* floor: an idle line stays flat, not noisy */
        for (int i = 0; i < count; i++) {
            if (p1[i] > max) max = p1[i];
            if (p2[i] > max) max = p2[i];
        }
    } else if (ci->metric == HIST_METRIC_CPU || ci->metric == HIST_METRIC_BATTERY) {
        max = 100.0;
    } else if (ci->metric == HIST_METRIC_TEMP) {
        max = 100.0;
        for (int i = 0; i < count; i++) {
            if (p1[i] > max) max = p1[i];
        }
    }

    /* Baseline */
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, 0.2);
    cairo_move_to(cr, 0, H - 0.5); cairo_line_to(cr, W, H - 0.5); cairo_stroke(cr);

    int passes = (ci->metric == HIST_METRIC_NET) ? 2 : 1;
    for (int pass = 0; pass < passes; pass++) {
        gboolean is_sec = (ci->metric == HIST_METRIC_NET && pass == 0);
        double *data = is_sec ? p2 : p1;
        double line_alpha = is_sec ? c.alpha * 0.45 : c.alpha;

        cairo_set_source_rgba(cr, c.red, c.green, c.blue, line_alpha);
        cairo_set_line_width(cr, 1.5);

        for (int i = 0; i < n; i++) {
            int back = n - 1 - i;
            double v = 0;
            if (back < count) {
                v = data[count - 1 - back];
            }
            double x = n > 1 ? (double)i * (W - 1) / (n - 1) : 0;
            double y = (H - 2) - (v / max) * (H - 4);
            if (y < 2) y = 2;
            if (y > H - 2) y = H - 2;

            if (i == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }
    return FALSE;
}

static GtkWidget *create_chart(HistMetric metric, const char *name, int def_sec, int def_h) {
    char sec_key[64], h_key[64];
    snprintf(sec_key, sizeof sec_key, "%s_seconds", name);
    snprintf(h_key, sizeof h_key, "%s_height", name);

    int g_sec = cfg_int("dashboard", "chart_seconds", 30);
    int g_h = cfg_int("dashboard", "chart_height", 36);

    ChartInfo *ci = g_new0(ChartInfo, 1);
    ci->metric = metric;
    ci->seconds = CLAMP(cfg_int("dashboard", sec_key, g_sec ? g_sec : def_sec), 5, HIST_CAPACITY);
    int h = MAX(12, cfg_int("dashboard", h_key, g_h ? g_h : def_h));
    g_strlcpy(ci->name, name, sizeof(ci->name));

    GtkWidget *w = gtk_drawing_area_new();
    gtk_widget_set_name(w, name);
    gtk_style_context_add_class(gtk_widget_get_style_context(w), "chart");
    gtk_widget_set_size_request(w, -1, h);
    g_signal_connect_data(w, "draw", G_CALLBACK(chart_draw), ci, (GClosureNotify)g_free, 0);
    return w;
}

/* ================================================================== */
/* ====================== EDIT HERE: the layout ===================== */
/* ================================================================== */
static void build_dashboard(GtkWidget *root) {
    /* --- clock + date --- */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    w_clock = gtk_label_new("--:--");
    w_date = gtk_label_new("");
    gtk_widget_set_name(w_clock, "bigclock");
    gtk_widget_set_name(w_date, "date");
    gtk_widget_set_halign(w_clock, align_cfg("clock_align", "right"));
    gtk_widget_set_halign(w_date, align_cfg("date_align", "right"));
    gtk_container_add(GTK_CONTAINER(head), w_clock);
    gtk_container_add(GTK_CONTAINER(head), w_date);
    new_card(root, head);

    /* --- live stats: [dashboard] stats=cpu,temp,net,battery  (order = order shown, leave one out to hide it) --- */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    char *stats_s = cfg_str("dashboard", "stats", "net,battery");
    char **stats = g_strsplit_set(stats_s, ", ", -1);
    int row = 0;
    for (char **st = stats; *st; st++) {
        if (!**st) continue;
        if (!g_ascii_strcasecmp(*st, "cpu")) {
            stat_row(grid, row++, "cpu", &w_cpu);
            if (cfg_bool("dashboard", "cpu_chart", FALSE)) {
                w_cpuchart = create_chart(HIST_METRIC_CPU, "cpuchart", 30, 36);
                gtk_grid_attach(GTK_GRID(grid), w_cpuchart, 0, row++, 2, 1);
            }
        } else if (!g_ascii_strcasecmp(*st, "temp")) {
            stat_row(grid, row++, "temp", &w_temp);
            if (cfg_bool("dashboard", "temp_chart", FALSE)) {
                w_tempchart = create_chart(HIST_METRIC_TEMP, "tempchart", 30, 36);
                gtk_grid_attach(GTK_GRID(grid), w_tempchart, 0, row++, 2, 1);
            }
        } else if (!g_ascii_strcasecmp(*st, "battery")) {
            stat_row(grid, row++, "battery", &w_bat);
            if (cfg_bool("dashboard", "battery_chart", FALSE)) {
                w_batchart = create_chart(HIST_METRIC_BATTERY, "batterychart", 30, 36);
                gtk_grid_attach(GTK_GRID(grid), w_batchart, 0, row++, 2, 1);
            }
        } else if (!g_ascii_strcasecmp(*st, "net")) {
            stat_row(grid, row++, "net", &w_net);
            if (cfg_bool("dashboard", "net_chart", TRUE)) {
                w_netchart = create_chart(HIST_METRIC_NET, "netchart", 30, 36);
                gtk_grid_attach(GTK_GRID(grid), w_netchart, 0, row++, 2, 1);
            }
        }
    }
    g_strfreev(stats);
    g_free(stats_s);
    if (row) new_card(root, grid); else gtk_widget_destroy(grid);

    /* --- buttons: one [button:xxx] group in config.ini each --- */
    GtkWidget *bgrid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(bgrid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(bgrid), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(bgrid), TRUE);
    int cols = MAX(1, cfg_int("dashboard", "columns", 3)), i = 0;
    gboolean show_labels = cfg_bool("dashboard", "show_labels", TRUE);
    char **groups = cfg_groups();
    for (char **g = groups; *g; g++) {
        if (!g_str_has_prefix(*g, "button:")) continue;
        char *label = cfg_str(*g, "label", *g + 7);
        char *icon = cfg_str(*g, "icon", "");
        char *cmd = cfg_str(*g, "cmd", NULL);
        char *tip = cfg_str(*g, "tooltip", NULL);
        gboolean hide = cfg_bool(*g, "hide", FALSE), term = cfg_bool(*g, "terminal", FALSE);
        if (cmd && !hide) {
            char *text = (*icon && show_labels && *label) ? g_strdup_printf("%s %s", icon, label)
                       : *icon ? g_strdup(icon) : g_strdup(label);
            char *run;
            if (term) {                                  /* run inside a terminal (TUI apps like btop, nmtui) */
                char *tc = cfg_str("dashboard", "terminal", "xdg-terminal-exec");
                run = g_strdup_printf("%s %s", tc, cmd);
                g_free(tc);
            } else run = g_strdup(cmd);
            GtkWidget *b = gtk_button_new_with_label(text);
            g_free(text);
            gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(b)), align_cfg("button_align", "center"));
            gtk_style_context_add_class(gtk_widget_get_style_context(b), "action");
            gtk_widget_set_name(b, *g + 7);                       /* #menu, #wifi, #shutdown ... */
            if (tip) gtk_widget_set_tooltip_text(b, tip);
            g_signal_connect_data(b, "clicked", G_CALLBACK(on_button_clicked), run, free_cmd, 0);
            gtk_grid_attach(GTK_GRID(bgrid), b, i % cols, i / cols, 1, 1);
            i++;
        }
        g_free(label); g_free(icon); g_free(cmd); g_free(tip);
    }
    g_strfreev(groups);
    new_card(root, bgrid);

    /* --- tray (hidden until an app has an icon) --- */
    tray_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    tray_card = new_card(root, tray_box);
    gtk_widget_set_no_show_all(tray_card, TRUE);     /* hidden until the first icon arrives... */
    gtk_widget_show(tray_box);                       /* ...but the box inside must still be shown */
}
/* ================================================================== */

/* chart tick runs every second while any chart or net row is shown */
static gboolean net_tick(gpointer d) {
    (void)d;
    history_tick();
    double down = 0, up = 0;
    if (!history_latest(NULL, NULL, &down, &up, NULL)) {
        net_read(&down, &up);
    }
    if (w_net) {
        char ds[32], us[32], b[96];
        fmt_speed(down, ds, sizeof ds);
        fmt_speed(up, us, sizeof us);
        snprintf(b, sizeof b, "\xe2\x96\xbc %s  \xe2\x96\xb2 %s", ds, us);   /* down = download, up = upload */
        gtk_label_set_text(GTK_LABEL(w_net), b);
    }
    if (w_netchart) gtk_widget_queue_draw(w_netchart);
    if (w_cpuchart) gtk_widget_queue_draw(w_cpuchart);
    if (w_tempchart) gtk_widget_queue_draw(w_tempchart);
    if (w_batchart) gtk_widget_queue_draw(w_batchart);
    return G_SOURCE_CONTINUE;
}

static gboolean dash_tick(gpointer d) {
    (void)d;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char b[160];
    strftime(b, sizeof b, "%H:%M", &tm);
    gtk_label_set_text(GTK_LABEL(w_clock), b);
    strftime(b, sizeof b, "%A, %d %B %Y", &tm);
    gtk_label_set_text(GTK_LABEL(w_date), b);

    double cpu = 0, temp = 0;
    gboolean has_hist = history_latest(&cpu, &temp, NULL, NULL, NULL);

    if (w_cpu) {
        int cpupct = has_hist ? (int)cpu : cpu_read();
        snprintf(b, sizeof b, "%d%%", cpupct);
        gtk_label_set_text(GTK_LABEL(w_cpu), b);
    }
    if (w_temp) {
        int tval = has_hist ? (int)temp : temp_read();
        if (tval >= 0) snprintf(b, sizeof b, "%d\xc2\xb0""C", tval); else snprintf(b, sizeof b, "n/a");
        gtk_label_set_text(GTK_LABEL(w_temp), b);
    }
    if (!w_bat) return G_SOURCE_CONTINUE;

    BatInfo bat;
    bat_read(&bat);
    if (!bat.present)
        snprintf(b, sizeof b, "none");
    else if (bat.charging)
        snprintf(b, sizeof b, "%d%%  \xe2\x96\xb2 %.1fW", bat.cap, bat.watts);
    else if (bat.discharging)
        snprintf(b, sizeof b, "%d%%  \xe2\x96\xbc %.1fW  %s", bat.cap, bat.watts, bat.rem);
    else
        snprintf(b, sizeof b, "%d%%", bat.cap);
    gtk_label_set_text(GTK_LABEL(w_bat), b);
    return G_SOURCE_CONTINUE;
}

/* ---------- middle-click drag to move the panel (position remembered) ---------- */
static GtkWidget *g_win, *g_root;
static GdkRectangle g_mon = { 0, 0, 1920, 1080 };
static int g_width, g_margin, g_margin_top;
static gboolean g_free_pos = FALSE, g_drag = FALSE, g_movable = TRUE;
static int g_x, g_y, g_gx, g_gy;
static gboolean g_ipc = FALSE;         /* have a global cursor (Hyprland)? then use it: it needs no feedback */
static gint64 g_last_move = 0;

static char *pos_path(void) { return g_build_filename(cfg_dir(), "position.ini", NULL); }

static void apply_pos(void) {
    gtk_layer_set_margin(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_LEFT, g_x);
    gtk_layer_set_margin(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_TOP, g_y);
    tray_set_origin(g_mon.x + g_x, g_mon.y + g_y);
    gtk_widget_queue_resize(g_win);                  /* makes GTK commit the new margins now */
    gtk_widget_queue_draw(g_win);
}

static void go_free(int x, int y) {     /* switch from "right column" to a free top-left position */
    g_free_pos = TRUE;
    g_x = x; g_y = y;
    gtk_layer_set_anchor(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_RIGHT, FALSE);
    gtk_layer_set_anchor(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_BOTTOM, FALSE);
    gtk_layer_set_margin(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_RIGHT, 0);
    gtk_layer_set_margin(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
    apply_pos();
}

/* event coordinates are relative to whichever child widget got the event: convert to the window's */
static void win_xy(GdkEvent *ev, double x, double y, int *ox, int *oy) {
    GtkWidget *w = gtk_get_event_widget(ev);
    int wx = (int)x, wy = (int)y;
    if (w && w != g_win) gtk_widget_translate_coordinates(w, g_win, (int)x, (int)y, &wx, &wy);
    *ox = wx; *oy = wy;
}

static gboolean on_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    (void)w; (void)d;
    if (e->button != 2 || !g_movable) return FALSE;
    if (!g_free_pos) go_free(g_mon.width - g_margin - g_width, g_margin_top);
    g_drag = TRUE;
    win_xy((GdkEvent *)e, e->x, e->y, &g_gx, &g_gy);      /* the spot of the panel we are holding */
    double cx, cy;
    g_ipc = hypr_cursor(&cx, &cy);
    if (g_ipc) { g_gx = (int)cx - g_mon.x - g_x; g_gy = (int)cy - g_mon.y - g_y; }
    return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer d) {
    (void)w; (void)d;
    if (!g_drag) return FALSE;
    gint64 now = g_get_monotonic_time() / 1000;
    if (now - g_last_move < 16) return TRUE;           /* one move per frame */
    int lx, ly, nx, ny;
    double cx, cy;
    if (g_ipc && hypr_cursor(&cx, &cy)) {              /* global pointer - held spot = new origin */
        lx = (int)cx - g_mon.x; ly = (int)cy - g_mon.y;
        nx = lx - g_gx; ny = ly - g_gy;
    } else {                                           /* other compositors: follow the drift */
        win_xy((GdkEvent *)e, e->x, e->y, &lx, &ly);
        nx = g_x + lx - g_gx; ny = g_y + ly - g_gy;
    }
    if (nx == g_x && ny == g_y) return TRUE;
    int ph = gtk_widget_get_allocated_height(g_root);
    g_x = CLAMP(nx, 0, MAX(0, g_mon.width - g_width));
    g_y = CLAMP(ny, 0, MAX(0, g_mon.height - ph));
    g_last_move = now;
    apply_pos();
    return TRUE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *e, gpointer d) {
    (void)w; (void)d;
    if (e->button != 2 || !g_drag) return FALSE;
    g_drag = FALSE;
    double cx, cy;
    if (g_ipc && hypr_cursor(&cx, &cy)) {            /* settle on where the pointer really let go */
        int ph = gtk_widget_get_allocated_height(g_root);
        g_x = CLAMP((int)cx - g_mon.x - g_gx, 0, MAX(0, g_mon.width - g_width));
        g_y = CLAMP((int)cy - g_mon.y - g_gy, 0, MAX(0, g_mon.height - ph));
        apply_pos();
    }
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "dashboard", "x", g_x);
    g_key_file_set_integer(kf, "dashboard", "y", g_y);
    char *p = pos_path();
    g_key_file_save_to_file(kf, p, NULL);
    g_free(p);
    g_key_file_free(kf);
    return TRUE;
}

static void load_pos(void) {
    GKeyFile *kf = g_key_file_new();
    char *p = pos_path();
    if (g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL) &&
        g_key_file_has_key(kf, "dashboard", "x", NULL) && g_key_file_has_key(kf, "dashboard", "y", NULL)) {
        int x = g_key_file_get_integer(kf, "dashboard", "x", NULL);
        int y = g_key_file_get_integer(kf, "dashboard", "y", NULL);
        go_free(CLAMP(x, 0, MAX(0, g_mon.width - g_width)), CLAMP(y, 0, MAX(0, g_mon.height - 200)));
    }
    g_free(p);
    g_key_file_free(kf);
}

static gboolean quit_cb(gpointer d) {
    (void)d;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

int dashboard_run(gboolean spawned) {
    if (spawned) prctl(PR_SET_PDEATHSIG, SIGTERM);   /* die with the indicator process */

    int width = cfg_int("dashboard", "width", 380);
    int margin = cfg_int("dashboard", "margin", 12);
    int margin_top = cfg_int("dashboard", "margin_top", 40);    /* leaves room for the indicator */
    int interval = MAX(1, cfg_int("dashboard", "interval", 5));

    /* corner radius from config (lower priority than style.css, so the CSS can still override) */
    {
        int r = MAX(0, cfg_int("dashboard", "radius", 0));
        char rc[128];
        snprintf(rc, sizeof rc, ".card, button.action { border-radius: %dpx; }", r);
        GtkCssProvider *rp = gtk_css_provider_new();
        gtk_css_provider_load_from_data(rp, rc, -1, NULL);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(rp),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    /* background transparency and card alpha from config */
    {
        char *bg = cfg_str("dashboard", "bg", NULL);
        if (!bg) bg = cfg_str("dashboard", "background", NULL);
        char css_str[512];
        if (bg && *bg) {
            snprintf(css_str, sizeof css_str, "window#dashboard { background-color: %s; }", bg);
        } else {
            double alpha = cfg_dbl("dashboard", "bg_alpha", 0.0);
            snprintf(css_str, sizeof css_str, "window#dashboard { background-color: rgba(22, 22, 29, %.2f); }", alpha);
        }
        g_free(bg);

        double card_alpha = cfg_dbl("dashboard", "card_alpha", -1.0);
        if (card_alpha >= 0.0) {
            char card_css[128];
            snprintf(card_css, sizeof card_css, " .card { background-color: alpha(@foreground, %.2f); }", card_alpha);
            g_strlcat(css_str, card_css, sizeof(css_str));
        }

        GtkCssProvider *bp = gtk_css_provider_new();
        gtk_css_provider_load_from_data(bp, css_str, -1, NULL);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(bp),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(win, "dashboard");
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_BOTTOM);
    gtk_layer_set_namespace(GTK_WINDOW(win), "panelless-dashboard");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, margin_top);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, margin);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, margin);

    GdkVisual *vis = gdk_screen_get_rgba_visual(gtk_widget_get_screen(win));
    if (vis) gtk_widget_set_visual(win, vis);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(root, "panel");
    gtk_widget_set_size_request(root, width, -1);
    gtk_widget_set_valign(root, GTK_ALIGN_START);
    gtk_container_add(GTK_CONTAINER(win), root);

    build_dashboard(root);

    /* where the window sits on screen, so tray apps can place their menus */
    GdkMonitor *m = gdk_display_get_monitor(gdk_display_get_default(), 0);
    GdkRectangle g = { 0, 0, 1920, 1080 };
    if (m) gdk_monitor_get_geometry(m, &g);
    g_mon = g; g_win = win; g_root = root;
    g_width = width; g_margin = margin; g_margin_top = margin_top;
    g_movable = cfg_bool("dashboard", "movable", TRUE);
    tray_set_origin(g.x + g.width - margin - width, g.y + margin_top);
    if (g_movable) {
        gtk_widget_add_events(win, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                   GDK_POINTER_MOTION_MASK | GDK_BUTTON_MOTION_MASK);
        g_signal_connect(win, "button-press-event", G_CALLBACK(on_press), NULL);
        g_signal_connect(win, "button-release-event", G_CALLBACK(on_release), NULL);
        g_signal_connect(win, "motion-notify-event", G_CALLBACK(on_motion), NULL);
        load_pos();
    }
    gtk_widget_show_all(win);
    tray_init(tray_box, cfg_int("dashboard", "tray_icon", 18), on_tray_changed);

    history_init();
    history_tick();
    dash_tick(NULL);
    g_timeout_add_seconds(interval, dash_tick, NULL);
    gboolean any_chart = (w_netchart || w_cpuchart || w_tempchart || w_batchart);
    if (w_net || any_chart) g_timeout_add_seconds(any_chart ? 1 : interval, net_tick, NULL);

    g_unix_signal_add(SIGTERM, quit_cb, NULL);
    g_unix_signal_add(SIGINT, quit_cb, NULL);
    gtk_main();
    return 0;
}
