/* common.c - config file, CSS loading, sensor readers */
#include "barless.h"
#include <glib/gstdio.h>
#include <glob.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ================================================================== */
/* Default files (written on first run, never overwritten)             */
/* ================================================================== */
static const char DEFAULT_CONFIG[] =
"# barless config (key-file format). Restart barless to apply changes.\n"
"# Fonts, colours and sizes live in style.css, not here.\n"
"\n"
"[indicator]\n"
"# distance from the top-right corner, px\n"
"margin_top=6\n"
"margin_right=10\n"
"# seconds between battery / temperature refreshes\n"
"interval=5\n"
"# OLED care: nudge the indicator by shift_px every shift_every seconds (0 = off)\n"
"shift_every=300\n"
"shift_px=2\n"
"# hover dodge: jump to top-centre when the pointer touches it. dodge_ms = how long\n"
"# before it checks whether the pointer left and comes back\n"
"dodge=true\n"
"dodge_ms=1200\n"
"show_temp=true\n"
"show_watts=true\n"
"# battery look: arrow (compact: the up/down triangle is filled & coloured by charge level; idle/full shows a dot)\n"
"#               | square (the little square only) | both\n"
"battery_style=arrow\n"
"# battery states: .ok above battery_mid, .mid up to it, .low at/below battery_low\n"
"battery_mid=50\n"
"battery_low=20\n"
"# battery state colors (optional hex or CSS color overrides)\n"
"# battery_color_ok=#4f7a52\n"
"# battery_color_mid=#8f7a3a\n"
"# battery_color_low=#a55555\n"
"# temperature states: .warm and .hot on #temp\n"
"temp_warm=65\n"
"temp_hot=85\n"
"# unfilled part of the battery shape, 0..1\n"
"track_alpha=0.25\n"
"radius=0\n"
"# optional: force one sensor file instead of auto-detect (see: barless --info)\n"
"# temp_path=/sys/devices/platform/coretemp.0/hwmon/hwmon5/temp1_input\n"
"# hide indicator when active workspace has no windows and dashboard opens\n"
"hide_on_empty=false\n"
"# background color / transparency (e.g. transparent, #16161d, @background, @foreground, or rgba(22, 22, 29, 0.8))\n"
"bg=transparent\n"
"# or set background alpha: 0.0 (transparent) to 1.0 (opaque) (uses theme @background by default)\n"
"# bg_alpha=0.0\n"
"\n"
"[dashboard]\n"
"# open automatically on a workspace with no windows (needs Hyprland)\n"
"auto=true\n"
"width=380\n"
"# gap to the screen edges, px (margin_top is bigger so it clears the indicator)\n"
"margin=12\n"
"margin_top=40\n"
"columns=3\n"
"# seconds between stat refreshes (only runs while the dashboard is open)\n"
"interval=5\n"
"tray_icon=18\n"
"# corner radius (px) of cards and buttons. 0 = square. Screens differ, so set what suits yours\n"
"radius=0\n"
"# text placement: left | center | right\n"
"clock_align=left\n"
"date_align=left\n"
"key_align=left\n"
"value_align=right\n"
"button_align=center\n"
"tray_align=center\n"
"# show tray items as buttons in the button grid (placed first before user buttons)\n"
"tray_buttons=true\n"
"# middle-click-drag the panel to move it; the spot is remembered in position.ini\n"
"# (delete that file to go back to the default corner). Set false to disable.\n"
"movable=true\n"
"# which stats to show, in this order. Remove a name to hide it: cpu, temp, net, battery\n"
"stats=cpu,temp,net,battery\n"
"# line charts under stats (one sample per second, preserved across panel restarts)\n"
"net_chart=true\n"
"cpu_chart=false\n"
"temp_chart=false\n"
"battery_chart=false\n"
"# seconds of history to show in charts (net_chart_seconds, etc. can override per-chart)\n"
"chart_seconds=30\n"
"net_chart_seconds=30\n"
"chart_height=36\n"
"# background color / transparency of the dashboard window\n"
"bg=transparent\n"
"# background transparency of cards (0.0 to 1.0) (uses theme @background)\n"
"card_alpha=0.65\n"
"# buttons: false = icon only (tooltip still shows the label)\n"
"show_labels=true\n"
"# used by buttons with terminal=true\n"
"terminal=xdg-terminal-exec\n"
"\n"
"# Buttons: one [button:name] group each, shown in this order. Add / delete / reorder freely.\n"
"#   icon     a Nerd Font glyph (browse them at nerdfonts.com/cheat-sheet)\n"
"#   label    text next to the icon (optional if there is an icon)\n"
"#   cmd      what to run. Split like a shell would, but NOT run through one: use  sh -c '...'  for pipes\n"
"#   terminal true = run it inside a terminal (for TUIs such as btop, nmtui, any omarchy tui)\n"
"#   hide     true = keep the group but do not show it\n"
"#   tooltip  hover text (optional)\n"
"[button:menu]\n"
"icon=\xf3\xb0\x85\x80\n"          /* U+F0140 */
"label=Menu\n"
"cmd=omarchy-menu-alternative\n"
"tooltip=Launcher\n"
"\n"
"[button:wifi]\n"
"icon=\xf3\xb0\xa4\xa8\n"          /* U+F0928 */
"label=Wifi\n"
"cmd=omarchy-launch-wifi\n"
"\n"
"[button:bluetooth]\n"
"icon=\xf3\xb0\x82\xaf\n"          /* U+F00AF */
"label=Bluetooth\n"
"cmd=omarchy-launch-bluetooth\n"
"\n"
"[button:lock]\n"
"icon=\xef\x80\xa3\n"              /* U+F023 */
"label=Lock\n"
"cmd=hyprlock\n"
"\n"
"[button:sleep]\n"
"icon=\xf3\xb0\xa4\x84\n"          /* U+F0904 */
"label=Sleep\n"
"cmd=systemctl suspend\n"
"\n"
"[button:restart]\n"
"icon=\xee\xab\x92\n"              /* U+EAD2 */
"label=Restart\n"
"cmd=systemctl reboot\n"
"\n"
"[button:shutdown]\n"
"icon=\xef\x80\x91\n"              /* U+F011 */
"label=Shutdown\n"
"cmd=systemctl poweroff\n"
"\n"
"# Example: a terminal app (remove the leading # to enable)\n"
"#[button:btop]\n"
"#label=btop\n"
"#cmd=btop\n"
"#terminal=true\n";

