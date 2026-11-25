/* launcher.c - one search box for everything: apps (.desktop), Omarchy menu entries ("settings"), Omarchy bins.
 *
 *   barless --launcher        (bind it to a key: it signals the running barless, which does the rest)
 *   ... | barless --dmenu     (a walker --dmenu replacement: lines in, the chosen line out)
 *
 * In the running barless the indicator itself travels from the top-right corner to the middle of the screen,
 * the panel opens there and its rows fade in one by one. Closing is instant: the indicator is back at once.
 * Nothing is built while it is closed; everything below is created on open and destroyed on close.
 */
#include "barless.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <malloc.h>
#include <gdk/gdkkeysyms.h>
#include <signal.h>

/* ================================================================== */
/* Items                                                               */
/* ================================================================== */
static gboolean l_calc = TRUE, l_runner = TRUE, run_in_term = FALSE;
enum { K_APP, K_SET, K_BIN, K_LINE, K_PICK, K_PIC, K_CALC, K_RUN };   /* K_CALC: a calculator result (Enter copies it); K_RUN: run what was typed */   /* K_PICK: opens a picker; K_PIC: one entry of a picker (with a preview image) */

typedef struct {
    char *label;        /* shown */
    char *key;          /* lowercase text that is searched */
    int kind;
    GAppInfo *app;      /* K_APP */
    char *cmd;          /* K_SET / K_BIN / K_PIC: command; K_PICK: "themes" | "backgrounds" */
    char *preview;      /* K_PIC: image path */
    int hist;           /* how often it was run */
    int pin;            /* [launcher] pin=: goes first */
    int score;
    int pre;            /* K_SET from a menu script: 1-based index of that script's helper functions (terminal, present_terminal ...) */
} Item;

static GPtrArray *items = NULL;          /* everything */
static GPtrArray *shown = NULL;          /* what the list currently displays (borrowed) */
static GHashTable *history = NULL;       /* "kind:label" -> count */
static gboolean dmenu_mode = FALSE;
static gboolean standalone = FALSE;       /* --menu: the launcher in a process of its own */
/* [launcher] options */
static gboolean l_enabled = TRUE, l_separate = FALSE, l_show_kind = TRUE, l_card = FALSE, l_card_hide_typing = TRUE;
static char *l_placeholder = NULL, *l_card_secs = NULL;
static int l_idle_s = 120;
static GtkWidget *cardw = NULL;
static gboolean cardw_film = FALSE;      /* the launcher plays the card's film for it */
static double film_total = 0, intro_end = 0;
static int film_edelay = 25;
static int list_h = 0;
static double l_alpha = 0.65;            /* opacity of the whole launcher (the card in it too); 0 = leave it to .card */                    /* pixel height of the result list */
static guint idle_id = 0;
static gint64 last_act = 0;
#define NO_INDICATOR (dmenu_mode || standalone)
static char *dmenu_prompt = NULL;
static int dmenu_width = 0;

static void item_free(gpointer p) {
    Item *i = p;
    g_free(i->label); g_free(i->key); g_free(i->cmd); g_free(i->preview);
    if (i->app) g_object_unref(i->app);
    g_free(i);
}

static Item *item_add(int kind, const char *label, const char *extra_key) {
    Item *i = g_new0(Item, 1);
    i->kind = kind;
    i->label = g_strdup(label);
    char *both = extra_key && *extra_key ? g_strdup_printf("%s %s", label, extra_key) : g_strdup(label);
    i->key = g_utf8_strdown(both, -1);
    g_free(both);
    g_ptr_array_add(items, i);
    return i;
}

static char *expand_home(const char *p) {
    if (p[0] == '~' && (p[1] == '/' || !p[1])) return g_build_filename(g_get_home_dir(), p + (p[1] ? 2 : 1), NULL);
    return g_strdup(p);
}

