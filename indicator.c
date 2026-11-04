/* indicator.c - the always-on top-right indicator + Hyprland IPC watcher.
 *
 * Shows: HH:MM   CPU temp   battery square (+ direction arrow + watts).
 * Also watches Hyprland and starts/stops the dashboard process when the active
 * workspace has no windows. */
#include "panelless.h"
#include <glib-unix.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static GtkWidget *win, *time_l, *temp_l, *bat_da, *arrow_l, *watts_l;
static int m_top, m_right, shift_px, shift_ix = 0;
static gboolean dodge_on, dodged = FALSE;
static int dodge_ms;
static guint dodge_id = 0;
static int bat_cap = 0;
static gboolean bat_charging = FALSE, bat_discharging = FALSE;
static double track_alpha;
static int radius;
static int bat_mid, bat_low, temp_warm, temp_hot;
static gboolean show_temp, show_watts, bat_square, bat_arrow, bat_both;
static gboolean hide_on_empty = FALSE;

/* ================================================================== */
/* Hyprland IPC (plain unix sockets, no deps)                          */
/* ================================================================== */
static int hypr_connect(const char *name) {
    const char *sig = g_getenv("HYPRLAND_INSTANCE_SIGNATURE"), *xrd = g_getenv("XDG_RUNTIME_DIR");
    if (!sig || !xrd) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    int n = snprintf(a.sun_path, sizeof a.sun_path, "%s/hypr/%s/%s", xrd, sig, name);
    if (n <= 0 || (size_t)n >= sizeof a.sun_path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

/* One request/response on the command socket (what hyprctl does). NULL on failure. */
static char *hypr_request(const char *cmd) {
    int fd = hypr_connect(".socket.sock");
    if (fd < 0) return NULL;
    struct timeval tv = { 0, 300000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (write(fd, cmd, strlen(cmd)) < 0) { close(fd); return NULL; }
    GString *s = g_string_new(NULL);
    char buf[2048];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) g_string_append_len(s, buf, n);
    close(fd);
    if (s->len == 0) { g_string_free(s, TRUE); return NULL; }
    return g_string_free(s, FALSE);
}

static double json_num(const char *json, const char *key, double def) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p || !(p = strchr(p, ':'))) return def;
    char *end;
    double v = strtod(p + 1, &end);
    return end == p + 1 ? def : v;
}

static int active_ws_windows(void) {
    char *r = hypr_request("j/activeworkspace");
    if (!r) return -1;
    int n = (int)json_num(r, "windows", -1);
    g_free(r);
    return n;
}

gboolean hypr_cursor(double *x, double *y) {
    char *r = hypr_request("j/cursorpos");
    if (!r) return FALSE;
    *x = json_num(r, "x", -1e9);
    *y = json_num(r, "y", -1e9);
    g_free(r);
    return *x > -1e8;
}

void hypr_info(void) {
    const char *sig = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
    g_print("hyprland:   %s\n", sig ? sig : "(HYPRLAND_INSTANCE_SIGNATURE not set: dashboard auto-open disabled)");
    if (!sig) return;
    int n = active_ws_windows();
    g_print("workspace:  %s\n", n < 0 ? "(could not query the command socket)" : "");
    if (n >= 0) g_print("            %d window(s) on the active workspace\n", n);
}

/* ================================================================== */
/* Dashboard process: spawned when the workspace is empty              */
/* ================================================================== */
static GSubprocess *dash = NULL;
static gint64 dash_started = 0;
static char *self_exe = NULL;
static guint recheck_id = 0;

static void schedule_recheck(guint ms);

static void dash_exited(GObject *src, GAsyncResult *res, gpointer d) {
    (void)d;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
    if (dash == G_SUBPROCESS(src)) g_clear_object(&dash);
    schedule_recheck(100);        /* it may have been stopped just as the workspace emptied again */
}