static const char DEFAULT_CSS[] =
"/* barless style.css - edit freely, it reloads when you save (and on Omarchy theme change). */\n"
"\n"
"/* Same colour file your waybar imports, so @foreground / @background follow the theme.\n"
"   The two lines before it are only a fallback if that file is missing. */\n"
"@define-color foreground #dcd7ba;\n"
"@define-color background #16161d;\n"
"@import \"../omarchy/current/theme/waybar.css\";\n"
"\n"
"* {\n"
"  font-family: \"GeistMono Nerd Font\", monospace;\n"
"}\n"
"\n"
"/* ============ always-on indicator ============\n"
"   Transparent window = those pixels stay off on an OLED. Keep colours dim. */\n"
"window#indicator { background-color: transparent; }\n"
"#indicator box   { padding: 2px 6px; }\n"
"#indicator label { color: alpha(@foreground, 0.55); margin: 0 5px; }\n"
"#indicator #time { color: alpha(@foreground, 0.65); }\n"
"\n"
"/* the battery square: its colour is the state colour; size = min-width/min-height */\n"
"#battery, #battery.ok                  { min-width: 11px; min-height: 11px; color: #4f7a52; }\n"
"#battery.mid                           { color: #8f7a3a; }\n"
"#battery.low                           { color: #a55555; }\n"
"/* the arrow indicator glyph: */\n"
"#arrow, #indicator #arrow, #arrow.ok, #indicator #arrow.ok { color: #4f7a52; }\n"
"#arrow.mid, #indicator #arrow.mid                         { color: #8f7a3a; }\n"
"#arrow.low, #indicator #arrow.low                         { color: #a55555; }\n"
"#watts.mid, #indicator #watts.mid                         { color: #8f7a3a; }\n"
"#watts.low, #indicator #watts.low                         { color: #a55555; }\n"
"\n"
"#temp.warm { color: #8f7a3a; }\n"
"#temp.hot  { color: #a55555; }\n"
"\n"
"/* ============ dashboard (empty workspace) ============ */\n"
"window#dashboard { background-color: transparent; }\n"
"#panel { padding: 0; }\n"
"\n"
".card {\n"
"  background-color: alpha(@background, 0.65);\n"
"  padding: 14px 16px;\n"
"  margin-bottom: 10px;\n"
"}\n"
"/* corner radius: [dashboard] radius in config.ini (add your own border-radius here to override).\n"
"   Bold / italic work on any element, e.g.  #bigclock { font-weight: bold; }  #date { font-style: italic; } */\n"
"#netchart, #cpuchart, #tempchart, #batterychart, .chart { color: alpha(@foreground, 0.8); }   /* chart line colour (upload is drawn dimmer) */\n"
"#bigclock { font-size: 46px; color: alpha(@foreground, 0.9); }\n"
"#date     { font-size: 15px; color: alpha(@foreground, 0.55); }\n"
".stat-key { color: alpha(@foreground, 0.45); }\n"
".stat-val { color: @foreground; }\n"
"\n"
"button.action {\n"
"  background-image: none;\n"
"  box-shadow: none;\n"
"  text-shadow: none;\n"
"  border: none;\n"
"  padding: 12px 8px;\n"
"  color: @foreground;\n"
"  background-color: alpha(@foreground, 0.06);\n"
"  transition: background-color 150ms ease, color 150ms ease;\n"
"}\n"
"button.action:hover {\n"
"  background-color: @foreground;\n"
"  color: @background;\n"
"}\n";