/* ---- history: ~/.local/state/barless/launcher-history, "count<TAB>kind<TAB>label" ---- */
static char *hist_path(void) {
    const char *st = g_getenv("XDG_STATE_HOME");
    char *dir = st && *st ? g_build_filename(st, "barless", NULL) : g_build_filename(g_get_home_dir(), ".local", "state", "barless", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *f = g_build_filename(dir, "launcher-history", NULL);
    g_free(dir);
    return f;
}

static void hist_load(void) {
    history = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *p = hist_path(), *txt = NULL;
    if (g_file_get_contents(p, &txt, NULL, NULL)) {
        char **ls = g_strsplit(txt, "\n", -1);
        for (char **l = ls; *l; l++) {
            char **f = g_strsplit(*l, "\t", 3);
            if (f[0] && f[1] && f[2]) g_hash_table_replace(history, g_strdup_printf("%s:%s", f[1], f[2]), GINT_TO_POINTER(atoi(f[0])));
            g_strfreev(f);
        }
        g_strfreev(ls);
        g_free(txt);
    }
    g_free(p);
}

static void hist_bump(Item *it) {
    if (!history || it->kind == K_LINE || it->kind == K_CALC || it->kind == K_RUN) return;
    char *k = g_strdup_printf("%d:%s", it->kind, it->label);
    int n = GPOINTER_TO_INT(g_hash_table_lookup(history, k)) + 1;
    g_hash_table_replace(history, k, GINT_TO_POINTER(n));
    GString *out = g_string_new("");
    GHashTableIter hi;
    gpointer a, b;
    g_hash_table_iter_init(&hi, history);
    while (g_hash_table_iter_next(&hi, &a, &b)) {
        const char *key = a;
        const char *colon = strchr(key, ':');
        if (colon) g_string_append_printf(out, "%d\t%.*s\t%s\n", GPOINTER_TO_INT(b), (int)(colon - key), key, colon + 1);
    }
    char *p = hist_path();
    g_file_set_contents(p, out->str, -1, NULL);
    g_free(p);
    g_string_free(out, TRUE);
}

/* ---- source 1: .desktop applications (GIO does the parsing) ---- */
static void index_apps(void) {
    GList *all = g_app_info_get_all();
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (GList *l = all; l; l = l->next) {
        GAppInfo *a = l->data;
        if (!g_app_info_should_show(a)) continue;
        const char *name = g_app_info_get_display_name(a);
        if (!name || g_hash_table_contains(seen, name)) continue;
        g_hash_table_add(seen, (gpointer)name);
        const char *exe = g_app_info_get_executable(a);
        char *base = exe ? g_path_get_basename(exe) : NULL;
        const char *desc = g_app_info_get_description(a);
        char *extra = g_strdup_printf("%s %s", base ? base : "", desc ? desc : "");
        Item *i = item_add(K_APP, name, extra);
        i->app = g_object_ref(a);
        g_free(extra); g_free(base);
    }
    g_hash_table_destroy(seen);
    g_list_free_full(all, g_object_unref);
}

/* ---- source 2: the entries of Omarchy's menu script:  ["󰀻  Apps"]="omarchy-launch-walker"  ---- */
/* The commands of the menu script call its own shell functions (terminal, present_terminal, open_in_editor,
   install_font ...). Nothing is guessed here: the functions are read out of the script itself and put in front
   of the command when it runs. The script's own menus (menu(), show_font_menu ...) call walker --dmenu; that call
   is pointed at barless --dmenu in the copy that runs, the file on disk is never touched. */
static GPtrArray *menu_pre = NULL;
static char *self_exe(void) {
    char *p = g_file_read_link("/proc/self/exe", NULL);
    return p ? p : g_strdup("barless");
}
static int menu_prelude(const char *txt) {
    const char *st = strstr(txt, "\nmenu() {");
    if (!st) return 0;
    const char *en = strstr(st, "\n# --- Flat Map");
    if (!en) en = strstr(st, "\ndeclare -A MENU_MAP");
    if (!en) return 0;
    char *body = g_strndup(st + 1, en - st);
    char *exe = self_exe();
    char *dm = g_strdup_printf("'%s' --dmenu", exe);
    GString *g = g_string_new(body);
    g_string_replace(g, "omarchy-launch-walker --dmenu", dm, 0);
    g_string_replace(g, "walker --dmenu", dm, 0);
    g_free(dm); g_free(exe); g_free(body);
    if (!menu_pre) menu_pre = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(menu_pre, g_string_free(g, FALSE));
    return (int)menu_pre->len;
}
static char *dq_unescape(const char *s) {                    /* the entry sits inside "..." in the script: \$ \" \\ \` */
    GString *o = g_string_new("");
    for (; *s; s++) {
        if (*s == '\\' && s[1] && strchr("$\"\\`", s[1])) s++;
        g_string_append_c(o, *s);
    }
    return g_string_free(o, FALSE);
}

static void index_menu_file(const char *path) {
    char *txt = NULL;
    if (!g_file_get_contents(path, &txt, NULL, NULL)) return;
    int pre = menu_prelude(txt);
    char **ls = g_strsplit(txt, "\n", -1);
    for (char **l = ls; *l; l++) {
        const char *s = g_strstrip(*l);
        if (s[0] != '[' || s[1] != '"') continue;
        const char *q = strstr(s + 2, "\"]=\"");
        if (!q) continue;
        char *label = g_strndup(s + 2, q - (s + 2));
        char *cmd = g_strdup(q + 4);
        size_t n = strlen(cmd);
        if (n && cmd[n - 1] == '"') cmd[n - 1] = '\0';
        gboolean pick = strstr(cmd, "menus:omarchythemes") || strstr(cmd, "menus:omarchyBackgroundSelector");   /* Elephant menus: our own pickers */
        if (!pick && (strstr(cmd, "walker") || !*cmd)) { g_free(label); g_free(cmd); continue; }   /* needs Walker: not a plain command */
        size_t ln = strlen(label);
        while (ln && label[ln - 1] == '/') label[--ln] = '\0';
        GString *disp = g_string_new("");
        for (char *c = label; *c; c++) {
            if (*c == '/') g_string_append(disp, " \xe2\x80\xba ");        /* "Update/Config" -> "Update › Config" */
            else g_string_append_c(disp, *c);
        }
        Item *i = item_add(pick ? K_PICK : K_SET, disp->str, NULL);
        i->cmd = pick ? g_strdup(strstr(cmd, "omarchythemes") ? "themes" : "backgrounds") : dq_unescape(cmd);
        if (!pick) {
            i->pre = pre;
            if (!strcmp(i->cmd, "omarchy-menu-keybindings")) {   /* its list is the point; only its last line (walker) is swapped for us */
                char *exe = self_exe();
                char *nc = g_strdup_printf("omarchy-menu-keybindings --print | '%s' --dmenu -p Keybindings", exe);
                g_free(exe); g_free(i->cmd); i->cmd = nc;
            }
        }
        g_string_free(disp, TRUE);
        g_free(label); g_free(cmd);
    }
    g_strfreev(ls);
    g_free(txt);
}

/* ---- source 3: omarchy-* executables (only offered once you type something) ---- */
static void index_bins(const char *dirs0) {
    char *dirs = NULL;
    const char *pv = strstr(dirs0, "$PATH");                  /* "$PATH" stands for every folder of your PATH */
    if (pv) dirs = g_strdup_printf("%.*s%s%s", (int)(pv - dirs0), dirs0, g_getenv("PATH") ? g_getenv("PATH") : "", pv + 5);
    else dirs = g_strdup(dirs0);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char **ds = g_strsplit(dirs, ":", -1);
    for (char **d = ds; *d; d++) {
        if (!**d) continue;
        char *dir = expand_home(*d);
        DIR *dp = opendir(dir);
        struct dirent *e;
        while (dp && (e = readdir(dp))) {
            if (e->d_name[0] == '.') continue;
            char *full = g_build_filename(dir, e->d_name, NULL);
            struct stat st;
            if (!g_hash_table_contains(seen, e->d_name) && stat(full, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111)) {
                g_hash_table_add(seen, g_strdup(e->d_name));
                char *nice = g_strdelimit(g_strdup(e->d_name), "-", ' ');
                Item *i = item_add(K_BIN, e->d_name, nice);
                i->cmd = g_strdup(e->d_name);
                g_free(nice);
            }
            g_free(full);
        }
        if (dp) closedir(dp);
        g_free(dir);
    }
    g_strfreev(ds);
    g_hash_table_destroy(seen);
    g_free(dirs);
}

static void index_dmenu(void) {
    char buf[4096];
    while (fgets(buf, sizeof buf, stdin)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
        if (!n) continue;
        item_add(K_LINE, buf, NULL);
    }
}

static void build_index(void) {
    items = g_ptr_array_new_with_free_func(item_free);
    if (dmenu_mode) { index_dmenu(); return; }
    hist_load();
    char *src = cfg_str("launcher", "sources", "apps,settings,bins");
    if (strstr(src, "apps")) index_apps();
    if (strstr(src, "settings")) {
        char *mf = cfg_str("launcher", "menu_file", "~/.local/share/omarchy/bin/omarchy-menu-alternative:~/.local/share/omarchy/bin/omarchy-menu");
        char **fs = g_strsplit(mf, ":", -1);
        for (char **f = fs; *f; f++) { char *p = expand_home(*f); index_menu_file(p); g_free(p); }
        g_strfreev(fs);
        g_free(mf);
    }
    if (strstr(src, "settings")) {                            /* the pickers exist even without the menu file */
        gboolean t = FALSE, b = FALSE;
        for (guint n = 0; n < items->len; n++) {
            Item *i = g_ptr_array_index(items, n);
            if (i->kind == K_PICK) { if (!strcmp(i->cmd, "themes")) t = TRUE; else b = TRUE; }
        }
        if (!t) item_add(K_PICK, "Style \xe2\x80\xba Theme", NULL)->cmd = g_strdup("themes");
        if (!b) item_add(K_PICK, "Style \xe2\x80\xba Background", NULL)->cmd = g_strdup("backgrounds");
    }
    {                                                       /* [launcher:entry:name]  label= cmd=  : your own entries */
        char **gs = cfg_groups();
        for (char **g = gs; *g; g++) {
            if (!g_str_has_prefix(*g, "launcher:entry:")) continue;
            char *label = cfg_str(*g, "label", *g + 15), *cmd = cfg_str(*g, "cmd", NULL);
            if (cmd && *cmd && !cfg_bool(*g, "hide", FALSE)) item_add(K_SET, label, NULL)->cmd = g_strdup(cmd);
            g_free(label); g_free(cmd);
        }
        g_strfreev(gs);
    }
    if (strstr(src, "bins")) {
        char *bd = cfg_str("launcher", "bin_dirs", "$PATH");
        index_bins(bd);
        g_free(bd);
    }
    g_free(src);
    for (guint n = 0; n < items->len; n++) {
        Item *i = g_ptr_array_index(items, n);
        char *k = g_strdup_printf("%d:%s", i->kind, i->label);
        i->hist = GPOINTER_TO_INT(g_hash_table_lookup(history, k));
        g_free(k);
    }
    char *hs = cfg_str("launcher", "hide", "");               /* hide= comma separated words: entries containing one are dropped */
    char **hp = g_strsplit(hs, ",", -1);
    for (char **p = hp; *p; p++) { g_strstrip(*p); char *lo = g_utf8_strdown(*p, -1); g_free(*p); *p = lo; }
    for (guint n = items->len; n-- > 0;) {
        Item *i = g_ptr_array_index(items, n);
        for (char **p = hp; *p; p++)
            if (**p && strstr(i->key, *p)) { g_ptr_array_remove_index(items, n); break; }
    }
    g_strfreev(hp);
    g_free(hs);
    char *ps = cfg_str("launcher", "pin", "");                /* pin= comma separated labels, listed first in this order */
    char **pp = g_strsplit(ps, ",", -1);
    for (char **p = pp; *p; p++) {
        g_strstrip(*p);
        if (!**p) continue;
        for (guint n = 0; n < items->len; n++) {
            Item *i = g_ptr_array_index(items, n);
            if (!g_ascii_strcasecmp(i->label, *p)) { i->pin = 1000 - (int)(p - pp); break; }
        }
    }
    g_strfreev(pp);
    g_free(ps);
}

/* ================================================================== */
/* Matching                                                            */
/* ================================================================== */
/* subsequence match with bonuses for a prefix, a word start and consecutive letters; -1 = no match */
/* Matching, light on purpose (a few string scans per entry, no tables, no library).
   Every word you type must match, in this order of quality:
     start of a word  >  inside a word  >  letters in order, but only from the start of a word and close together.
   So "moni" finds Monitors, never "Animated Background Install" (its m-o-n-i are scattered far apart). */
static gboolean is_sep(char c) { return c == ' ' || c == '-' || c == '_' || c == '/' || c == '.'; }
static int tok_score(const char *t, int tl, const char *k) {
    int best = -1;
    for (const char *w = k; *w;) {                           /* w is at the start of a word */
        if (!strncmp(w, t, tl)) best = MAX(best, 1000 + (w == k ? 300 : 0));
        while (*w && !is_sep(*w)) w++;
        while (*w && is_sep(*w)) w++;
    }
    if (best >= 0) return best;
    const char *f = strstr(k, t);
    if (f) return 600 - MIN((int)(f - k), 50);
    if (tl >= 2) {                                           /* initials: "vsc" = Visual Studio Code */
        char ini[64];
        int n = 0, first = 1;
        for (const char *w = k; *w && n < 63;) {
            ini[n++] = *w;
            while (*w && !is_sep(*w)) w++;
            while (*w && is_sep(*w)) w++;
        }
        ini[n] = 0;
        const char *h = strstr(ini, t);
        if (h) { first = (h == ini); best = MAX(best, 450 + (first ? 100 : 0)); }
    }
    if (best >= 0) return best;
    if (tl >= 3) {                                           /* letters in order: from a word start, at most a few letters apart */
        for (const char *w = k; *w;) {
            if (*w == t[0]) {
                const char *p = w + 1;
                int ok = 1;
                for (int i = 1; i < tl && ok; i++) {
                    const char *g = strchr(p, t[i]);
                    if (!g) { ok = 0; break; }
                    for (const char *x = p; x < g; x++) if (is_sep(*x)) { ok = 0; break; }   /* inside one word */
                    p = g + 1;
                }
                int span = (int)(p - w);
                if (ok && span <= tl + 2) best = MAX(best, 300 - span * 8);
            }
            while (*w && !is_sep(*w)) w++;
            while (*w && is_sep(*w)) w++;
        }
    }
    return best;
}
static int fuzzy(const char *q, const char *k) {
    int total = 0, n = 0;
    char **toks = g_strsplit(q, " ", -1);
    for (char **t = toks; *t; t++) {
        if (!**t) continue;
        int s = tok_score(*t, (int)strlen(*t), k);
        if (s < 0) { g_strfreev(toks); return -1; }
        total += s;
        n++;
    }
    g_strfreev(toks);
    if (!n) return 0;
    return total + MAX(0, 30 - (int)strlen(k) / 2);
}

static int item_cmp(gconstpointer a, gconstpointer b) {
    const Item *x = *(Item *const *)a, *y = *(Item *const *)b;
    if (x->score != y->score) return y->score - x->score;
    return g_utf8_collate(x->label, y->label);
}

/* ================================================================== */
/* The panel                                                           */
/* ================================================================== */
static GtkWidget *host = NULL;           /* the toplevel: the indicator's window, or our own one for --dmenu */
static GtkWidget *stage = NULL, *panel = NULL, *entry = NULL, *list = NULL, *scroll = NULL;
static int rows_cfg = 9, panel_w = 560, panel_top = 22, max_results = 40;
static int mon_w = 1920, mon_h = 1080, panel_x = 0, panel_y = 0;
static double p_reveal = 1.0;
typedef enum { LS_CLOSED, LS_TRAVEL, LS_OPEN } LState;
static LState lstate = LS_CLOSED;
static int travel_ms = 180, expand_ms = 120, elem_delay = 18;
static double t_ms = 0;
static gint64 last_us = 0;
static gboolean ticking = FALSE, intro_done = TRUE;
static int rx0, ry0, rtx, rty, row_w, row_h;
static char *chosen = NULL;              /* --dmenu result */
/* the morph: the panel is one shape that changes position and size, starting as the indicator and ending as the panel */
static Morph mo;
static gboolean mo_on = FALSE, closing = FALSE, l_morph = FALSE;
static gint64 mo_t0 = 0;
static double mo_delay = 0, mo_content_a = 1.0;

static double ease_out(double t) { t = CLAMP(t, 0.0, 1.0); return 1.0 - (1.0 - t) * (1.0 - t); }

/* ================================================================== */
/* Pickers: Theme / Background, with an image preview                  */
/* (what Elephant's omarchy_themes.lua / omarchy_background_selector.lua did)  */
/* ================================================================== */
static GPtrArray *main_items = NULL;     /* the normal index, parked while a picker shows its own list */
static int pmode = 0;                    /* 0 = main list, 1 = themes, 2 = backgrounds */
static int preview_w = 300, pv_h = 170;                      /* the picture sits where the card sits: same width as the panel */
static GtkWidget *pvbox = NULL, *pvimg = NULL;
static GCancellable *pv_cancel = NULL;
static guint pv_timer = 0, pv_gen = 0;

static char *title_case(const char *n) {                    /* "tokyo-night" -> "Tokyo Night" */
    char *t = g_strdup(n);
    gboolean start = TRUE;
    for (char *c = t; *c; c++) {
        if (*c == '-' || *c == '_') { *c = ' '; start = TRUE; }
        else { *c = start ? g_ascii_toupper(*c) : g_ascii_tolower(*c); start = FALSE; }
    }
    return t;
}

static char *read_line_file(const char *path) {
    char *txt = NULL;
    if (!g_file_get_contents(path, &txt, NULL, NULL) || !txt) return NULL;
    g_strstrip(txt);
    return txt;
}

static gint name_cmp(gconstpointer a, gconstpointer b) { return g_strcmp0(*(char *const *)a, *(char *const *)b); }
static char *read_line_file_rel(const char *home, const char *rel) {
    char *p = g_build_filename(home, rel, NULL), *r = read_line_file(p);
    g_free(p);
    return r;
}

static char *shquote(const char *s) {                       /* 'a'\''b' */
    GString *o = g_string_new("'");
    for (; *s; s++) { if (*s == '\'') g_string_append(o, "'\\''"); else g_string_append_c(o, *s); }
    g_string_append_c(o, '\'');
    return g_string_free(o, FALSE);
}

static gboolean is_image(const char *n) {
    const char *d = strrchr(n, '.');
    if (!d) return FALSE;
    return !g_ascii_strcasecmp(d, ".png") || !g_ascii_strcasecmp(d, ".jpg") || !g_ascii_strcasecmp(d, ".jpeg") ||
           !g_ascii_strcasecmp(d, ".webp") || !g_ascii_strcasecmp(d, ".bmp") || !g_ascii_strcasecmp(d, ".gif");
}

static void pick_themes(void) {
    char *home = expand_home("~");
    const char *op = g_getenv("OMARCHY_PATH");
    char *dirs[2] = { g_build_filename(home, ".config/omarchy/themes", NULL),
                      op && *op ? g_build_filename(op, "themes", NULL) : g_build_filename(home, ".local/share/omarchy/themes", NULL) };
    char *curp = g_build_filename(home, ".config/omarchy/current/theme.name", NULL);
    char *cur = read_line_file(curp);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (int d = 0; d < 2; d++) {                           /* the user's own themes come first and win over system ones */
        GDir *gd = g_dir_open(dirs[d], 0, NULL);
        if (!gd) continue;
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        const char *n;
        while ((n = g_dir_read_name(gd))) {
            if (n[0] == '.') continue;
            char *full = g_build_filename(dirs[d], n, NULL);
            if (g_file_test(full, G_FILE_TEST_IS_DIR) && !g_hash_table_contains(seen, n)) {
                g_hash_table_add(seen, g_strdup(n));
                g_ptr_array_add(names, g_strdup(n));
            }
            g_free(full);
        }
        g_dir_close(gd);
        g_ptr_array_sort(names, name_cmp);   /* pointer-to-pointer compare below */
        for (guint k = 0; k < names->len; k++) {
            const char *nm = g_ptr_array_index(names, k);
            char *t = title_case(nm);
            Item *i = item_add(K_PIC, t, nm);
            char *q = shquote(nm);
            i->cmd = g_strdup_printf("omarchy-theme-set %s", q);
            g_free(q);
            const char *ex[] = { "preview.png", "preview.jpg", "preview.jpeg", "preview.webp", NULL };
            for (int e = 0; ex[e] && !i->preview; e++) {
                char *pp = g_build_filename(dirs[d], nm, ex[e], NULL);
                if (g_file_test(pp, G_FILE_TEST_EXISTS)) i->preview = pp; else g_free(pp);
            }
            if (!i->preview) {                                       /* like the Lua menu: first image of the theme's backgrounds */
                char *bd = g_build_filename(dirs[d], nm, "backgrounds", NULL);
                GDir *g2 = g_dir_open(bd, 0, NULL);
                if (g2) {
                    GPtrArray *fs = g_ptr_array_new_with_free_func(g_free);
                    const char *f;
                    while ((f = g_dir_read_name(g2))) if (f[0] != '.') g_ptr_array_add(fs, g_strdup(f));
                    g_dir_close(g2);
                    g_ptr_array_sort(fs, name_cmp);
                    if (fs->len) i->preview = g_build_filename(bd, g_ptr_array_index(fs, 0), NULL);
                    g_ptr_array_free(fs, TRUE);
                }
                g_free(bd);
            }
            if (cur && !g_ascii_strcasecmp(cur, nm)) i->hist = 1;    /* marks the current theme (preselected) */
            g_free(t);
        }
        g_ptr_array_free(names, TRUE);
    }
    g_hash_table_destroy(seen);
    g_free(dirs[0]); g_free(dirs[1]); g_free(curp); g_free(cur); g_free(home);
}

static char *bg_title(const char *f) {                      /* "1-forest-night.png" -> "Forest Night" (FormatName of the Lua menu) */
    const char *p = f;
    while (g_ascii_isdigit(*p)) p++;
    if (*p == '-') p++;
    char *t = g_strdup(*p ? p : f);
    char *dot = strrchr(t, '.');
    if (dot && dot != t) *dot = '\0';
    char *r = title_case(t);
    g_free(t);
    return r;
}

static void pick_bgs(void) {
    char *home = expand_home("~");
    char *cur = read_line_file_rel(home, ".config/omarchy/current/theme.name");
    char *dirs[2];
    dirs[0] = g_build_filename(home, ".config/omarchy/current/theme/backgrounds", NULL);
    dirs[1] = cur ? g_build_filename(home, ".config/omarchy/backgrounds", cur, NULL) : NULL;
    char *curbg = NULL;
    {
        char *l = g_build_filename(home, ".config/omarchy/current/background", NULL);
        curbg = g_file_read_link(l, NULL);
        g_free(l);
    }
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (int d = 0; d < 2; d++) {
        if (!dirs[d]) continue;
        GDir *gd = g_dir_open(dirs[d], 0, NULL);
        if (!gd) continue;
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        const char *n;
        while ((n = g_dir_read_name(gd))) if (n[0] != '.' && is_image(n)) g_ptr_array_add(names, g_strdup(n));
        g_dir_close(gd);
        g_ptr_array_sort(names, name_cmp);
        for (guint k = 0; k < names->len; k++) {
            const char *fn = g_ptr_array_index(names, k);
            char *full = g_build_filename(dirs[d], fn, NULL);
            if (g_hash_table_contains(seen, fn)) { g_free(full); continue; }          /* same file name in both folders: once */
            g_hash_table_add(seen, g_strdup(fn));
            char *pt = bg_title(fn);
            Item *i = item_add(K_PIC, pt, fn);
            g_free(pt);
            char *q = shquote(full);
            i->cmd = g_strdup_printf("omarchy-theme-bg-set %s", q);
            g_free(q);
            i->preview = full;
            if (curbg && !strcmp(curbg, full)) i->hist = 1;
        }
        g_ptr_array_free(names, TRUE);
    }
    g_hash_table_destroy(seen);
    g_free(dirs[0]); g_free(dirs[1]); g_free(cur); g_free(curbg); g_free(home);
}

static void panel_close(void);
static void panel_close_soft(void);
static void enter_picker(int mode);
static void leave_picker(void);

static double bar_y = 0, lh_cur = 0, lh_tgt = 0;
static int cur_row_h = 34;
static gboolean bar_snap = TRUE, ui_ticking = FALSE;
static void retarget_list(gboolean snap);
static void ui_kick(void);
static void mq_stop(void);
/* ================================================================== */
/* Calculator: a tiny expression evaluator, runs only on text that     */
/* looks like maths. + - * / % ^ ( ) and sqrt sin cos tan ln log ...   */
/* ================================================================== */
typedef struct { const char *p; gboolean err; } Calc;
static double calc_expr(Calc *c);
static void calc_ws(Calc *c) { while (*c->p == ' ') c->p++; }
static double calc_atom(Calc *c) {
    calc_ws(c);
    if (*c->p == '(') {
        c->p++;
        double v = calc_expr(c);
        calc_ws(c);
        if (*c->p == ')') c->p++; else c->err = TRUE;
        return v;
    }
    if (g_ascii_isdigit(*c->p) || *c->p == '.') {
        char *end;
        double v = g_ascii_strtod(c->p, &end);
        if (end == c->p) { c->err = TRUE; return 0; }
        c->p = end;
        return v;
    }
    if (g_ascii_isalpha(*c->p)) {
        char name[12] = "";
        int n = 0;
        while (g_ascii_isalpha(*c->p) && n < 10) name[n++] = *c->p++;
        name[n] = 0;
        if (!strcmp(name, "pi")) return M_PI;
        if (!strcmp(name, "e")) return M_E;
        calc_ws(c);
        if (*c->p != '(') { c->err = TRUE; return 0; }
        c->p++;
        double v = calc_expr(c);
        calc_ws(c);
        if (*c->p == ')') c->p++; else c->err = TRUE;
        if (!strcmp(name, "sqrt")) return sqrt(v);
        if (!strcmp(name, "sin")) return sin(v);
        if (!strcmp(name, "cos")) return cos(v);
        if (!strcmp(name, "tan")) return tan(v);
        if (!strcmp(name, "asin")) return asin(v);
        if (!strcmp(name, "acos")) return acos(v);
        if (!strcmp(name, "atan")) return atan(v);
        if (!strcmp(name, "ln")) return log(v);
        if (!strcmp(name, "log")) return log10(v);
        if (!strcmp(name, "exp")) return exp(v);
        if (!strcmp(name, "abs")) return fabs(v);
        if (!strcmp(name, "floor")) return floor(v);
        if (!strcmp(name, "ceil")) return ceil(v);
        if (!strcmp(name, "round")) return round(v);
        c->err = TRUE;
        return 0;
    }
    c->err = TRUE;
    return 0;
}
static double calc_unary(Calc *c) {
    calc_ws(c);
    if (*c->p == '-') { c->p++; return -calc_unary(c); }
    if (*c->p == '+') { c->p++; return calc_unary(c); }
    double b = calc_atom(c);
    calc_ws(c);
    if (*c->p == '^') { c->p++; return pow(b, calc_unary(c)); }          /* right-associative */
    if (c->p[0] == '*' && c->p[1] == '*') { c->p += 2; return pow(b, calc_unary(c)); }
    return b;
}
static double calc_term(Calc *c) {
    double v = calc_unary(c);
    for (;;) {
        calc_ws(c);
        if (*c->p == '*' && c->p[1] != '*') { c->p++; v *= calc_unary(c); }
        else if (*c->p == '/') { c->p++; v /= calc_unary(c); }
        else if (*c->p == '%') { c->p++; v = fmod(v, calc_unary(c)); }
        else return v;
    }
}
static double calc_expr(Calc *c) {
    double v = calc_term(c);
    for (;;) {
        calc_ws(c);
        if (*c->p == '+') { c->p++; v += calc_term(c); }
        else if (*c->p == '-') { c->p++; v -= calc_term(c); }
        else return v;
    }
}
static char *calc_try(const char *q) {                       /* the result as text, or NULL when q is not maths */
    gboolean digit = FALSE, op = FALSE;
    for (const char *p = q; *p; p++) {
        if (g_ascii_isdigit(*p)) digit = TRUE;
        else if (strchr("+-*/%^(", *p) || g_ascii_isalpha(*p)) op = TRUE;
    }
    if (!digit || !op) return NULL;                          /* "7" or "firefox" is not a calculation */
    if (g_ascii_isalpha(q[0]) && !g_str_has_prefix(q, "sqrt") && !g_str_has_prefix(q, "sin") && !g_str_has_prefix(q, "cos") && !g_str_has_prefix(q, "tan") &&
        !g_str_has_prefix(q, "asin") && !g_str_has_prefix(q, "acos") && !g_str_has_prefix(q, "atan") && !g_str_has_prefix(q, "ln") && !g_str_has_prefix(q, "log") &&
        !g_str_has_prefix(q, "exp") && !g_str_has_prefix(q, "abs") && !g_str_has_prefix(q, "floor") && !g_str_has_prefix(q, "ceil") &&
        !g_str_has_prefix(q, "round") && !g_str_has_prefix(q, "pi") && !(q[0] == 'e' && !g_ascii_isalpha(q[1])))
        return NULL;                                         /* "libreoffice 7" starts with a word that is not a function */
    Calc c = { q, FALSE };
    double v = calc_expr(&c);
    calc_ws(&c);
    if (c.err || *c.p || !isfinite(v)) return NULL;
    char buf[48];
    g_snprintf(buf, sizeof buf, "%.10g", v);
    return g_strdup(buf);
}
static Item *dyn_item(int kind, char *label, char *cmd) {   /* a row that is not in the index: made fresh for each search */
    Item *i = g_new0(Item, 1);
    i->kind = kind;
    i->label = label;
    i->key = g_strdup("");
    i->cmd = cmd;
    i->score = 900000;
    return i;
}
static GPtrArray *dyn = NULL;

static void list_fill(void) {
    mq_stop();
    if (dyn) g_ptr_array_free(dyn, TRUE);
    dyn = g_ptr_array_new_with_free_func(item_free);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *k = kids; k; k = k->next) gtk_widget_destroy(k->data);
    g_list_free(kids);
    const char *qraw = gtk_entry_get_text(GTK_ENTRY(entry));
    char *q = g_utf8_strdown(qraw, -1);
    g_strstrip(q);
    size_t ql = strlen(q);
    g_ptr_array_set_size(shown, 0);
    GPtrArray *cand = g_ptr_array_new();
    for (guint n = 0; n < items->len; n++) {
        Item *i = g_ptr_array_index(items, n);
        if (ql == 0 && (dmenu_mode || pmode)) {
            i->score = 100000 - (int)n;                      /* dmenu: keep the order the script gave */
        } else if (ql == 0) {
            if (i->kind == K_BIN) continue;                 /* bins only once you type */
            i->score = (i->kind == K_APP ? 200000 : 100000) + i->hist * 40 + i->pin * 1000;     /* pinned, recent, then apps, then settings */
        } else {
            if (i->kind == K_BIN && ql < 2) continue;
            int s = fuzzy(q, i->key);
            if (s < 0) continue;
            i->score = pmode ? s : s + i->hist * 25 + (i->pin ? 60 : 0) + (i->kind == K_APP ? 40 : 0) - (i->kind == K_BIN ? (g_str_has_prefix(i->label, "omarchy-") ? 250 : 300) : 0);   /* same match quality: apps, settings, then programs */
        }
        g_ptr_array_add(cand, i);
    }
    if (ql > 0 && !dmenu_mode && !pmode) {
        const char *qs = qraw;
        while (*qs == ' ') qs++;
        char *res = l_calc ? calc_try(qs) : NULL;
        if (res) {                                            /* = 42   (Enter copies it) */
            char *lab = g_strdup_printf("= %s", res);
            char *cmd = g_strdup_printf("printf %%s '%s' | wl-copy", res);
            Item *ci = dyn_item(K_CALC, lab, cmd);
            g_ptr_array_add(dyn, ci);
            g_ptr_array_add(cand, ci);
            g_free(res);
        }
        if (l_runner && *qs) {                                /* "> cmd" or "known-program args": run the line as typed */
            const char *line = qs;
            gboolean force = *qs == '>';
            if (force) { line++; while (*line == ' ') line++; }
            gboolean ok = force && *line;
            if (!ok && strchr(qs, ' ')) {
                char *w0 = g_strndup(qs, strchr(qs, ' ') - qs), *k0 = g_utf8_strdown(w0, -1);
                for (guint n = 0; n < items->len && !ok; n++) {
                    Item *i = g_ptr_array_index(items, n);
                    if (i->kind == K_BIN && i->cmd && !g_ascii_strcasecmp(i->cmd, k0)) ok = TRUE;
                }
                g_free(w0); g_free(k0);
            }
            if (ok) {
                Item *ri = dyn_item(K_RUN, g_strdup_printf("Run: %s", line), g_strdup(line));
                ri->score = 800000;
                g_ptr_array_add(dyn, ri);
                g_ptr_array_add(cand, ri);
            }
        }
    }
    g_ptr_array_sort(cand, item_cmp);
    for (guint n = 0; n < cand->len && (int)n < max_results; n++) {
        Item *i = g_ptr_array_index(cand, n);
        g_ptr_array_add(shown, i);
        GtkWidget *rw = gtk_list_box_row_new();
        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        if (i->kind == K_APP) {
            GIcon *ic = g_app_info_get_icon(i->app);
            GtkWidget *img = ic ? gtk_image_new_from_gicon(ic, GTK_ICON_SIZE_LARGE_TOOLBAR) : gtk_image_new();
            gtk_image_set_pixel_size(GTK_IMAGE(img), 22);
            gtk_box_pack_start(GTK_BOX(hb), img, FALSE, FALSE, 0);
        }
        GtkWidget *lb = gtk_label_new(i->label);
        gtk_label_set_xalign(GTK_LABEL(lb), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lb), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(hb), lb, TRUE, TRUE, 0);
        if (!dmenu_mode && l_show_kind && i->kind != K_PIC) {
            GtkWidget *kd = gtk_label_new(i->kind == K_APP ? "app" : i->kind == K_SET ? "setting" : i->kind == K_PICK ? "picker" : i->kind == K_CALC ? "copy" : i->kind == K_RUN ? "run" : "command");
            gtk_style_context_add_class(gtk_widget_get_style_context(kd), "lkind");
            gtk_box_pack_end(GTK_BOX(hb), kd, FALSE, FALSE, 0);
        }
        gtk_container_add(GTK_CONTAINER(rw), hb);
        g_object_set_data(G_OBJECT(rw), "item", i);
        g_object_set_data(G_OBJECT(rw), "lb", lb);
        g_object_set_data(G_OBJECT(rw), "hb", hb);
        gtk_container_add(GTK_CONTAINER(list), rw);
        if (!intro_done) gtk_widget_set_opacity(rw, 0.0);
    }
    g_ptr_array_free(cand, TRUE);
    g_free(q);
    gtk_widget_show_all(list);
    bar_snap = TRUE;
    retarget_list(!intro_done || lh_cur <= 0);
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0);
    if (first) gtk_list_box_select_row(GTK_LIST_BOX(list), first);
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    gtk_adjustment_set_value(adj, 0);
}