static void dash_start(void) {
    if (dash) return;
    gint64 now = g_get_monotonic_time();
    if (now - dash_started < 1000000) { schedule_recheck(1000); return; }   /* no crash loops */
    dash_started = now;
    char *exe = g_strdup(self_exe);
    char *del = strstr(exe, " (deleted)");     /* binary was replaced while running */
    if (del) *del = '\0';
    const char *argv[] = { exe, "--dashboard", "--spawned", NULL };
    GError *e = NULL;
    dash = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_NONE, &e);
    if (!dash) {
        g_printerr("panelless: cannot start dashboard: %s\n", e->message);
        g_error_free(e);
    } else {
        g_subprocess_wait_async(dash, NULL, dash_exited, NULL);
    }
    g_free(exe);
}

static void dash_stop(void) {
    if (dash) g_subprocess_send_signal(dash, SIGTERM);
}

static gboolean recheck(gpointer d) {
    (void)d;
    recheck_id = 0;
    int n = active_ws_windows();
    if (n < 0) return G_SOURCE_REMOVE;
    if (n == 0) {
        dash_start();
        if (hide_on_empty && win) gtk_widget_set_visible(win, FALSE);
    } else {
        dash_stop();
        if (hide_on_empty && win) gtk_widget_set_visible(win, TRUE);
    }
    return G_SOURCE_REMOVE;
}

static void schedule_recheck(guint ms) {
    if (!recheck_id) recheck_id = g_timeout_add(ms, recheck, NULL);
}

static gboolean ev_is(const char *line, const char *name) {
    size_t n = strlen(name);
    return !strncmp(line, name, n) && line[n] == '>' && line[n + 1] == '>';
}

static gboolean relevant_event(const char *line) {
    static const char *const names[] = {
        "workspace", "workspacev2", "createworkspace", "createworkspacev2",
        "destroyworkspace", "destroyworkspacev2", "openwindow", "closewindow",
        "movewindow", "movewindowv2", "focusedmon", "focusedmonv2",
        "activespecial", "activespecialv2", "monitoradded", "monitorremoved", NULL };
    for (int i = 0; names[i]; i++) if (ev_is(line, names[i])) return TRUE;
    return FALSE;
}

static GString *evbuf = NULL;

static gboolean on_hypr_events(gint fd, GIOCondition cond, gpointer d) {
    (void)d;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0 || (cond & (G_IO_HUP | G_IO_ERR))) {      /* Hyprland went away: the session is ending */
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    g_string_append_len(evbuf, buf, n);
    char *nl;
    while ((nl = memchr(evbuf->str, '\n', evbuf->len))) {
        *nl = '\0';
        if (relevant_event(evbuf->str)) schedule_recheck(80);   /* debounce bursts */
        g_string_erase(evbuf, 0, (nl - evbuf->str) + 1);
    }
    return G_SOURCE_CONTINUE;
}

static void hypr_watch_start(void) {
    int fd = hypr_connect(".socket2.sock");
    if (fd < 0) {
        g_printerr("panelless: Hyprland event socket not available: dashboard will not auto-open\n");
        return;
    }
    evbuf = g_string_new(NULL);
    g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_hypr_events, NULL);
    schedule_recheck(50);
}

/* ================================================================== */
/* Indicator window                                                    */
/* ================================================================== */
static void set_state_class(GtkWidget *w, const char *cls) {
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    static const char *const all[] = { "ok", "mid", "low", NULL };
    for (int i = 0; all[i]; i++) gtk_style_context_remove_class(sc, all[i]);
    if (cls) gtk_style_context_add_class(sc, cls);
}

static void set_temp_class(const char *cls) {
    GtkStyleContext *sc = gtk_widget_get_style_context(temp_l);
    gtk_style_context_remove_class(sc, "warm");
    gtk_style_context_remove_class(sc, "hot");
    if (cls) gtk_style_context_add_class(sc, cls);
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,         G_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2,  G_PI);
    cairo_arc(cr, x + r,     y + r,     r, G_PI,      3 * G_PI / 2);
    cairo_close_path(cr);
}

static void draw_shape_path(cairo_t *cr, int shape, double x, double y, double s) {
    if (shape == 1) { /* UP triangle (charging) */
        cairo_new_sub_path(cr);
        cairo_move_to(cr, x + s / 2.0, y);
        cairo_line_to(cr, x + s, y + s);
        cairo_line_to(cr, x, y + s);
        cairo_close_path(cr);
    } else if (shape == 2) { /* DOWN triangle (discharging) */
        cairo_new_sub_path(cr);
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, x + s, y);
        cairo_line_to(cr, x + s / 2.0, y + s);
        cairo_close_path(cr);
    } else if (shape == 3) { /* CIRCLE (idle / AC full) */
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + s / 2.0, y + s / 2.0, s / 2.0, 0, 2 * G_PI);
    } else { /* SQUARE */
        rrect(cr, x, y, s, s, radius);
    }
}