/* ================================================================== */
/* Paths                                                               */
/* ================================================================== */
const char *cfg_dir(void) {
    static char *d = NULL;
    if (!d) {
        char *bdir = g_build_filename(g_get_user_config_dir(), "barless", NULL);
        char *pdir = g_build_filename(g_get_user_config_dir(), "panelless", NULL);
        if (g_file_test(bdir, G_FILE_TEST_IS_DIR) || !g_file_test(pdir, G_FILE_TEST_IS_DIR)) {
            d = bdir;
            g_free(pdir);
        } else {
            d = pdir;
            g_free(bdir);
        }
    }
    return d;
}

const char *sysfs_root(void) {
    const char *r = g_getenv("BARLESS_SYSFS");
    if (!r || !*r) r = g_getenv("PANELLESS_SYSFS");
    return (r && *r) ? r : "/sys";
}

static void ensure_file(const char *name, const char *content) {
    char *p = g_build_filename(cfg_dir(), name, NULL);
    if (!g_file_test(p, G_FILE_TEST_EXISTS)) {
        g_mkdir_with_parents(cfg_dir(), 0755);
        g_file_set_contents(p, content, -1, NULL);
    }
    g_free(p);
}

/* ================================================================== */
/* Config (GKeyFile: part of GLib, no extra dependency)                */
/* ================================================================== */
static GKeyFile *kf = NULL;

void cfg_load(void) {
    ensure_file("config.ini", DEFAULT_CONFIG);
    ensure_file("style.css", DEFAULT_CSS);
    kf = g_key_file_new();
    char *p = g_build_filename(cfg_dir(), "config.ini", NULL);
    GError *e = NULL;
    if (!g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, &e)) {
        g_printerr("barless: %s: %s (using defaults)\n", p, e->message);
        g_error_free(e);
    }
    g_free(p);
}