static void child_setup(gpointer d) { (void)d; setsid(); }

static void run_item(Item *it) {
    if (dmenu_mode) { chosen = g_strdup(it->label); panel_close(); return; }
    if (it->kind == K_PICK) { enter_picker(!strcmp(it->cmd, "themes") ? 1 : 2); return; }
    if (it->kind != K_PIC) hist_bump(it);
    GAppInfo *app = it->app ? g_object_ref(it->app) : NULL;
    char *cmd = it->cmd ? g_strdup(it->cmd) : NULL;
    int kind = it->kind;                                     /* the row may be freed by panel_close */
    if (cmd && it->pre > 0 && menu_pre && (guint)it->pre <= menu_pre->len) {
        char *full = g_strdup_printf("%s\n%s", (char *)g_ptr_array_index(menu_pre, it->pre - 1), cmd);
        g_free(cmd);
        cmd = full;
    }
    panel_close();                                           /* focus goes back first, then the thing starts */
    if (app) {
        GError *err = NULL;
        if (!g_app_info_launch(app, NULL, NULL, &err)) { g_printerr("barless: %s\n", err->message); g_error_free(err); }
        g_object_unref(app);
    } else if (cmd) {
        char *tcmd = NULL;
        if (run_in_term && (kind == K_BIN || kind == K_RUN)) {      /* TUI programs (btop, nmtui ...) need a terminal */
            char *tc = cfg_str("dashboard", "terminal", "xdg-terminal-exec");
            tcmd = g_strdup_printf("%s %s", tc, cmd);
            g_free(tc);
        }
        gchar *argv[] = { "bash", "-c", tcmd ? tcmd : cmd, NULL };
        GError *err = NULL;
        if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                           child_setup, NULL, NULL, &err)) { g_printerr("barless: %s\n", err->message); g_error_free(err); }
        g_free(tcmd);
    }
    g_free(cmd);
}