/* The battery indicator: triangle up (charging), triangle down (discharging), circle (idle)
 * or square. Filled from bottom up to bat_cap %. Colour comes from CSS `color`. */
static gboolean bat_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    (void)d;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double s = MIN(W, H), x = (W - s) / 2, y = (H - s) / 2;
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA c;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &c);

    int shape = 0;
    if (!bat_both && bat_arrow) {
        if (bat_charging) shape = 1;
        else if (bat_discharging) shape = 2;
        else shape = 3;
    }

    draw_shape_path(cr, shape, x, y, s);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha * track_alpha);
    cairo_fill(cr);

    double fh = s * bat_cap / 100.0;
    if (fh > 0.5) {
        cairo_save(cr);
        cairo_rectangle(cr, x - 1, y + s - fh, s + 2, fh + 1);
        cairo_clip(cr);
        draw_shape_path(cr, shape, x, y, s);
        cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha);
        cairo_fill(cr);
        cairo_restore(cr);
    }
    return TRUE;
}

static void update_clock(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char b[16];
    strftime(b, sizeof b, "%H:%M", &tm);
    if (strcmp(b, gtk_label_get_text(GTK_LABEL(time_l)))) gtk_label_set_text(GTK_LABEL(time_l), b);
}

static gboolean clock_tick(gpointer d) {
    (void)d;
    update_clock();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);       /* sleep until just after the next minute starts */
    long ms = (60 - ts.tv_sec % 60) * 1000L - ts.tv_nsec / 1000000L + 30;
    g_timeout_add(ms, clock_tick, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean sensor_tick(gpointer d) {
    (void)d;
    if (show_temp) {
        int t = temp_read();
        if (t < 0) {
            gtk_widget_set_visible(temp_l, FALSE);
        } else {
            char b[16];
            snprintf(b, sizeof b, "%d\xc2\xb0", t);
            if (strcmp(b, gtk_label_get_text(GTK_LABEL(temp_l)))) gtk_label_set_text(GTK_LABEL(temp_l), b);
            gtk_widget_set_visible(temp_l, TRUE);
            set_temp_class(t >= temp_hot ? "hot" : t >= temp_warm ? "warm" : NULL);
        }
    }

    BatInfo b;
    bat_read(&b);
    gtk_widget_set_visible(bat_da, b.present && (bat_square || bat_arrow));
    if (!b.present) {
        gtk_widget_set_visible(arrow_l, FALSE);
        gtk_widget_set_visible(watts_l, FALSE);
        return G_SOURCE_CONTINUE;
    }
    bat_charging = b.charging;
    bat_discharging = b.discharging;
    bat_cap = b.cap;

    const char *cls = b.cap <= bat_low ? "low" : b.cap <= bat_mid ? "mid" : "ok";
    set_state_class(bat_da, cls);
    set_state_class(arrow_l, cls);
    set_state_class(watts_l, cls);
    gtk_widget_queue_draw(bat_da);

    /* direction of power flow: in arrow mode, bat_da IS the triangle.
     * in bat_both mode, arrow_l is a separate label */
    if (bat_both) {
        const char *arrow = b.charging ? "\xe2\x96\xb2" : b.discharging ? "\xe2\x96\xbc" : "\xe2\x97\x8f";
        if (strcmp(arrow, gtk_label_get_text(GTK_LABEL(arrow_l)))) gtk_label_set_text(GTK_LABEL(arrow_l), arrow);
        gtk_widget_set_visible(arrow_l, TRUE);
    } else {
        gtk_widget_set_visible(arrow_l, FALSE);
    }

    char w[24];
    snprintf(w, sizeof w, "%.1fW", b.watts);
    if (strcmp(w, gtk_label_get_text(GTK_LABEL(watts_l)))) gtk_label_set_text(GTK_LABEL(watts_l), w);
    gtk_widget_set_visible(watts_l, show_watts && (b.charging || b.discharging));

    char tip[96];
    if (b.charging)
        snprintf(tip, sizeof tip, "Battery: %d%% charging (%.1fW%s%s)", b.cap, b.watts, b.rem[0] ? ", " : "", b.rem);
    else if (b.discharging)
        snprintf(tip, sizeof tip, "Battery: %d%% discharging (%.1fW%s%s)", b.cap, b.watts, b.rem[0] ? ", " : "", b.rem);
    else
        snprintf(tip, sizeof tip, "Battery: %d%% (idle/full)", b.cap);
    gtk_widget_set_tooltip_text(bat_da, tip);

    return G_SOURCE_CONTINUE;
}

/* ---- position: top-right (home) or top-centre (dodged), plus OLED shift ---- */
static void apply_margins(void) {
    static const int ox[4] = { 0, 1, 1, 0 }, oy[4] = { 0, 0, 1, 1 };
    int sx = ox[shift_ix] * shift_px, sy = oy[shift_ix] * shift_px;
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, m_top + sy);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, dodged ? 0 : m_right + sx);
}