int cfg_int(const char *g, const char *k, int def) {
    if (!kf || !g_key_file_has_key(kf, g, k, NULL)) return def;
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, g, k, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

double cfg_dbl(const char *g, const char *k, double def) {
    if (!kf || !g_key_file_has_key(kf, g, k, NULL)) return def;
    GError *e = NULL;
    double v = g_key_file_get_double(kf, g, k, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

gboolean cfg_bool(const char *g, const char *k, gboolean def) {
    if (!kf || !g_key_file_has_key(kf, g, k, NULL)) return def;
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(kf, g, k, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

char *cfg_str(const char *g, const char *k, const char *def) {
    char *v = kf ? g_key_file_get_string(kf, g, k, NULL) : NULL;
    if (v) return v;
    return def ? g_strdup(def) : NULL;
}

char **cfg_groups(void) {
    return kf ? g_key_file_get_groups(kf, NULL) : g_new0(char *, 1);
}

/* ================================================================== */
/* CSS: user file at USER priority, hot-reloaded                       */
/* ================================================================== */
static GtkCssProvider *css = NULL;
static guint css_reload_id = 0;

static void css_load(void) {
    char *p = g_build_filename(cfg_dir(), "style.css", NULL);
    GError *e = NULL;
    if (!gtk_css_provider_load_from_path(css, p, &e)) {
        g_printerr("barless: %s: %s (using built-in style)\n", p, e->message);
        g_error_free(e);
        gtk_css_provider_load_from_data(css, DEFAULT_CSS, -1, NULL);
    }
    g_free(p);
}

static gboolean css_reload_cb(gpointer d) {
    (void)d;
    css_reload_id = 0;
    css_load();
    return G_SOURCE_REMOVE;
}

static void css_changed(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer d) {
    (void)m; (void)other; (void)ev;
    /* the user's style.css, or anything directly under omarchy/current (theme swap) */
    if (d) {
        char *b = g_file_get_basename(f);
        gboolean ours = !g_strcmp0(b, "style.css");
        g_free(b);
        if (!ours) return;
    }
    if (!css_reload_id) css_reload_id = g_timeout_add(250, css_reload_cb, NULL);
}

static void css_watch(const char *dir, gboolean only_style) {
    GFile *f = g_file_new_for_path(dir);
    GFileMonitor *m = g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (m) g_signal_connect(m, "changed", G_CALLBACK(css_changed), only_style ? GINT_TO_POINTER(1) : NULL);
    g_object_unref(f);
}

void css_init(void) {
    css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER);
    css_load();
    css_watch(cfg_dir(), TRUE);
    char *cur = g_build_filename(g_get_user_config_dir(), "omarchy", "current", NULL);
    css_watch(cur, FALSE);
    g_free(cur);
}

/* ================================================================== */
/* Small readers                                                       */
/* ================================================================== */
static long read_long(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void read_str(const char *path, char *buf, size_t len) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(buf, len, f)) buf[strcspn(buf, "\n")] = '\0';
    fclose(f);
}

/* ================================================================== */
/* Battery                                                             */
/* ================================================================== */
void bat_read(BatInfo *b) {
    memset(b, 0, sizeof(*b));

    /* test hook: BARLESS_FAKE_BAT="18 discharging 7.5"  (preview CSS states) */
    const char *fake = g_getenv("BARLESS_FAKE_BAT");
    if (!fake || !*fake) fake = g_getenv("PANELLESS_FAKE_BAT");
    if (fake && *fake) {
        char st[32] = "discharging";
        int cap = 50;
        double w = 5.0;
        sscanf(fake, "%d %31s %lf", &cap, st, &w);
        b->present = TRUE;
        b->cap = CLAMP(cap, 0, 100);
        b->charging = !g_ascii_strcasecmp(st, "charging");
        b->discharging = !g_ascii_strcasecmp(st, "discharging");
        b->watts = w;
        return;
    }

    char pat[256];
    snprintf(pat, sizeof pat, "%s/class/power_supply/BAT*", sysfs_root());
    glob_t g;
    if (glob(pat, 0, NULL, &g) != 0 || g.gl_pathc == 0) { globfree(&g); return; }
    const char *bp = g.gl_pathv[0];
    char path[320], status[32];
    b->present = TRUE;

    snprintf(path, sizeof path, "%s/capacity", bp);
    long cap = read_long(path);
    b->cap = cap < 0 ? 0 : (cap > 100 ? 100 : (int)cap);

    snprintf(path, sizeof path, "%s/status", bp);
    read_str(path, status, sizeof status);
    b->charging = !strcmp(status, "Charging");
    b->discharging = !strcmp(status, "Discharging");

    snprintf(path, sizeof path, "%s/power_now", bp);
    long p_now = read_long(path);
    snprintf(path, sizeof path, "%s/energy_now", bp);
    long e_now = read_long(path);
    snprintf(path, sizeof path, "%s/energy_full", bp);
    long e_full = read_long(path);

    double watts = 0, energy = 0, full = 0;
    if (p_now >= 0 && e_now >= 0) {
        watts = p_now / 1e6;
        energy = e_now / 1e6;
        full = e_full >= 0 ? e_full / 1e6 : energy;
    } else {
        snprintf(path, sizeof path, "%s/current_now", bp);
        long c = read_long(path);
        snprintf(path, sizeof path, "%s/voltage_now", bp);
        long v = read_long(path);
        snprintf(path, sizeof path, "%s/charge_now", bp);
        long q = read_long(path);
        snprintf(path, sizeof path, "%s/charge_full", bp);
        long qf = read_long(path);
        if (c > 0 && v > 0) {
            double volts = v / 1e6;
            watts = (c / 1e6) * volts;
            energy = (q > 0 ? q / 1e6 : 0) * volts;
            full = (qf > 0 ? qf / 1e6 : 0) * volts;
        }
    }
    globfree(&g);

    b->watts = watts;
    if (watts > 0.1) {
        double hrs = b->discharging ? energy / watts : (full - energy) / watts;
        if (hrs < 0) hrs = 0;
        if (hrs > 99) hrs = 99;
        int h = (int)hrs, m = (int)((hrs - h) * 60);
        snprintf(b->rem, sizeof b->rem, "%d:%02d", h, m);
    }
}

/* ================================================================== */
/* CPU temperature: find the package sensor instead of hard-coding     */
/* hwmonN (that number changes between boots)                          */
/* ================================================================== */
static char temp_file[512];
static gboolean temp_tried = FALSE;

static void temp_discover(void) {
    temp_tried = TRUE;
    temp_file[0] = '\0';

    char *forced = cfg_str("indicator", "temp_path", NULL);
    if (forced && *forced && g_file_test(forced, G_FILE_TEST_EXISTS)) {
        snprintf(temp_file, sizeof temp_file, "%s", forced);
        g_free(forced);
        return;
    }
    g_free(forced);

    static const char *const names[] = { "coretemp", "k10temp", "zenpower", "cpu_thermal", NULL };
    static const char *const labels[] = { "Package id 0", "Tdie", "Tctl", "CPU", NULL };
    int best = 99;
    char pat[256];
    snprintf(pat, sizeof pat, "%s/class/hwmon/hwmon*", sysfs_root());
    glob_t g;
    if (glob(pat, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char path[400], name[64];
            snprintf(path, sizeof path, "%s/name", g.gl_pathv[i]);
            read_str(path, name, sizeof name);
            int pri = -1;
            for (int n = 0; names[n]; n++) if (!strcmp(name, names[n])) pri = n;
            if (pri < 0 || pri >= best) continue;

            /* prefer the package/die label, else temp1 */
            char chosen[400] = "";
            for (int l = 0; labels[l] && !chosen[0]; l++)
                for (int t = 1; t <= 16; t++) {
                    char lp[400], lab[64];
                    snprintf(lp, sizeof lp, "%s/temp%d_label", g.gl_pathv[i], t);
                    read_str(lp, lab, sizeof lab);
                    if (!strcmp(lab, labels[l])) {
                        snprintf(chosen, sizeof chosen, "%s/temp%d_input", g.gl_pathv[i], t);
                        break;
                    }
                }
            if (!chosen[0]) snprintf(chosen, sizeof chosen, "%s/temp1_input", g.gl_pathv[i]);
            if (read_long(chosen) >= 0) {
                snprintf(temp_file, sizeof temp_file, "%s", chosen);
                best = pri;
            }
        }
    }
    globfree(&g);
    if (temp_file[0]) return;

    /* fallback: thermal zones (x86_pkg_temp, then anything) */
    snprintf(pat, sizeof pat, "%s/class/thermal/thermal_zone*", sysfs_root());
    if (glob(pat, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char path[400], type[64];
            snprintf(path, sizeof path, "%s/type", g.gl_pathv[i]);
            read_str(path, type, sizeof type);
            snprintf(path, sizeof path, "%s/temp", g.gl_pathv[i]);
            if (read_long(path) < 0) continue;
            if (!strcmp(type, "x86_pkg_temp")) { snprintf(temp_file, sizeof temp_file, "%s", path); break; }
            if (!temp_file[0]) snprintf(temp_file, sizeof temp_file, "%s", path);
        }
    }
    globfree(&g);
}

const char *temp_sensor_path(void) {
    if (!temp_tried) temp_discover();
    return temp_file;
}

int temp_read(void) {
    if (!temp_tried) temp_discover();
    if (!temp_file[0]) return -1;
    long v = read_long(temp_file);
    if (v < 0) { temp_tried = FALSE; return -1; }    /* sensor vanished (resume?): rediscover next time */
    int c = (int)(v / 1000);
    return c > 99 ? 99 : c;
}

/* ================================================================== */
/* CPU usage and network speed (deltas since the previous call)        */
/* ================================================================== */
int cpu_read(void) {
    static unsigned long long pt = 0, pi = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    unsigned long long u = 0, n = 0, s = 0, i = 0, w = 0, irq = 0, sirq = 0, st = 0;
    int c = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &w, &irq, &sirq, &st);
    fclose(f);
    if (c < 4) return 0;
    unsigned long long total = u + n + s + i + w + irq + sirq + st, idle = i + w;
    unsigned long long dt = total - pt, di = idle - pi;
    pt = total; pi = idle;
    if (dt == 0 || di > dt) return 0;
    int pct = (int)(100.0 * (double)(dt - di) / (double)dt + 0.5);
    return pct > 99 ? 99 : pct;
}

static void net_bytes(unsigned long long *rx, unsigned long long *tx) {
    *rx = 0; *tx = 0;
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[512], iface[64];
    unsigned long long r, t, d;
    if (!fgets(line, sizeof line, f) || !fgets(line, sizeof line, f)) { fclose(f); return; }
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " %63[^:]: %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   iface, &r, &d, &d, &d, &d, &d, &d, &d, &t) == 10) {
            if (!strcmp(iface, "lo") || !strncmp(iface, "docker", 6) || !strncmp(iface, "veth", 4) ||
                !strncmp(iface, "br-", 3) || !strncmp(iface, "virbr", 5)) continue;
            *rx += r; *tx += t;
        }
    }
    fclose(f);
}