static gint64 pv_fade_t0 = 0;
static gboolean pv_fade_tick(GtkWidget *w, GdkFrameClock *fc, gpointer d) {   /* the new picture fades in over 90 ms */
    (void)fc; (void)d;
    double p = (g_get_monotonic_time() - pv_fade_t0) / 90000.0;
    gtk_widget_set_opacity(w, CLAMP(p, 0.0, 1.0));
    return p >= 1.0 ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}
static void pv_fade_start(void) {
    if (!pvimg) return;
    gtk_widget_set_opacity(pvimg, 0.0);
    pv_fade_t0 = g_get_monotonic_time();
    gtk_widget_add_tick_callback(pvimg, pv_fade_tick, NULL, NULL);
}
static void pv_done(GObject *src, GAsyncResult *res, gpointer d) {
    GInputStream *st = G_INPUT_STREAM(src);
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_stream_finish(res, &err);
    g_object_unref(st);
    if (err) g_error_free(err);
    if (GPOINTER_TO_UINT(d) == pv_gen && pvimg && pb) { gtk_image_set_from_pixbuf(GTK_IMAGE(pvimg), pb); pv_fade_start(); }
    if (pb) g_object_unref(pb);
}
static void pv_opened(GObject *src, GAsyncResult *res, gpointer d) {
    GError *err = NULL;
    GFileInputStream *st = g_file_read_finish(G_FILE(src), res, &err);
    if (err) g_error_free(err);
    if (!st) return;
    if (GPOINTER_TO_UINT(d) != pv_gen || !pvimg || !pv_cancel) { g_object_unref(st); return; }
    gdk_pixbuf_new_from_stream_at_scale_async(G_INPUT_STREAM(st), preview_w, pv_h, TRUE, pv_cancel, pv_done, d);
}
static gboolean pv_fire(gpointer d) {
    pv_timer = 0;
    GtkListBoxRow *r = list ? gtk_list_box_get_selected_row(GTK_LIST_BOX(list)) : NULL;
    Item *it = r ? g_object_get_data(G_OBJECT(r), "item") : NULL;
    if (pv_cancel) { g_cancellable_cancel(pv_cancel); g_object_unref(pv_cancel); }
    pv_cancel = g_cancellable_new();
    pv_gen++;
    (void)d;
    if (!it || !it->preview) { if (pvimg) gtk_image_clear(GTK_IMAGE(pvimg)); return G_SOURCE_REMOVE; }
    GFile *f = g_file_new_for_path(it->preview);
    g_file_read_async(f, G_PRIORITY_DEFAULT, pv_cancel, pv_opened, GUINT_TO_POINTER(pv_gen));
    g_object_unref(f);
    return G_SOURCE_REMOVE;
}
/* ================================================================== */
/* The list's look: corner marks, one accent bar that slides, hover,   */
/* a marquee for the long selected line, a height that eases           */
/* ================================================================== */
static gint64 ui_last = 0;
static GdkRGBA col_accent = { 0.9, 0.3, 0.3, 1 };
static int l_bar_pct = 45;
static double l_speed = 1.0;
static int l_corner = 3;
static gint64 sb_t = 0;                                      /* the scrollbar: last time the list scrolled */
static gboolean sb_ticking = FALSE;

static gboolean sel_rect(double *y, double *h) {             /* the selected row, in the list's own coordinates */
    GtkListBoxRow *r = list ? gtk_list_box_get_selected_row(GTK_LIST_BOX(list)) : NULL;
    if (!r) return FALSE;
    int x = 0, yy = 0;
    GtkAllocation a;
    gtk_widget_get_allocation(GTK_WIDGET(r), &a);
    if (a.height <= 1) return FALSE;
    if (!gtk_widget_translate_coordinates(GTK_WIDGET(r), list, 0, 0, &x, &yy)) return FALSE;
    if (y) *y = yy;
    if (h) *h = a.height;
    return TRUE;
}

static void dot(cairo_t *cr, double x, double y) { cairo_rectangle(cr, x, y, 2, 2); cairo_fill(cr); }

