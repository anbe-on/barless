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
static GtkWidget *tray_card = NULL, *tray_box = NULL, *w_netchart = NULL;

/* net history for the chart: newest sample last, one per second */
#define HIST_MAX 600
static double h_down[HIST_MAX], h_up[HIST_MAX];
static int h_n = 30, h_count = 0;          /* h_n = seconds shown ([dashboard] net_chart_seconds) */

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

/* two-line chart of download (solid) and upload (dimmer): colour = CSS `color` of #netchart */
static gboolean netchart_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    (void)d;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    GdkRGBA c;
    gtk_style_context_get_color(gtk_widget_get_style_context(w), gtk_widget_get_state_flags(w), &c);
    double max = 10 * 1024;                          /* floor: an idle line stays flat, not noisy */
    int n = MIN(h_n, h_count);
    for (int i = 0; i < n; i++) {
        int k = h_count - n + i;
        double a = h_down[k % HIST_MAX], b = h_up[k % HIST_MAX];
        if (a > max) max = a;
        if (b > max) max = b;
    }
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, 0.2);          /* baseline */
    cairo_move_to(cr, 0, H - 0.5); cairo_line_to(cr, W, H - 0.5); cairo_stroke(cr);
    for (int pass = 0; pass < 2; pass++) {                           /* 0 = upload (behind), 1 = download */
        cairo_set_source_rgba(cr, c.red, c.green, c.blue, pass ? c.alpha : c.alpha * 0.45);
        cairo_set_line_width(cr, 1.5);
        for (int i = 0; i < h_n; i++) {
            int back = h_n - 1 - i;                                  /* samples ago */
            double v = 0;
            if (back < h_count) { int k = h_count - 1 - back; v = (pass ? h_down : h_up)[k % HIST_MAX]; }
            double x = h_n > 1 ? (double)i * (W - 1) / (h_n - 1) : 0;
            double y = (H - 2) - v / max * (H - 4);
            if (i == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }
    return FALSE;
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
        if (!g_ascii_strcasecmp(*st, "cpu")) stat_row(grid, row++, "cpu", &w_cpu);
        else if (!g_ascii_strcasecmp(*st, "temp")) stat_row(grid, row++, "temp", &w_temp);
        else if (!g_ascii_strcasecmp(*st, "battery")) stat_row(grid, row++, "battery", &w_bat);
        else if (!g_ascii_strcasecmp(*st, "net")) {
            stat_row(grid, row++, "net", &w_net);
            if (cfg_bool("dashboard", "net_chart", TRUE)) {
                w_netchart = gtk_drawing_area_new();
                gtk_widget_set_name(w_netchart, "netchart");
                gtk_widget_set_size_request(w_netchart, -1, MAX(12, cfg_int("dashboard", "chart_height", 36)));
                g_signal_connect(w_netchart, "draw", G_CALLBACK(netchart_draw), NULL);
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

/* net runs on its own timer: every second while the chart is shown (that is what feeds it) */
static gboolean net_tick(gpointer d) {
    (void)d;
    double down, up;
    char ds[32], us[32], b[96];
    net_read(&down, &up);
    h_down[h_count % HIST_MAX] = down;
    h_up[h_count % HIST_MAX] = up;
    h_count++;
    fmt_speed(down, ds, sizeof ds);
    fmt_speed(up, us, sizeof us);
    snprintf(b, sizeof b, "\xe2\x96\xbc %s  \xe2\x96\xb2 %s", ds, us);   /* down = download, up = upload */
    gtk_label_set_text(GTK_LABEL(w_net), b);
    if (w_netchart) gtk_widget_queue_draw(w_netchart);
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

    if (w_cpu) {
        snprintf(b, sizeof b, "%d%%", cpu_read());
        gtk_label_set_text(GTK_LABEL(w_cpu), b);
    }
    if (w_temp) {
        int temp = temp_read();
        if (temp >= 0) snprintf(b, sizeof b, "%d\xc2\xb0""C", temp); else snprintf(b, sizeof b, "n/a");
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

    h_n = CLAMP(cfg_int("dashboard", "net_chart_seconds", 30), 5, HIST_MAX);
    cpu_read();                 /* prime the delta counters */
    double d0, u0;
    net_read(&d0, &u0);
    dash_tick(NULL);
    g_timeout_add_seconds(interval, dash_tick, NULL);
    if (w_net) g_timeout_add_seconds(w_netchart ? 1 : interval, net_tick, NULL);

    g_unix_signal_add(SIGTERM, quit_cb, NULL);
    g_unix_signal_add(SIGINT, quit_cb, NULL);
    gtk_main();
    return 0;
}