void net_read(double *down, double *up) {
    static unsigned long long prx = 0, ptx = 0;
    static gint64 pt = 0;
    unsigned long long rx, tx;
    net_bytes(&rx, &tx);
    gint64 now = g_get_monotonic_time();
    double dt = pt ? (now - pt) / 1e6 : 0;
    *down = (dt > 0.01 && rx >= prx) ? (rx - prx) / dt : 0;     /* counters can shrink when a link vanishes */
    *up   = (dt > 0.01 && tx >= ptx) ? (tx - ptx) / dt : 0;
    prx = rx; ptx = tx; pt = now;
}

/* Fixed 8 chars: 4-char number (max 3 digits) + 4-char unit. */
void fmt_speed(double bps, char *out, size_t len) {
    static const char *const units[] = { " B/s", "KB/s", "MB/s", "GB/s" };
    int u = 0;
    double v = bps;
    while (v >= 999.5 && u < 3) { v /= 1024.0; u++; }
    char num[16];
    if (u == 0)         snprintf(num, sizeof num, "%.0f", v);
    else if (v < 9.995) snprintf(num, sizeof num, "%.2f", v);
    else if (v < 99.95) snprintf(num, sizeof num, "%.1f", v);
    else                snprintf(num, sizeof num, "%.0f", v);
    snprintf(out, len, "%4s%s", num, units[u]);
}