static gboolean shift_tick(gpointer d) {
    (void)d;
    shift_ix = (shift_ix + 1) % 4;
    apply_margins();
    return G_SOURCE_CONTINUE;
}

static void set_input(gboolean on) {
    if (on) {
        gtk_widget_input_shape_combine_region(win, NULL);
    } else {
        cairo_region_t *empty = cairo_region_create();
        gtk_widget_input_shape_combine_region(win, empty);
        cairo_region_destroy(empty);
    }
}

static void home_rect(GdkRectangle *r) {
    GdkDisplay *d = gdk_display_get_default();
    GdkWindow *gw = gtk_widget_get_window(win);
    GdkMonitor *m = gw ? gdk_display_get_monitor_at_window(d, gw) : NULL;
    if (!m) m = gdk_display_get_monitor(d, 0);
    GdkRectangle g = { 0, 0, 0, 0 };
    if (m) gdk_monitor_get_geometry(m, &g);
    int w = gtk_widget_get_allocated_width(win), h = gtk_widget_get_allocated_height(win);
    r->x = g.x + g.width - m_right - shift_px - w;
    r->y = g.y + m_top;
    r->width = w + shift_px;
    r->height = h + shift_px;
}

static gboolean dodge_check(gpointer d) {
    (void)d;
    double x, y;
    if (hypr_cursor(&x, &y)) {
        GdkRectangle r;
        home_rect(&r);
        const int pad = 12;
        if (x >= r.x - pad && x <= r.x + r.width + pad && y >= r.y - pad && y <= r.y + r.height + pad)
            return G_SOURCE_CONTINUE;          /* pointer is still parked there: stay away */
    }
    dodge_id = 0;
    dodged = FALSE;
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    apply_margins();
    set_input(TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean on_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d) {
    (void)w; (void)e; (void)d;
    if (!dodge_on || dodged) return FALSE;
    dodged = TRUE;
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, FALSE);   /* only TOP => centred */
    apply_margins();
    set_input(FALSE);                       /* while away it never blocks clicks */
    dodge_id = g_timeout_add(dodge_ms, dodge_check, NULL);
    return FALSE;
}