static gboolean list_draw_over(GtkWidget *w, cairo_t *cr, gpointer d) {
    (void)d;
    GdkRGBA fg;
    gtk_style_context_get_color(gtk_widget_get_style_context(w), GTK_STATE_FLAG_NORMAL, &fg);
    double W = gtk_widget_get_allocated_width(w);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
    double sy = 0, sh = 0;
    gboolean have = sel_rect(&sy, &sh);
    for (GList *k = kids; k; k = k->next) {                   /* a dot at each end of every row boundary: where one entry ends and the next begins */
        GtkAllocation a;
        gtk_widget_get_allocation(k->data, &a);
        if (a.height <= 1) continue;
        double op = gtk_widget_get_opacity(k->data);
        gdk_cairo_set_source_rgba(cr, &(GdkRGBA){ fg.red, fg.green, fg.blue, 0.35 * op });
        dot(cr, 0, a.y); dot(cr, W - 2, a.y);
        if (!k->next) { dot(cr, 0, a.y + a.height - 2); dot(cr, W - 2, a.y + a.height - 2); }
    }
    g_list_free(kids);
    if (have) {
        GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(w));
        double op = gtk_widget_get_opacity(GTK_WIDGET(r));
        if (bar_snap) { bar_y = sy; bar_snap = FALSE; }
        gdk_cairo_set_source_rgba(cr, &(GdkRGBA){ fg.red, fg.green, fg.blue, 0.9 * op });   /* the four corners of the chosen row */
        double C = l_corner;                                   /* four bold dots */
        cairo_rectangle(cr, 0, sy, C, C);             cairo_rectangle(cr, W - C, sy, C, C);
        cairo_rectangle(cr, 0, sy + sh - C, C, C);    cairo_rectangle(cr, W - C, sy + sh - C, C, C);
        cairo_fill(cr);
        double bh = MAX(10.0, sh * l_bar_pct / 100.0);         /* the accent bar: short, in the middle of the left edge, and it slides */
        gdk_cairo_set_source_rgba(cr, &(GdkRGBA){ col_accent.red, col_accent.green, col_accent.blue, op });
        cairo_rectangle(cr, 0, bar_y + (sh - bh) / 2, 3, bh);
        cairo_fill(cr);
    }
    GtkAdjustment *adj = scroll ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)) : NULL;
    if (adj && gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj) > 1) {   /* the scrollbar: a thin square line, only while the list scrolls */
        double age = (g_get_monotonic_time() - sb_t) / 1000.0;
        double a = age < 700 ? 1.0 : age < 1000 ? 1.0 - (age - 700) / 300.0 : 0.0;
        if (a > 0) {
            double val = gtk_adjustment_get_value(adj), pg = gtk_adjustment_get_page_size(adj), up = gtk_adjustment_get_upper(adj);
            double th = MAX(16.0, pg * pg / up), ty = val + (pg - th) * val / (up - pg);
            gdk_cairo_set_source_rgba(cr, &(GdkRGBA){ fg.red, fg.green, fg.blue, 0.18 * a });
            for (double y = val; y < val + pg; y += 4) cairo_rectangle(cr, W - 2, y, 2, 1);
            cairo_fill(cr);
            gdk_cairo_set_source_rgba(cr, &(GdkRGBA){ fg.red, fg.green, fg.blue, 0.6 * a });
            cairo_rectangle(cr, W - 2, ty, 2, th);
            cairo_fill(cr);
        }
    }
    return FALSE;
}
static gboolean sb_tick(GtkWidget *w, GdkFrameClock *fc, gpointer d) {
    (void)fc; (void)d;
    gtk_widget_queue_draw(w);
    if ((g_get_monotonic_time() - sb_t) / 1000.0 > 1050) { sb_ticking = FALSE; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}
static void on_vadj(GtkAdjustment *a, gpointer d) {
    (void)a; (void)d;
    sb_t = g_get_monotonic_time();
    if (!sb_ticking && list) { sb_ticking = TRUE; gtk_widget_add_tick_callback(list, sb_tick, NULL, NULL); }
}

static void retarget_list(gboolean snap) {                  /* the list is as tall as its rows (up to the limit): fewer results, a shorter panel, eased */
    if (!scroll || !shown) return;
    lh_tgt = CLAMP((int)shown->len, 1, 1000) * cur_row_h;
    if (list_h > 0 && lh_tgt > list_h) lh_tgt = list_h;
    if (snap) { lh_cur = lh_tgt; gtk_widget_set_size_request(scroll, -1, (int)lround(lh_cur)); }
    else ui_kick();
}
static gboolean ui_tick(GtkWidget *w, GdkFrameClock *fc, gpointer d) {
    (void)w; (void)fc; (void)d;
    if (!list || !scroll) { ui_ticking = FALSE; return G_SOURCE_REMOVE; }
    gint64 now = g_get_monotonic_time();
    double dt = MIN((now - ui_last) / 1000.0, 34.0);
    ui_last = now;
    gboolean more = FALSE;
    double ty;
    if (sel_rect(&ty, NULL)) {
        if (fabs(ty - bar_y) > 0.5) { bar_y += (ty - bar_y) * (1 - exp(-dt / 28.0)); more = TRUE; }   /* fast first, soft landing; a held key just retargets */
        else bar_y = ty;
        gtk_widget_queue_draw(list);
    }
    if (fabs(lh_tgt - lh_cur) > 0.5) { lh_cur += (lh_tgt - lh_cur) * (1 - exp(-dt / 40.0)); more = TRUE; }
    else lh_cur = lh_tgt;
    gtk_widget_set_size_request(scroll, -1, (int)lround(lh_cur));
    if (!more) { ui_ticking = FALSE; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}
static void ui_kick(void) {
    if (ui_ticking || !list) return;
    ui_ticking = TRUE;
    ui_last = g_get_monotonic_time();
    gtk_widget_add_tick_callback(list, ui_tick, NULL, NULL);
}

static gboolean on_list_motion(GtkWidget *w, GdkEventMotion *e, gpointer d) {   /* the selection follows the mouse, but only when the mouse really moves */
    (void)w; (void)d;
    static double lx = -1, ly = -1;
    if (e->x_root == lx && e->y_root == ly) return FALSE;
    lx = e->x_root; ly = e->y_root;
    GtkListBoxRow *r = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), (int)e->y);
    if (r && r != gtk_list_box_get_selected_row(GTK_LIST_BOX(list))) gtk_list_box_select_row(GTK_LIST_BOX(list), r);
    return FALSE;
}

/* marquee: the selected line, if it does not fit, scrolls once (after a short pause) and rests */
static GtkWidget *mq_row = NULL, *mq_sw = NULL;
static guint mq_timer = 0;
static gint64 mq_t0 = 0;
static void mq_stop(void) {
    if (mq_timer) { g_source_remove(mq_timer); mq_timer = 0; }
    if (mq_row && mq_sw) {                                   /* give the row its ellipsised label back */
        GtkWidget *lb = g_object_get_data(G_OBJECT(mq_row), "lb");
        GtkWidget *hb = g_object_get_data(G_OBJECT(mq_row), "hb");
        if (lb && hb && gtk_widget_get_parent(mq_sw) == hb) {
            gtk_container_remove(GTK_CONTAINER(hb), mq_sw);
            gtk_box_pack_start(GTK_BOX(hb), lb, TRUE, TRUE, 0);
            gtk_box_reorder_child(GTK_BOX(hb), lb, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mq_row), "lbpos")));
            g_object_unref(lb);
        }
    }
    mq_row = mq_sw = NULL;
}
static gboolean mq_step(gpointer d) {
    (void)d;
    if (!mq_sw) { mq_timer = 0; return G_SOURCE_REMOVE; }
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(mq_sw));
    double span = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
    double el = (g_get_monotonic_time() - mq_t0) / 1000.0 - 600;     /* ms after the pause */
    if (el > 0) gtk_adjustment_set_value(adj, MIN(span, el * 0.06));   /* 60 px/s */
    if (span <= 0 || el * 0.06 >= span) { mq_timer = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}
static gboolean mq_start(gpointer d) {
    mq_timer = 0;
    (void)d;
    GtkListBoxRow *r = list ? gtk_list_box_get_selected_row(GTK_LIST_BOX(list)) : NULL;
    if (!r) return G_SOURCE_REMOVE;
    GtkWidget *lb = g_object_get_data(G_OBJECT(r), "lb"), *hb = g_object_get_data(G_OBJECT(r), "hb");
    if (!lb || !hb || gtk_widget_get_parent(lb) != hb) return G_SOURCE_REMOVE;
    if (!pango_layout_is_ellipsized(gtk_label_get_layout(GTK_LABEL(lb)))) return G_SOURCE_REMOVE;   /* it fits */
    GList *kids = gtk_container_get_children(GTK_CONTAINER(hb));
    int pos = g_list_index(kids, lb);
    g_list_free(kids);
    g_object_set_data(G_OBJECT(r), "lbpos", GINT_TO_POINTER(pos));
    g_object_ref(lb);
    gtk_container_remove(GTK_CONTAINER(hb), lb);
    GtkWidget *l2 = gtk_label_new(gtk_label_get_text(GTK_LABEL(lb)));
    gtk_label_set_xalign(GTK_LABEL(l2), 0.0f);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_NONE);
    gtk_container_add(GTK_CONTAINER(sw), l2);
    gtk_box_pack_start(GTK_BOX(hb), sw, TRUE, TRUE, 0);
    gtk_box_reorder_child(GTK_BOX(hb), sw, pos);
    gtk_widget_show_all(sw);
    mq_row = GTK_WIDGET(r); mq_sw = sw;
    mq_t0 = g_get_monotonic_time();
    mq_timer = g_timeout_add(16, mq_step, NULL);
    return G_SOURCE_REMOVE;
}
static void mq_schedule(void) {
    mq_stop();
    mq_timer = g_timeout_add(300, mq_start, NULL);
}

static void on_row_selected(GtkListBox *b, GtkListBoxRow *r, gpointer d) {
    (void)b; (void)r; (void)d;
    ui_kick();
    mq_schedule();
    if (!pmode) return;
    if (pv_timer) g_source_remove(pv_timer);
    pv_timer = g_timeout_add(pmode == 2 ? 70 : 30, pv_fire, NULL);     /* debounce: holding Down must not decode every wallpaper */
}
static void pv_stop(void) {
    if (pv_timer) { g_source_remove(pv_timer); pv_timer = 0; }
    if (pv_cancel) { g_cancellable_cancel(pv_cancel); g_object_unref(pv_cancel); pv_cancel = NULL; }
    pv_gen++;
}

static void panel_resize(int w) {
    gtk_widget_set_size_request(panel, w, -1);
    panel_x = (mon_w - w) / 2;
    gtk_fixed_move(GTK_FIXED(stage), panel, panel_x, panel_y);
}

static void fit_rows(gboolean with_card) {                  /* the list gets the rows that fit under the card (and on the screen) */
    int cardh = 0;
    if (with_card && cardw) gtk_widget_get_preferred_height(cardw, NULL, &cardh);
    else if (!with_card && pmode) cardh = pv_h + 12;           /* the picture takes the card's place */
    static int row_h = 34;                                    /* measured from the first row: your CSS decides how tall a row is */
    GtkListBoxRow *r0 = list ? gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0) : NULL;
    int mh = 0, nh = 0;
    if (r0) { gtk_widget_get_preferred_height(GTK_WIDGET(r0), &mh, &nh); if (nh > 8) row_h = nh; }
    int avail = mon_h - panel_y - cardh - 150;
    list_h = CLAMP(avail, 3 * row_h, rows_cfg * row_h);
    cur_row_h = row_h;
    retarget_list(!intro_done || lh_cur <= 0 || with_card != (pmode == 0));
}