/* ================================================================== */
/* History ring buffer across panel restarts (shared memory tmpfs)     */
/* ================================================================== */
#define HIST_MAGIC 0x50414E4C /* "PANL" */
#define HIST_VERSION 1

typedef struct {
    gint64 timestamp;
    double net_down;
    double net_up;
    double cpu;
    double temp;
    double bat_cap;
    double bat_watts;
} HistSample;

typedef struct {
    guint32 magic;
    guint32 version;
    guint32 count;
    gint64  last_time;
    pid_t   last_writer_pid;
    HistSample samples[HIST_CAPACITY];
} HistData;

static HistData *hist = NULL;

void history_init(void) {
    if (hist) return;
    const char *runtimedir = g_get_user_runtime_dir();
    char *dir = g_build_filename(runtimedir ? runtimedir : "/tmp", "barless", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "history.bin", NULL);
    g_free(dir);

    int fd = open(path, O_RDWR | O_CREAT, 0600);
    g_free(path);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size < sizeof(HistData)) {
        if (ftruncate(fd, sizeof(HistData)) != 0) {
            close(fd);
            return;
        }
    }

    void *mem = mmap(NULL, sizeof(HistData), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mem == MAP_FAILED) return;

    hist = (HistData *)mem;
    if (hist->magic != HIST_MAGIC || hist->version != HIST_VERSION) {
        memset(hist, 0, sizeof(HistData));
        hist->magic = HIST_MAGIC;
        hist->version = HIST_VERSION;
    }
}