static gboolean quit_cb(gpointer d) {
    (void)d;
    dash_stop();
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static gboolean hist_timer_cb(gpointer d) {
    (void)d;
    history_tick();
    return G_SOURCE_CONTINUE;
}

int indicator_run(void) {
    m_top = cfg_int("indicator", "margin_top", 6);
    m_right = cfg_int("indicator", "margin_right", 10);
    int interval = MAX(1, cfg_int("indicator", "interval", 5));
    int shift_every = cfg_int("indicator", "shift_every", 300);
    shift_px = cfg_int("indicator", "shift_px", 2);
    dodge_on = cfg_bool("indicator", "dodge", TRUE);
    dodge_ms = MAX(200, cfg_int("indicator", "dodge_ms", 1200));
    show_temp = cfg_bool("indicator", "show_temp", TRUE);
    show_watts = cfg_bool("indicator", "show_watts", TRUE);
    {   /* battery_style: arrow (default: up/down triangle colored by charge level) | square | both */
        char *bs = cfg_str("indicator", "battery_style", "arrow");
        bat_both = !g_ascii_strcasecmp(bs, "both");
        bat_square = !g_ascii_strcasecmp(bs, "square") || bat_both;
        bat_arrow = g_ascii_strcasecmp(bs, "square") != 0;
        g_free(bs);
    }
    hide_on_empty = cfg_bool("indicator", "hide_on_empty", FALSE);
    bat_mid = cfg_int("indicator", "battery_mid", 50);
    bat_low = cfg_int("indicator", "battery_low", 20);
    temp_warm = cfg_int("indicator", "temp_warm", 65);
    temp_hot = cfg_int("indicator", "temp_hot", 85);
    track_alpha = cfg_dbl("indicator", "track_alpha", 0.25);
    radius = cfg_int("indicator", "radius", 0);

    /* Background transparency / color from config */
    {
        char *bg = cfg_str("indicator", "bg", NULL);
        if (!bg) bg = cfg_str("indicator", "background", NULL);
        char css_str[256];
        if (bg && *bg) {
            snprintf(css_str, sizeof css_str, "window#indicator { background-color: %s; }", bg);
        } else {
            double alpha = cfg_dbl("indicator", "bg_alpha", 0.0);
            snprintf(css_str, sizeof css_str, "window#indicator { background-color: rgba(22, 22, 29, %.2f); }", alpha);
        }
        g_free(bg);
        GtkCssProvider *bp = gtk_css_provider_new();
        gtk_css_provider_load_from_data(bp, css_str, -1, NULL);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(bp),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    self_exe = g_file_read_link("/proc/self/exe", NULL);
    if (!self_exe) self_exe = g_strdup("panelless");

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(win, "indicator");
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_namespace(GTK_WINDOW(win), "panelless-indicator");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    apply_margins();

    GdkVisual *vis = gdk_screen_get_rgba_visual(gtk_widget_get_screen(win));
    if (vis) gtk_widget_set_visual(win, vis);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    time_l = gtk_label_new("--:--");   gtk_widget_set_name(time_l, "time");
    temp_l = gtk_label_new("");        gtk_widget_set_name(temp_l, "temp");
    /* a (childless) GtkBox, not a drawing area: boxes honour CSS min-width/min-height,
     * so the square's size is editable in style.css. We paint the square ourselves. */
    bat_da = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);   gtk_widget_set_name(bat_da, "battery");
    arrow_l = gtk_label_new("");       gtk_widget_set_name(arrow_l, "arrow");
    watts_l = gtk_label_new("");       gtk_widget_set_name(watts_l, "watts");
    gtk_widget_set_valign(bat_da, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all(temp_l, TRUE);
    gtk_widget_set_no_show_all(bat_da, TRUE);
    gtk_widget_set_no_show_all(arrow_l, TRUE);
    gtk_widget_set_no_show_all(watts_l, TRUE);
    g_signal_connect_after(bat_da, "draw", G_CALLBACK(bat_draw), NULL);
    gtk_box_pack_start(GTK_BOX(row), time_l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), temp_l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), bat_da, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), arrow_l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), watts_l, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(win), row);

    gtk_widget_add_events(win, GDK_ENTER_NOTIFY_MASK);
    g_signal_connect(win, "enter-notify-event", G_CALLBACK(on_enter), NULL);

    gtk_widget_show_all(win);
    gtk_widget_set_visible(time_l, TRUE);
    update_clock();
    sensor_tick(NULL);
    clock_tick(NULL);
    history_tick();
    g_timeout_add_seconds(1, hist_timer_cb, NULL);
    g_timeout_add_seconds(interval, sensor_tick, NULL);
    if (shift_every > 0 && shift_px > 0) g_timeout_add_seconds(shift_every, shift_tick, NULL);

    /* the tray watcher must live in the always-on process so tray apps keep a stable owner */
    tray_init(NULL, 0, NULL);

    if (cfg_bool("dashboard", "auto", TRUE)) hypr_watch_start();

    g_unix_signal_add(SIGTERM, quit_cb, NULL);
    g_unix_signal_add(SIGINT, quit_cb, NULL);
    gtk_main();
    dash_stop();
    return 0;
}