static void enter_picker(int mode) {
    if (dmenu_mode || pmode) return;
    main_items = items;
    items = g_ptr_array_new_with_free_func(item_free);
    if (mode == 1) pick_themes(); else pick_bgs();
    pmode = mode;
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), mode == 1 ? "Theme\xe2\x80\xa6" : "Background\xe2\x80\xa6");
    gtk_widget_show(pvbox);
    if (cardw) gtk_widget_hide(cardw);
    fit_rows(FALSE);
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    list_fill();
    for (guint n = 0; n < shown->len; n++)                    /* start on the current one */
        if (((Item *)g_ptr_array_index(shown, n))->hist) {
            GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), (int)n);
            gtk_list_box_select_row(GTK_LIST_BOX(list), r);
            GtkAllocation a;
            gtk_widget_get_allocation(GTK_WIDGET(r), &a);
            break;
        }
    on_row_selected(NULL, NULL, NULL);
}

static void leave_picker(void) {
    if (!pmode) return;
    pv_stop();
    g_ptr_array_free(items, TRUE);
    items = main_items;
    main_items = NULL;
    pmode = 0;
    if (pvimg) gtk_image_clear(GTK_IMAGE(pvimg));
    gtk_widget_hide(pvbox);
    if (cardw) gtk_widget_show(cardw);
    fit_rows(TRUE);
    panel_resize(panel_w);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), l_placeholder);
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    list_fill();
}

static void move_sel(int d) {
    GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
    int idx = cur ? gtk_list_box_row_get_index(cur) : -1;
    int n = (int)shown->len;
    if (!n) return;
    idx = CLAMP(idx + d, 0, n - 1);
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), idx);
    if (!r) return;
    gtk_list_box_select_row(GTK_LIST_BOX(list), r);
    GtkAllocation a;
    gtk_widget_get_allocation(GTK_WIDGET(r), &a);              /* keep it visible */
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    double top = a.y, bot = a.y + a.height, v = gtk_adjustment_get_value(adj), ps = gtk_adjustment_get_page_size(adj);
    if (top < v) gtk_adjustment_set_value(adj, top);
    else if (bot > v + ps) gtk_adjustment_set_value(adj, bot - ps);
}

static void activate_selected(void) {
    GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
    if (cur) run_item(g_object_get_data(G_OBJECT(cur), "item"));
}

static gboolean on_entry_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
    (void)w; (void)d;
    if (closing) return TRUE;
    last_act = g_get_monotonic_time();
    gboolean ctrl = (e->state & GDK_CONTROL_MASK) != 0;
    switch (e->keyval) {
    case GDK_KEY_Escape: panel_close_soft(); return TRUE;
    case GDK_KEY_BackSpace:
        if (pmode && !*gtk_entry_get_text(GTK_ENTRY(entry))) { leave_picker(); return TRUE; }
        break;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: run_in_term = ctrl; activate_selected(); run_in_term = FALSE; return TRUE;   /* Ctrl+Enter: in a terminal */
    case GDK_KEY_Down: case GDK_KEY_Tab: move_sel(1); return TRUE;
    case GDK_KEY_Up: case GDK_KEY_ISO_Left_Tab: move_sel(-1); return TRUE;
    case GDK_KEY_Page_Down: move_sel(rows_cfg - 1); return TRUE;
    case GDK_KEY_Page_Up: move_sel(-(rows_cfg - 1)); return TRUE;
    case GDK_KEY_n: case GDK_KEY_j: if (ctrl) { move_sel(1); return TRUE; } break;
    case GDK_KEY_p: case GDK_KEY_k: if (ctrl) { move_sel(-1); return TRUE; } break;
    }
    return FALSE;
}

static void on_entry_changed(GtkEditable *e, gpointer d) {
    (void)d;
    last_act = g_get_monotonic_time();
    if (cardw && l_card_hide_typing && !pmode) gtk_widget_set_visible(cardw, !*gtk_entry_get_text(GTK_ENTRY(e)));   /* the results get the room */
    list_fill();
}
static void on_row_activated(GtkListBox *b, GtkListBoxRow *r, gpointer d) {
    (void)b; (void)d;
    run_item(g_object_get_data(G_OBJECT(r), "item"));
}

/* a click outside the panel closes it.
   The event's x/y are relative to the GdkWindow it happened in (the list lives in a viewport window of its own), not to the
   launcher window: only root coordinates minus the window origin can be compared with the panel. */
static gboolean on_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    (void)w; (void)d;
    if (lstate == LS_CLOSED || !panel) return FALSE;
    if (closing) return TRUE;
    last_act = g_get_monotonic_time();
    int ox = 0, oy = 0;
    gdk_window_get_origin(gtk_widget_get_window(host), &ox, &oy);
    double x = e->x_root - ox, y = e->y_root - oy;
    GtkAllocation a;
    gtk_widget_get_allocation(panel, &a);
    int px = 0, py = 0;
    gtk_widget_translate_coordinates(panel, host, 0, 0, &px, &py);
    if (x < px || x > px + a.width || y < py || y > py + a.height) { panel_close_soft(); return TRUE; }
    return FALSE;
}

static void draw_kid(GtkWidget *k, gpointer d) { gpointer *p = d; gtk_container_propagate_draw(GTK_CONTAINER(p[0]), k, p[1]); }

/* while the shape moves, the stage draws it (background + frame, anywhere on the screen); the panel only draws its content,
   clipped to the shape. When the shape has settled, the panel draws itself again. */
static gboolean stage_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    (void)w; (void)d;
    if (mo_on && panel) morph_draw_shape(panel, cr, morph_rect(&mo));
    return FALSE;
}

static gboolean panel_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    (void)d;
    if (p_reveal <= 0.0) return TRUE;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    gpointer p[2] = { w, cr };
    if (mo_on) {
        GtkAllocation a;
        gtk_widget_get_allocation(w, &a);
        MRect r = morph_rect(&mo);
        cairo_save(cr);
        cairo_rectangle(cr, r.x - a.x, r.y - a.y, r.w, r.h);
        cairo_clip(cr);
        if (closing) cairo_push_group(cr);
        gtk_container_forall(GTK_CONTAINER(w), draw_kid, p);
        if (closing) { cairo_pop_group_to_source(cr); cairo_paint_with_alpha(cr, mo_content_a); }
        cairo_restore(cr);
        return TRUE;
    }
    cairo_save(cr);
    if (p_reveal < 1.0) {                                    /* morph off: the box grows from its top right corner */
        cairo_rectangle(cr, W * (1.0 - p_reveal), 0, W * p_reveal, H * p_reveal);
        cairo_clip(cr);
    }
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    gtk_render_background(sc, cr, 0, 0, W, H);
    gtk_render_frame(sc, cr, 0, 0, W, H);
    gtk_container_forall(GTK_CONTAINER(w), draw_kid, p);
    cairo_restore(cr);
    return TRUE;
}

/* every frame of the morph: the indicator row rides on the shape and fades out (in on the way back), a changed panel size retargets */
static gboolean close_idle(gpointer d) { (void)d; closing = FALSE; panel_close(); return G_SOURCE_REMOVE; }
static void mo_frame(gpointer d, gboolean settled) {
    (void)d;
    if (lstate == LS_CLOSED || !panel || !stage) return;
    GtkWidget *row = NO_INDICATOR ? NULL : indicator_row();
    double T = MAX(60, morph_ms()), tm = (g_get_monotonic_time() - mo_t0) / 1000.0 * l_speed;
    MRect r = morph_rect(&mo);
    if (row) {
        double o = closing ? 0.0 : (indicator_row_dim() ? 0.0 : 1.0 - ease_out(tm / (0.35 * T)));   /* no text flashes on the way back: the indicator comes back by itself */
        gtk_widget_set_opacity(row, o);
        gtk_fixed_move(GTK_FIXED(stage), row, (int)(r.x + (r.w - row_w) / 2), (int)r.y);
        if (!closing && o <= 0.0) gtk_widget_hide(row);
    }
    if (closing) {
        mo_content_a = 1.0 - ease_out(tm / (0.3 * T));
        gtk_widget_queue_draw(panel);
        if (settled) g_idle_add(close_idle, NULL);
        return;
    }
    if (lstate != LS_OPEN) return;
    GtkAllocation a;
    gtk_widget_get_allocation(panel, &a);
    if (a.width > 1 && (fabs(mo.t[0] - a.x) > 0.5 || fabs(mo.t[1] - a.y) > 0.5 || fabs(mo.t[2] - (a.x + a.width)) > 0.5 || fabs(mo.t[3] - (a.y + a.height)) > 0.5)) {
        morph_go(&mo, (MRect){ a.x, a.y, a.width, a.height });   /* the content asked for another size meanwhile */
        return;
    }
    if (settled) { mo_on = FALSE; if (row) gtk_widget_hide(row); gtk_widget_queue_draw(stage); gtk_widget_queue_draw(panel); }
}

static void apply_intro(double te) {                         /* te = ms since the panel started to open */
    if (!l_morph) p_reveal = ease_out(te / expand_ms);
    gtk_widget_queue_draw(panel);
    if (intro_done) return;
    gboolean film = cardw && cardw_film;
    double base = expand_ms * 0.3, row0 = expand_ms * 0.5;
    if (film) {                                              /* the card plays its own film first, the search box and the rows follow it */
        base = card_embed_film(MAX(te, 0.0), &film_total, &film_edelay);
        row0 = base + elem_delay;
    }
    int rd = MIN(elem_delay, 12);                            /* the rows follow quickly */
    intro_end = row0 + MIN((int)shown->len, 20) * rd + expand_ms * 0.6;
    gtk_widget_set_opacity(entry, ease_out((te - base) / (expand_ms * 0.6)));
    GList *kids = gtk_container_get_children(GTK_CONTAINER(list));
    int j = 0;
    for (GList *k = kids; k; k = k->next, j++)
        gtk_widget_set_opacity(k->data, ease_out((te - row0 - j * rd) / (expand_ms * 0.6)));
    g_list_free(kids);
}

static void arrive(void) {
    lstate = LS_OPEN;
    if (l_morph) {                                           /* the panel is laid out: the shape goes from the indicator to it */
        GtkAllocation a;
        gtk_widget_get_allocation(panel, &a);
        p_reveal = 1.0;
        mo_delay = morph_ms() * 0.55;                        /* the content comes in when the shape has nearly arrived */
        mo_t0 = g_get_monotonic_time();
        morph_go(&mo, (MRect){ a.x, a.y, a.width, a.height });
    } else if (!NO_INDICATOR) indicator_overlay_arrived();   /* hides the indicator row, window#launcher */
    apply_intro(l_morph ? -mo_delay : 0);                                          /* before the first frame: nothing of the rows / card may show yet */
    gtk_widget_grab_focus(entry);
}

