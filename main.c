/* barless - always-on OLED-friendly indicator + empty-workspace dashboard.
 *
 *   barless               run the indicator (it expands into the card on empty workspaces)
 *   barless --dashboard   run only the card (for designing it)
 *   barless --info        print which sensors/sockets it found, then exit
 *
 * Preview the battery colours without draining the battery:
 *   BARLESS_FAKE_BAT="18 discharging 7.5" barless      (capacity, charging|discharging|idle, watts)
 */
#include "barless.h"
#include <string.h>
#include <stdio.h>

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
    gboolean dash = FALSE, want_info = FALSE;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dashboard")) dash = TRUE;
        else if (!strcmp(argv[i], "--info")) want_info = TRUE;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { cfg_load(); usage(); return 0; }
    }
    gtk_init(&argc, &argv);
    cfg_load();
    if (want_info) return info();
    css_init();
    if (dash) indicator_demo();
    return indicator_run();
}
