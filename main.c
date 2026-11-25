/* barless - always-on OLED-friendly indicator + empty-workspace dashboard.
 *
 *   barless               run the indicator (it expands into the card on empty workspaces)
 *   barless --dashboard   run only the card (for designing it)
 *   barless --launcher [theme|background|<search>]   open the launcher (bind it to a key), optionally straight in a picker
 *   ... | barless --dmenu a dmenu: lines in, the chosen line out
 *   barless --info        print which sensors/sockets it found, then exit
 *
 * Preview the battery colours without draining the battery:
 *   BARLESS_FAKE_BAT="18 discharging 7.5" barless      (capacity, charging|discharging|idle, watts)
 */
#include "barless.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

static void usage(void) {
    g_print("usage: barless [--dashboard] [--info] [--help]\n"
            "  config: %s/config.ini   style: %s/style.css\n", cfg_dir(), cfg_dir());
}

static int info(void) {
    g_print("config dir: %s\n", cfg_dir());
    const char *tp = temp_sensor_path();
    if (tp && *tp) g_print("cpu temp:   %s  ->  %d C\n", tp, temp_read());
    else g_print("cpu temp:   no sensor found (set temp_path in config.ini)\n");
    BatInfo b;
    bat_read(&b);
    if (b.present)
        g_print("battery:    %d%%  %s  %.1f W\n", b.cap,
                b.charging ? "charging" : b.discharging ? "discharging" : "idle/full", b.watts);
    else
        g_print("battery:    none found\n");
    hypr_info();
    return 0;
}

int main(int argc, char **argv) {
    /* --launcher: tell the running barless to open the launcher (no GTK needed) */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--launcher")) {
            char *pf = barless_pidfile(), *txt = NULL;
            if (i + 1 < argc && argv[i + 1][0] != '-') {          /* --launcher themes | backgrounds */
                char *mp = g_strconcat(pf, ".mode", NULL);
                g_file_set_contents(mp, argv[i + 1], -1, NULL);
                g_free(mp);
            }
            int pid = (g_file_get_contents(pf, &txt, NULL, NULL) && txt) ? atoi(txt) : 0;
            g_free(pf); g_free(txt);
            if (pid > 1 && kill(pid, SIGUSR1) == 0) return 0;
            g_printerr("barless is not running\n");
            return 1;
        }
    gboolean dash = FALSE, want_info = FALSE, dmenu = FALSE, menu = FALSE;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dashboard")) dash = TRUE;
        else if (!strcmp(argv[i], "--info")) want_info = TRUE;
        else if (!strcmp(argv[i], "--dmenu")) dmenu = TRUE;
        else if (!strcmp(argv[i], "--menu")) menu = TRUE;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { cfg_load(); usage(); return 0; }
    }
    gtk_init(&argc, &argv);
    cfg_load();
    if (want_info) return info();
    css_init();
    if (dmenu) return launcher_dmenu(argc, argv);
    if (menu) return launcher_menu(argc, argv);
    if (dash) indicator_demo();
    return indicator_run();
}