static gboolean tick(GtkWidget *w, GdkFrameClock *fc, gpointer d) {
    (void)w; (void)fc; (void)d;
    if (lstate == LS_CLOSED) { ticking = FALSE; return G_SOURCE_REMOVE; }
    gint64 now = g_get_monotonic_time();
    t_ms += MIN((now - last_us) / 1000.0, 34.0);             /* a slow frame (first show of a big panel) must not skip the animation */
    last_us = now;
    if (lstate == LS_TRAVEL) {                               /* no indicator trip: the panel lays itself out, invisible, until its size stops changing */
        static int warm_h = -1, warm_n = 0;
        if (t_ms < 40) warm_h = -1, warm_n = 0;
        int h = gtk_widget_get_allocated_height(panel), aw = gtk_widget_get_allocated_width(panel);
        if (aw > 1 && aw != panel_w) {                       /* the content asked for more width than [dashboard] width (as it does for the card): follow it */
            GtkBorder pd;
            gtk_style_context_get_padding(gtk_widget_get_style_context(panel), GTK_STATE_FLAG_NORMAL, &pd);
            panel_w = aw;
            preview_w = MAX(160, aw - pd.left - pd.right);
            pv_h = preview_w * 9 / 16;
            gtk_fixed_move(GTK_FIXED(stage), panel, (mon_w - panel_w) / 2, panel_y);
            warm_n = 0;
        }
        if (h == warm_h && h > 1) warm_n++; else warm_n = 0;
        warm_h = h;
        if (warm_n >= 2 || t_ms >= 200) { travel_ms = (int)t_ms; arrive(); }
        return G_SOURCE_CONTINUE;
    }
    double te = (t_ms - travel_ms) * l_speed - (l_morph ? mo_delay : 0);   /* open_speed: 2 = the whole opening twice as fast */
    apply_intro(te);
    double total = expand_ms * 1.1 + (int)shown->len * elem_delay + 40;
    if (cardw && cardw_film) total = MAX(film_total, intro_end) + 120;
    if (te >= total) {
        intro_done = TRUE;
        if (cardw) card_embed_film_end();
        p_reveal = 1.0;
        gtk_widget_set_opacity(entry, 1.0);
        GList *kids = gtk_container_get_children(GTK_CONTAINER(list));
        for (GList *k = kids; k; k = k->next) gtk_widget_set_opacity(k->data, 1.0);
        g_list_free(kids);
        gtk_widget_queue_draw(panel);
        ticking = FALSE;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void panel_close(void) {
    if (lstate == LS_CLOSED) return;
    lstate = LS_CLOSED;
    sb_ticking = FALSE;
    morph_stop(&mo);
    mo_on = FALSE; closing = FALSE;
    if (dmenu_mode) {                                        /* the process ends: print the result and go */
        if (chosen) { g_print("%s\n", chosen); fflush(stdout); }
        exit(chosen ? 0 : 1);
    }
    pv_stop();
    if (idle_id) { g_source_remove(idle_id); idle_id = 0; }
    if (pmode && main_items) { g_ptr_array_free(main_items, TRUE); main_items = NULL; }
    pmode = 0;
    card_embed_free();                                       /* its buttons leave the panel before it is destroyed */
    cardw = NULL;
    if (standalone) {                                        /* the process just ends with its window */
        if (items) { g_ptr_array_free(items, TRUE); items = NULL; }
        gtk_main_quit();
        return;
    }
    indicator_overlay_end(stage);                            /* destroys stage + panel, indicator row returns */
    mq_stop();
    if (dyn) { g_ptr_array_free(dyn, TRUE); dyn = NULL; }
    ui_ticking = FALSE;
    stage = panel = entry = list = scroll = pvbox = pvimg = NULL;
    host = NULL;
    if (items) { g_ptr_array_free(items, TRUE); items = NULL; }
    if (shown) { g_ptr_array_free(shown, TRUE); shown = NULL; }
    if (history) { g_hash_table_destroy(history); history = NULL; }
    malloc_trim(0);
    indicator_recheck();                                     /* empty workspace: the card comes back */
}

/* css: lowest priority, so style.css overrides everything */
static void launcher_css(void) {
    static gboolean done = FALSE;
    if (done) return;
    done = TRUE;
    char *accent = accent_color();
    if (!gdk_rgba_parse(&col_accent, accent)) gdk_rgba_parse(&col_accent, "#e06c75");
    char *css = g_strdup_printf(
        "@define-color accent %s;\n"
        "window#launcher { background-color: transparent; }\n"
        "#lpanel { background-color: alpha(@background, 0.65); padding: 14px 16px; }\n"
        "#lentry { background-image: none; background-color: alpha(@foreground, 0.06); border: none; border-radius: 0; box-shadow: none;"
        "         padding: 8px 10px; min-height: 22px; color: @foreground; caret-color: @foreground; }\n"
        "#llist, #llist row { background-color: transparent; background-image: none; }\n"
        "#llist row { padding: 5px 12px; color: alpha(@foreground, 0.75); }\n"
        /* the selected row: a darker plate; the corner marks and the sliding accent bar are drawn by the list itself */
        "#llist { color: @foreground; }\n"
        "#llist row:selected { color: @foreground; background-color: alpha(black, 0.35); }\n"
        "#lpreview { background-color: alpha(@foreground, 0.05); }\n"
        ".lkind { color: alpha(@foreground, 0.35); font-size: 11px; }\n", accent);
    g_free(accent);
    GtkCssProvider *cp = gtk_css_provider_new();
    gtk_css_provider_load_from_data(cp, css, -1, NULL);
    g_free(css);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void panel_close_soft(void) {                         /* Esc, a click outside, the key again: the shape goes back to the indicator */
    if (closing || lstate == LS_CLOSED) return;
    if (!l_morph || lstate != LS_OPEN || NO_INDICATOR || !panel || !stage) { panel_close(); return; }
    closing = TRUE;
    mo_content_a = 1.0;
    mo_t0 = g_get_monotonic_time();
    if (!mo_on) {                                            /* settled: start from the panel as it is */
        GtkAllocation a;
        gtk_widget_get_allocation(panel, &a);
        morph_jump(&mo, (MRect){ a.x, a.y, a.width, a.height });
        mo_on = TRUE;
    }
    gtk_widget_hide(indicator_row());
    morph_go(&mo, (MRect){ rx0, ry0, row_w, row_h });
}

static void panel_build(void) {
    panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_name(panel, "lpanel");
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "card");
    if (l_alpha > 0) {                                       /* [launcher] alpha: one opacity for the panel and the card in it */
        char ab[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(ab, sizeof ab, "%.3f", CLAMP(l_alpha, 0.0, 1.0));
        char *css = g_strdup_printf("#lpanel { background-color: alpha(@background, %s); }", ab);
        GtkCssProvider *ap = gtk_css_provider_new();
        gtk_css_provider_load_from_data(ap, css, -1, NULL);
        gtk_style_context_add_provider(gtk_widget_get_style_context(panel), GTK_STYLE_PROVIDER(ap), GTK_STYLE_PROVIDER_PRIORITY_USER + 200);
        g_object_unref(ap);
        g_free(css);
    }     /* the very look of the card: your .card CSS and the [dashboard] card keys */
    intro_done = FALSE;                                      /* from the very first layout: rows stay invisible and nothing animates until the opening is over */
    gtk_widget_set_size_request(panel, panel_w, -1);
    {
        GtkBorder pd;
        gtk_style_context_get_padding(gtk_widget_get_style_context(panel), GTK_STATE_FLAG_NORMAL, &pd);
        preview_w = MAX(160, panel_w - pd.left - pd.right);
        pv_h = preview_w * 9 / 16;
    }
    cardw = NULL;
    if (l_card && !dmenu_mode) {                             /* the card sits on top of the search box, as wide as the panel */
        GtkBorder pad;
        gtk_style_context_get_padding(gtk_widget_get_style_context(panel), GTK_STATE_FLAG_NORMAL, &pad);
        int iw = panel_w - pad.left - pad.right;
        GtkWidget *cbox = card_embed_new(iw, l_card_secs, panel_close);
        if (cbox) {                                          /* no clamp: like the plain card, the panel widens to fit its content, so the two are the same width */
            cardw = cbox;
            gtk_box_pack_start(GTK_BOX(panel), cardw, FALSE, FALSE, 0);
            cardw_film = card_embed_expand_ms() > 0;
            if (cardw_film) {                                /* same timing as the card, so the two openings are one thing */
                expand_ms = card_embed_expand_ms();
                card_embed_film(0, &film_total, &film_edelay);
                elem_delay = film_edelay;
            }
        }
    }
    entry = gtk_entry_new();
    gtk_widget_set_name(entry, "lentry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), dmenu_prompt ? dmenu_prompt : l_placeholder);
    g_signal_connect(entry, "key-press-event", G_CALLBACK(on_entry_key), NULL);
    g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), NULL);
    gtk_box_pack_start(GTK_BOX(panel), entry, FALSE, FALSE, 0);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(scroll, "lscroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);   /* no stock scrollbar: list_draw_over draws a thin one */
    g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)), "value-changed", G_CALLBACK(on_vadj), NULL);
    gtk_widget_set_size_request(scroll, -1, rows_cfg * 34);
    list = gtk_list_box_new();
    gtk_widget_set_name(list, "llist");
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_container_add(GTK_CONTAINER(scroll), list);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(body), scroll, TRUE, TRUE, 0);
    pvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(pvbox, "lpreview");
    gtk_widget_set_size_request(pvbox, preview_w, pv_h);
    pvimg = gtk_image_new();
    gtk_widget_set_halign(pvimg, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(pvimg, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(pvimg, TRUE);
    gtk_box_pack_start(GTK_BOX(pvbox), pvimg, TRUE, TRUE, 0);
    gtk_widget_show(pvimg);
    gtk_widget_set_no_show_all(pvbox, TRUE);                 /* only the pickers show it */
    gtk_box_pack_start(GTK_BOX(panel), pvbox, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(panel), pvbox, 0);          /* above the search box, like the card */
    gtk_box_pack_start(GTK_BOX(panel), body, TRUE, TRUE, 0);
    g_signal_connect(list, "row-selected", G_CALLBACK(on_row_selected), NULL);
    g_signal_connect_after(list, "draw", G_CALLBACK(list_draw_over), NULL);
    gtk_widget_add_events(list, GDK_POINTER_MOTION_MASK);
    g_signal_connect(list, "motion-notify-event", G_CALLBACK(on_list_motion), NULL);
    lh_cur = lh_tgt = 0; bar_snap = TRUE;
    g_signal_connect(panel, "draw", G_CALLBACK(panel_draw), NULL);
    shown = g_ptr_array_new();
    gtk_widget_show_all(panel);
    p_reveal = 0.0;                                          /* shown but clipped to nothing: it lays itself out before the opening starts */
}

static void read_cfg(void) {
    rows_cfg = CLAMP(cfg_int("launcher", "rows", 9), 3, 20);
    l_card = cfg_bool("launcher", "card", TRUE);
    int lw = cfg_int("launcher", "width", 0);                 /* unset: as wide as the card, so the card inside is exactly the card */
    panel_w = lw >= 240 ? lw : (l_card && !dmenu_mode) ? MAX(240, cfg_int("dashboard", "width", 380)) : 560;
    panel_top = CLAMP(cfg_int("launcher", "top_percent", 22), 0, 80);
    max_results = CLAMP(cfg_int("launcher", "max_results", 100), 5, 200);
    l_calc = cfg_bool("launcher", "calculator", TRUE);
    l_runner = cfg_bool("launcher", "runner", TRUE);
    l_speed = CLAMP(cfg_dbl("launcher", "open_speed", 1.0), 0.5, 10.0);
    l_corner = CLAMP(cfg_int("launcher", "corner_px", 3), 2, 8);
    l_bar_pct = CLAMP(cfg_int("launcher", "bar_height_percent", 45), 10, 100);
    travel_ms = MAX(0, cfg_int("launcher", "travel_ms", 180));
    expand_ms = MAX(1, cfg_int("launcher", "expand_ms", 120));
    elem_delay = MAX(0, cfg_int("launcher", "element_delay_ms", 18));
    l_enabled = cfg_bool("launcher", "enabled", TRUE);
    char *mode = cfg_str("launcher", "mode", "inline");
    l_separate = !g_ascii_strcasecmp(mode, "separate");
    g_free(mode);
    l_show_kind = cfg_bool("launcher", "show_kind", TRUE);
    l_card_hide_typing = cfg_bool("launcher", "card_hide_when_typing", FALSE);
    l_alpha = cfg_dbl("launcher", "alpha", 0.65);
    g_free(l_card_secs);
    l_card_secs = cfg_str("launcher", "card_sections", "clock,sliders,stats,buttons");
    g_free(l_placeholder);
    l_placeholder = cfg_str("launcher", "placeholder", "Search apps, settings, commands\xe2\x80\xa6");
    l_idle_s = MAX(0, cfg_int("launcher", "idle_timeout_s", 120));
}

static gboolean idle_check(gpointer d) {                     /* a launcher nobody uses closes by itself: the keyboard grab cannot get stuck */
    (void)d;
    if (lstate == LS_CLOSED) { idle_id = 0; return G_SOURCE_REMOVE; }
    if (l_idle_s > 0 && g_get_monotonic_time() - last_act > (gint64)l_idle_s * 1000000) { idle_id = 0; panel_close_soft(); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}
static void idle_start(void) {
    last_act = g_get_monotonic_time();
    if (idle_id) g_source_remove(idle_id);
    idle_id = g_timeout_add_seconds(5, idle_check, NULL);
}

static void start_anim(void) {
    intro_done = FALSE;
    p_reveal = 0.0;
    gtk_widget_set_opacity(entry, 0.0);
    last_us = g_get_monotonic_time();
    ticking = TRUE;
    gtk_widget_add_tick_callback(host, tick, NULL, NULL);
}

/* ================================================================== */
/* In the running barless: the indicator travels to the middle         */
/* ================================================================== */
static int want_mode = 0;                                  /* picker to jump into once opened (barless --launcher themes) */
static char *want_query = NULL;                            /* any other word: prefilled into the search (barless --launcher keybindings) */
/* theme|themes -> 1, background(s)|bg|wallpaper(s) -> 2, anything else -> 0 (and the word becomes the search text) */
static int parse_mode(const char *w) {
    if (!w || !*w) return 0;
    char *l = g_ascii_strdown(w, -1);
    int m = (!strcmp(l, "theme") || !strcmp(l, "themes")) ? 1
          : (!strcmp(l, "background") || !strcmp(l, "backgrounds") || !strcmp(l, "bg") || !strcmp(l, "wallpaper") || !strcmp(l, "wallpapers")) ? 2 : 0;
    g_free(l);
    return m;
}
static void apply_want(void) {
    if (want_mode) { enter_picker(want_mode); want_mode = 0; }
    else if (want_query) { gtk_entry_set_text(GTK_ENTRY(entry), want_query); gtk_editable_set_position(GTK_EDITABLE(entry), -1); }
    g_free(want_query); want_query = NULL;
}
static gboolean open_retry(gpointer d) {
    static int tries = 0;
    if (!card_is_closed() && ++tries < 30) return G_SOURCE_CONTINUE;
    tries = 0;
    launcher_open();
    (void)d;
    return G_SOURCE_REMOVE;
}

void launcher_open(void) {
    if (lstate != LS_CLOSED) return;
    if (!card_is_closed()) {                                  /* the card is up: let it go first */
        card_close_instant();
        g_timeout_add(40, open_retry, NULL);
        return;
    }
    read_cfg();
    launcher_css();
    dmenu_mode = FALSE;
    build_index();
    stage = gtk_fixed_new();
    panel_build();
    host = indicator_overlay_begin(stage, &mon_w, &mon_h, &rx0, &ry0, &row_w, &row_h);
    panel_x = (mon_w - panel_w) / 2;
    panel_y = mon_h * panel_top / 100;
    gtk_fixed_put(GTK_FIXED(stage), panel, panel_x, panel_y);
    fit_rows(TRUE);
    rtx = (mon_w - row_w) / 2;
    rty = panel_y + 10;
    g_signal_connect(host, "button-press-event", G_CALLBACK(on_press), NULL);
    gtk_widget_add_events(host, GDK_BUTTON_PRESS_MASK);
    gtk_widget_show_all(stage);
    l_morph = morph_ms() > 0;
    if (l_morph) {                                             /* the shape starts exactly where the indicator is */
        morph_init(&mo, stage, mo_frame, NULL);
        mo.speed = l_speed;
        g_signal_connect(stage, "draw", G_CALLBACK(stage_draw), NULL);
        indicator_overlay_name();                              /* window#launcher, so #indicator rules do not hit the panel */
        morph_jump(&mo, (MRect){ rx0, ry0, row_w, row_h });
        mo_on = TRUE; closing = FALSE;
    } else indicator_overlay_arrived();                        /* the indicator is simply gone: the box growing from its corner says where it came from */
    list_fill();
    fit_rows(TRUE);
    lstate = LS_TRAVEL;
    t_ms = 0;
    start_anim();
    idle_start();
    apply_want();                                              /* opens straight in the picker / with the search prefilled */
}

void launcher_close(void) { panel_close(); }
/* mode=separate: the launcher is a short-lived process of its own (like --dmenu), so a bug in it cannot touch the indicator */
static GPid sep_pid = 0;
static void sep_exit(GPid p, gint st, gpointer d) {
    (void)st; (void)d;
    g_spawn_close_pid(p);
    if (sep_pid == p) sep_pid = 0;
    indicator_recheck();                                       /* empty workspace: the card comes back */
}
static void toggle_separate(const char *arg) {
    if (sep_pid) { kill(sep_pid, SIGTERM); return; }
    if (!card_is_closed()) card_close();
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    gchar *argv[4] = { exe ? exe : (gchar *)"barless", (gchar *)"--menu", (gchar *)arg, NULL };
    GError *err = NULL;
    GPid pid = 0;
    if (g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, &err)) { sep_pid = pid; g_child_watch_add(pid, sep_exit, NULL); }
    else { g_printerr("barless: cannot start the launcher: %s\n", err->message); g_error_free(err); }
    g_free(exe);
}

/* `barless --launcher themes` leaves its wish in <pidfile>.mode before it sends the signal */
void launcher_toggle(void) {
    char *pf = barless_pidfile(), *mp = g_strconcat(pf, ".mode", NULL);
    char *m = read_line_file(mp);
    unlink(mp);
    g_free(mp); g_free(pf);
    int mode = parse_mode(m);
    read_cfg();
    if (!l_enabled) { g_free(m); return; }                     /* [launcher] enabled=false: the key does nothing */
    if (l_separate) { toggle_separate(m && *m ? m : NULL); g_free(m); return; }
    if (lstate == LS_CLOSED) { want_mode = mode; g_free(want_query); want_query = (!mode && m && *m) ? g_strdup(m) : NULL; launcher_open(); }
    else if (mode && !pmode) enter_picker(mode);               /* already open: jump into the picker */
    else panel_close_soft();
    g_free(m);
}

/* ================================================================== */
/* --dmenu: a process of its own, like walker --dmenu                   */
/* ================================================================== */
static int standalone_run(void);

int launcher_dmenu(int argc, char **argv) {
    dmenu_mode = TRUE;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--prompt")) && i + 1 < argc) dmenu_prompt = g_strdup(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) dmenu_width = atoi(argv[++i]);
        else if ((!strcmp(argv[i], "--height") || !strcmp(argv[i], "--minheight") ||
                  !strcmp(argv[i], "--maxheight") || !strcmp(argv[i], "-c") || !strcmp(argv[i], "-q") || !strcmp(argv[i], "-m")) && i + 1 < argc) i++;   /* walker flags: ignored */
    }
    read_cfg();
    {   /* walker's --width is ignored on purpose: a menu is as wide as the card. [launcher] dmenu_width = N overrides. */
        int dw = cfg_int("launcher", "dmenu_width", 0);
        panel_w = dw >= 200 ? dw : MAX(240, cfg_int("dashboard", "width", 380));
    }
    (void)dmenu_width;
    launcher_css();
    build_index();
    return standalone_run();
}