void history_tick(void) {
    history_init();
    if (!hist) return;

    gint64 now = g_get_monotonic_time();
    /* If another process recorded within the last 850ms, avoid double sampling */
    if (hist->count > 0 && (now - hist->last_time) < 850000 && hist->last_writer_pid != getpid()) {
        return;
    }

    double down = 0, up = 0;
    net_read(&down, &up);
    double cpu = (double)cpu_read();
    double temp = (double)temp_read();
    BatInfo b;
    bat_read(&b);

    guint32 idx = hist->count % HIST_CAPACITY;
    hist->samples[idx].timestamp = now;
    hist->samples[idx].net_down = down;
    hist->samples[idx].net_up = up;
    hist->samples[idx].cpu = cpu >= 0 ? cpu : 0;
    hist->samples[idx].temp = temp >= 0 ? temp : 0;
    hist->samples[idx].bat_cap = b.present ? (double)b.cap : 0;
    hist->samples[idx].bat_watts = b.present ? b.watts : 0;

    hist->last_time = now;
    hist->last_writer_pid = getpid();
    hist->count++;
}

int history_get(HistMetric metric, double *p1, double *p2, int max_samples) {
    history_init();
    if (!hist || hist->count == 0 || max_samples <= 0) return 0;

    guint32 total = hist->count;
    int available = (total < HIST_CAPACITY) ? (int)total : HIST_CAPACITY;
    int n = (available < max_samples) ? available : max_samples;

    for (int i = 0; i < n; i++) {
        guint32 idx = (total - n + i) % HIST_CAPACITY;
        HistSample *s = &hist->samples[idx];
        switch (metric) {
            case HIST_METRIC_NET:
                if (p1) p1[i] = s->net_down;
                if (p2) p2[i] = s->net_up;
                break;
            case HIST_METRIC_CPU:
                if (p1) p1[i] = s->cpu;
                if (p2) p2[i] = 0;
                break;
            case HIST_METRIC_TEMP:
                if (p1) p1[i] = s->temp;
                if (p2) p2[i] = 0;
                break;
            case HIST_METRIC_BATTERY:
                if (p1) p1[i] = s->bat_cap;
                if (p2) p2[i] = s->bat_watts;
                break;
        }
    }
    return n;
}

gboolean history_latest(double *cpu, double *temp, double *net_down, double *net_up, BatInfo *bat) {
    history_init();
    if (!hist || hist->count == 0) return FALSE;
    guint32 idx = (hist->count - 1) % HIST_CAPACITY;
    HistSample *s = &hist->samples[idx];
    if (cpu) *cpu = s->cpu;
    if (temp) *temp = s->temp;
    if (net_down) *net_down = s->net_down;
    if (net_up) *net_up = s->net_up;
    if (bat) {
        memset(bat, 0, sizeof(*bat));
        bat->present = (s->bat_cap > 0 || s->bat_watts > 0);
        bat->cap = (int)s->bat_cap;
        bat->watts = s->bat_watts;
    }
    return TRUE;
}