int launcher_menu(int argc, char **argv) {                    /* barless --menu [themes|backgrounds] */
    standalone = TRUE;
    dmenu_mode = FALSE;
    card_set_standalone();
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (parse_mode(argv[i])) want_mode = parse_mode(argv[i]);
        else { g_free(want_query); want_query = g_strdup(argv[i]); }
    }
    read_cfg();
    launcher_css();
    build_index();
    return standalone_run();
}

static int standalone_run(void) {
    GdkRectangle g = { 0, 0, 1920, 1080 };
    GdkMonitor *m = gdk_display_get_monitor(gdk_display_get_default(), 0);
    if (m) gdk_monitor_get_geometry(m, &g);
    mon_w = g.width; mon_h = g.height;
    host = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(host, "launcher");
    gtk_layer_init_for_window(GTK_WINDOW(host));
    gtk_layer_set_layer(GTK_WINDOW(host), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(host), "barless-menu");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(host), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(host), -1);
    gtk_layer_set_anchor(GTK_WINDOW(host), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(host), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(host), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(host), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    GdkVisual *vis = gdk_screen_get_rgba_visual(gtk_widget_get_screen(host));
    if (vis) gtk_widget_set_visual(host, vis);
    stage = gtk_fixed_new();
    gtk_container_add(GTK_CONTAINER(host), stage);
    panel_build();
    panel_x = (mon_w - panel_w) / 2;
    panel_y = mon_h * panel_top / 100;
    gtk_fixed_put(GTK_FIXED(stage), panel, panel_x, panel_y);
    fit_rows(TRUE);
    g_signal_connect(host, "button-press-event", G_CALLBACK(on_press), NULL);
    gtk_widget_add_events(host, GDK_BUTTON_PRESS_MASK);
    gtk_widget_show_all(host);
    l_morph = morph_ms() > 0;
    if (l_morph) {                                             /* no indicator here: the shape starts as a thin line at the top of the panel */
        morph_init(&mo, stage, mo_frame, NULL);
        mo.speed = l_speed;
        g_signal_connect(stage, "draw", G_CALLBACK(stage_draw), NULL);
        morph_jump(&mo, (MRect){ panel_x + panel_w / 2.0 - 30, panel_y, 60, 4 });
        mo_on = TRUE; closing = FALSE;
    }
    list_fill();
    fit_rows(TRUE);
    lstate = LS_TRAVEL;
    travel_ms = 60;                                            /* no indicator to move: a few frames for the layout to settle (the panel is clipped to nothing meanwhile) */
    t_ms = 0;
    start_anim();
    idle_start();
    apply_want();
    gtk_main();
    return standalone ? 0 : chosen ? 0 : 1;
}
