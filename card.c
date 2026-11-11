/* card.c - the expanded card. THIS IS YOUR PLAYGROUND.
 *
 * One card, shown when the active Hyprland workspace has no windows (the indicator hides while it
 * is up) and animated away when a window appears. It lives in the same process as the indicator:
 * the window is built when it opens and destroyed when it closes, so it costs nothing while you work.
 *
 * Sections, top to bottom (each one is a widget named like this, style it in style.css):
 *   #clock    the big clock (+ the calendar beside it)
 *   #sliders  volume / brightness bars
 *   #stats    cpu / temp / net / battery on one line
 *   #buttons  tray icons + the [button:xxx] groups of config.ini
 * Add a section: build a widget in card_build(), name it, pack it into the card box.
 * Preview without the indicator:   make && ./barless --dashboard */
#include "barless.h"
#include <glib-unix.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <malloc.h>
#include <glob.h>
#include <pango/pangocairo.h>

/* widgets that card_tick() refreshes */
static GtkWidget *w_clock, *w_cpu, *w_temp, *w_net, *w_bat;
static GtkWidget *bgrid = NULL;
static GList *user_buttons = NULL;
static gboolean tray_as_buttons = TRUE;

/* the animation film: anim_now = ms into it, 1e9 = finished (everything fully drawn) */
static int anim_ms = 80, card_delay = 50, elem_delay = 25;
static double anim_now = 1e9, anim_len = 0;
static int anim_dir = 1;                 /* +1 opening, -1 closing */

static GtkAlign align_cfg(const char *key, const char *def) {
    char *v = cfg_str("dashboard", key, def);
    GtkAlign a = GTK_ALIGN_START;
    if (!g_ascii_strcasecmp(v, "center") || !g_ascii_strcasecmp(v, "middle")) a = GTK_ALIGN_CENTER;
    else if (!g_ascii_strcasecmp(v, "right") || !g_ascii_strcasecmp(v, "end")) a = GTK_ALIGN_END;
    g_free(v);
    return a;
}

/* ================================================================== */
/* Buttons: tray icons first, then the [button:xxx] groups              */
/* ================================================================== */
static int btn_size = 34, btn_gap = 6, btn_cols_cfg = 0, card_inner_w = 340;

static void detach(GtkWidget *w) {
    GtkWidget *p = gtk_widget_get_parent(w);
    if (p) gtk_container_remove(GTK_CONTAINER(p), w);
}

static void relayout_button_grid(void) {
    if (!bgrid) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(bgrid));
    for (GList *l = children; l; l = l->next) gtk_container_remove(GTK_CONTAINER(bgrid), GTK_WIDGET(l->data));
    g_list_free(children);

    /* columns: the config value, or as many as fit the card */
    int cols = btn_cols_cfg > 0 ? btn_cols_cfg
             : btn_size > 0 ? MAX(1, (card_inner_w + btn_gap) / (btn_size + btn_gap)) : 6;
    int i = 0;
    if (tray_as_buttons) {
        GList *tw = tray_get_widgets();
        for (GList *l = tw; l; l = l->next) {
            GtkWidget *w = GTK_WIDGET(l->data);
            detach(w);
            if (!gtk_widget_get_visible(w)) continue;
            gtk_grid_attach(GTK_GRID(bgrid), w, i % cols, i / cols, 1, 1);
            gtk_widget_show(w);
            i++;
        }
        g_list_free(tw);
    }
    for (GList *l = user_buttons; l; l = l->next) {
        GtkWidget *w = GTK_WIDGET(l->data);
        detach(w);
        gtk_grid_attach(GTK_GRID(bgrid), w, i % cols, i / cols, 1, 1);
        gtk_widget_show(w);
        i++;
    }
    gtk_widget_show_all(bgrid);
}

static void on_tray_changed(int n) {
    (void)n;
    relayout_button_grid();
}

static void on_button_clicked(GtkButton *b, gpointer cmd) {
    (void)b;
    GError *e = NULL;
    if (!g_spawn_command_line_async((const char *)cmd, &e)) {
        g_printerr("barless: cannot run '%s': %s\n", (const char *)cmd, e->message);
        g_error_free(e);
    }
}
static void free_cmd(gpointer d, GClosure *c) { (void)c; g_free(d); }

/* ================================================================== */
/* Halftone shader, per section                                        */
/* ================================================================== */
typedef struct {
    GtkWidget *w;
    char name[24];
    gboolean ht;               /* halftone applies to this card */
    int dot, fog;              /* this card's own halftone settings (default: the shared ones) */
    double sharp;
    gboolean stagger;
    char **skip;
    int index;                 /* position in the card, for the staggered fade */
    GPtrArray *elems;          /* the things inside that fade in one after another */
} Section;
static GPtrArray *sections = NULL;
static char **ht_skip = NULL;  /* element names / classes the halftone leaves alone */
static char *ht_sections_s = NULL;
static int ht_dot = 5, ht_fog = 6;
static double ht_sharp = 0.0;
static gboolean ht_stagger = TRUE;

static double ease_out(double t) { t = CLAMP(t, 0.0, 1.0); return 1.0 - pow(1.0 - t, 3.0); }

/* box blur along one axis; outside the image counts as 0 so the fog fades out at the edges */
static void blur_pass(const guchar *src, guchar *dst, int W, int H, int r, gboolean horiz) {
    int len = horiz ? W : H, lines = horiz ? H : W, win = 2 * r + 1;
    for (int l = 0; l < lines; l++) {
        int sum = 0;
        for (int k = 0; k <= r && k < len; k++) sum += src[horiz ? l * W + k : k * W + l];
        for (int i = 0; i < len; i++) {
            dst[horiz ? l * W + i : i * W + l] = (guchar)(sum / win);
            int add = i + r + 1, rem = i - r;
            if (add < len) sum += src[horiz ? l * W + add : add * W + l];
            if (rem >= 0)  sum -= src[horiz ? l * W + rem : rem * W + l];
        }
    }
}

typedef struct { GtkWidget *card; cairo_t *cr; } DrawCtx;
static void draw_child(GtkWidget *child, gpointer d) {
    DrawCtx *x = d;
    gtk_container_propagate_draw(GTK_CONTAINER(x->card), child, x->cr);
}

static gboolean is_skipped(GtkWidget *w, char **ht_skip) {
    if (!ht_skip) return FALSE;
    const char *n = gtk_widget_get_name(w);
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    for (char **s = ht_skip; *s; s++) {
        if (!**s) continue;
        if (n && !strcmp(n, *s)) return TRUE;
        if (gtk_style_context_has_class(sc, *s)) return TRUE;
        char alt[64];                                        /* "net" also covers a stats row: .stat-net and #netchart */
        snprintf(alt, sizeof alt, "stat-%s", *s);
        if (gtk_style_context_has_class(sc, alt)) return TRUE;
        snprintf(alt, sizeof alt, "%schart", *s);
        if (n && !strcmp(n, alt)) return TRUE;
    }
    return FALSE;
}
static void find_skipped(GtkWidget *w, GList **out, char **skip) {
    if (is_skipped(w, skip)) { *out = g_list_prepend(*out, w); return; }
    if (GTK_IS_CONTAINER(w)) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList *k = kids; k; k = k->next) find_skipped(k->data, out, skip);
        g_list_free(kids);
    }
}

/* The shader. The card's content is drawn offscreen, blurred into "fog", and re-drawn as a dot grid whose
 * dots grow with the fog and take its colour. Elements on the skip list are cut out and drawn normally on top. */
static void card_halftone(Section *c, cairo_t *cr, int W, int H) {
    int sf = MAX(1, gtk_widget_get_scale_factor(c->w));
    int PW = W * sf, PH = H * sf;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, PW, PH);
    cairo_surface_set_device_scale(s, sf, sf);
    cairo_t *oc = cairo_create(s);
    DrawCtx dc = { c->w, oc };
    gtk_container_forall(GTK_CONTAINER(c->w), draw_child, &dc);
    GList *skips = NULL;
    find_skipped(c->w, &skips, c->skip);
    cairo_set_operator(oc, CAIRO_OPERATOR_CLEAR);
    for (GList *l = skips; l; l = l->next) {
        int x, y;
        if (!gtk_widget_translate_coordinates(l->data, c->w, 0, 0, &x, &y)) continue;
        cairo_rectangle(oc, x, y, gtk_widget_get_allocated_width(l->data), gtk_widget_get_allocated_height(l->data));
        cairo_fill(oc);
    }
    cairo_destroy(oc);
    cairo_surface_flush(s);

    gsize n = (gsize)PW * PH;
    guchar *P[4], *B = g_malloc(n);                         /* planes: A R G B (premultiplied) */
    for (int i = 0; i < 4; i++) P[i] = g_malloc(n);
    const guchar *data = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < PH; y++) {
        const guint32 *row = (const guint32 *)(data + (gsize)y * stride);
        for (int x = 0; x < PW; x++) {
            guint32 v = row[x];
            gsize o = (gsize)y * PW + x;
            P[0][o] = v >> 24; P[1][o] = (v >> 16) & 255; P[2][o] = (v >> 8) & 255; P[3][o] = v & 255;
        }
    }
    if (c->fog > 0) {
        int r = MAX(1, (c->fog * sf + 1) / 2);
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 3; k++) {                   /* 3 box passes per axis ~ gaussian */
                blur_pass(P[i], B, PW, PH, r, TRUE);
                blur_pass(B, P[i], PW, PH, r, FALSE);
            }
    }
    cairo_save(cr);
    cairo_scale(cr, 1.0 / sf, 1.0 / sf);
    double step = c->dot * sf, rowstep = c->stagger ? step * 0.866 : step;
    for (int gy = 0; gy * rowstep < PH; gy++) {
        double y = gy * rowstep + rowstep / 2, xoff = (c->stagger && (gy & 1)) ? step / 2 : 0;
        for (double x = xoff + step / 2; x < PW; x += step) {
            int ix = (int)x, iy = (int)y;
            if (ix >= PW || iy >= PH) continue;
            gsize o = (gsize)iy * PW + ix;
            int a = P[0][o];
            double cov = MIN(1.0, a / 255.0 * 1.5);
            double rad = step * 0.57 * sqrt(cov);
            if (cov < 0.05 || rad < 0.5 * sf) continue;
            cairo_set_source_rgb(cr, MIN(1.0, (double)P[1][o] / a), MIN(1.0, (double)P[2][o] / a),
                                     MIN(1.0, (double)P[3][o] / a));
            cairo_arc(cr, x, y, rad, 0, 2 * G_PI);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
    for (int i = 0; i < 4; i++) g_free(P[i]);
    g_free(B);
    if (c->sharp > 0.01) {                                  /* optional crisp content on top */
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_paint_with_alpha(cr, c->sharp);
    }
    cairo_surface_destroy(s);
    for (GList *l = skips; l; l = l->next) {                /* the untouched elements */
        int x, y;
        if (!gtk_widget_translate_coordinates(l->data, c->w, 0, 0, &x, &y)) continue;
        cairo_save(cr);
        cairo_translate(cr, x, y);
        gtk_widget_draw(l->data, cr);
        cairo_restore(cr);
    }
    g_list_free(skips);
}


/* the card's own widgets: one Section per block, named like it (#clock, #sliders, #stats, #buttons) */
static void section_free(gpointer p) {
    Section *c = p;
    g_strfreev(c->skip);
    if (c->elems) g_ptr_array_free(c->elems, TRUE);
    g_free(c);
}

static gboolean section_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    card_halftone(d, cr, gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    return TRUE;
}

static void section_new(GtkWidget *w, const char *name) {
    if (!sections) sections = g_ptr_array_new_with_free_func(section_free);
    Section *c = g_new0(Section, 1);
    c->w = w;
    g_strlcpy(c->name, name, sizeof c->name);
    c->index = sections->len;
    if (ht_sections_s) {
        char **v = g_strsplit_set(ht_sections_s, ", ", -1);
        for (char **s = v; *s; s++)
            if (**s && (!g_ascii_strcasecmp(*s, "all") || !g_ascii_strcasecmp(*s, name))) c->ht = TRUE;
        g_strfreev(v);
    }
    /* per-section overrides: "<section>.halftone_dot=7" etc. in [dashboard] */
    char k[64];
    #define CK(x) (snprintf(k, sizeof k, "%s.halftone_%s", name, x), k)
    c->dot = CLAMP(cfg_int("dashboard", CK("dot"), ht_dot), 0, 16);
    if (c->dot == 0) c->ht = FALSE;                          /* dot=0: no dots, this section is drawn normally */
    c->fog = CLAMP(cfg_int("dashboard", CK("fog"), ht_fog), 0, 30);
    c->sharp = CLAMP(cfg_dbl("dashboard", CK("sharp"), ht_sharp), 0.0, 1.0);
    c->stagger = cfg_bool("dashboard", CK("stagger"), ht_stagger);
    char *sk = cfg_str("dashboard", CK("skip"), NULL);
    #undef CK
    c->skip = sk ? g_strsplit_set(sk, ", ", -1) : g_strdupv(ht_skip);
    g_free(sk);
    gtk_widget_set_name(w, name);
    g_ptr_array_add(sections, c);
    if (c->ht) g_signal_connect(w, "draw", G_CALLBACK(section_draw), c);
}

/* ================================================================== */
/* The card: grows from its top-right corner, then the contents fade in */
/* ================================================================== */
static GtkWidget *win = NULL, *card_w = NULL;

static double reveal(void) { return anim_now >= 1e8 ? 1.0 : ease_out(anim_now / anim_ms); }

static gboolean card_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    (void)d;
    double e = reveal();
    if (e <= 0.0) return TRUE;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    cairo_save(cr);
    if (e < 1.0) {                                           /* a square growing diagonally: top-right -> bottom-left */
        cairo_rectangle(cr, W * (1.0 - e), 0, W * e, H * e);
        cairo_clip(cr);
    }
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    gtk_render_background(sc, cr, 0, 0, W, H);
    gtk_render_frame(sc, cr, 0, 0, W, H);
    DrawCtx dc = { w, cr };
    gtk_container_forall(GTK_CONTAINER(w), draw_child, &dc);
    cairo_restore(cr);
    return TRUE;
}

/* elements = the leaves inside a section (boxes and grids are walked into; the calendar counts as one) */
static void collect_elems(GtkWidget *w, GPtrArray *out) {
    const char *n = gtk_widget_get_name(w);
    if ((GTK_IS_BOX(w) || GTK_IS_GRID(w) || GTK_IS_FLOW_BOX(w) || GTK_IS_FLOW_BOX_CHILD(w)) && !(n && !strcmp(n, "calendar"))) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList *k = kids; k; k = k->next) collect_elems(k->data, out);
        g_list_free(kids);
    } else g_ptr_array_add(out, w);
}
static GtkWidget *sort_root;
static int elem_cmp(gconstpointer a, gconstpointer b) {
    int ax = 0, ay = 0, bx = 0, by = 0;
    GtkWidget *wa = *(GtkWidget **)a, *wb = *(GtkWidget **)b;
    gtk_widget_translate_coordinates(wa, sort_root, 0, 0, &ax, &ay);
    gtk_widget_translate_coordinates(wb, sort_root, 0, 0, &bx, &by);
    if (ay / 8 != by / 8) return ay / 8 - by / 8;            /* rows first, then right to left like the reveal */
    return bx - ax;
}

static double anim_total(void) {
    double t = 0;
    for (guint i = 0; sections && i < sections->len; i++) {
        Section *c = g_ptr_array_index(sections, i);
        double end = anim_ms * 0.5 + c->index * card_delay + (c->elems ? c->elems->len : 0) * elem_delay + anim_ms * 0.6;
        t = MAX(t, end);
    }
    return MAX(t, (double)anim_ms) + 40;
}

/* push the film into the widgets: the box clip via redraw, the fades via opacity */
static void anim_apply(void) {
    if (card_w) gtk_widget_queue_draw(card_w);
    for (guint i = 0; sections && i < sections->len; i++) {
        Section *c = g_ptr_array_index(sections, i);
        for (guint j = 0; c->elems && j < c->elems->len; j++) {
            double es = anim_ms * 0.5 + c->index * card_delay + j * elem_delay;
            double o = anim_now >= 1e8 ? 1.0 : ease_out((anim_now - es) / (anim_ms * 0.6));
            gtk_widget_set_opacity(g_ptr_array_index(c->elems, j), o);
        }
    }
}

/* ================================================================== */
/* Clock + calendar                                                    */
/* ================================================================== */
/* Clock: strftime formats (the halftone is the card shader above)     */
/* ================================================================== */
static char clock_fmt[96] = "%H:%M";
static GtkAlign clock_al = GTK_ALIGN_START;
static GtkWidget *clock_box = NULL;
static double clock_scale = 1.0;

/* the big clock shrinks (never grows) so it always fits the space it is given */
static void clock_fit(const char *txt) {
    if (!clock_box) return;
    int avail = gtk_widget_get_allocated_width(clock_box) - 4;
    if (avail < 20) return;
    PangoLayout *lay = gtk_widget_create_pango_layout(w_clock, txt);
    int tw, th;
    pango_layout_get_pixel_size(lay, &tw, &th);
    g_object_unref(lay);
    double sc = tw > 0 ? MIN(1.0, (double)avail / tw) : 1.0;
    if (sc < 0.2) sc = 0.2;
    if (fabs(sc - clock_scale) < 0.01) return;
    clock_scale = sc;
    PangoAttrList *al = pango_attr_list_new();
    pango_attr_list_insert(al, pango_attr_scale_new(sc));
    gtk_label_set_attributes(GTK_LABEL(w_clock), al);
    pango_attr_list_unref(al);
}

static void clock_box_allocated(GtkWidget *w, GdkRectangle *a, gpointer d) {
    (void)w; (void)a; (void)d;
    clock_fit(gtk_label_get_text(GTK_LABEL(w_clock)));
}

static void clock_set_text(const char *txt) {
    if (strcmp(txt, gtk_label_get_text(GTK_LABEL(w_clock)))) gtk_label_set_text(GTK_LABEL(w_clock), txt);
    clock_fit(txt);
}


/* ================================================================== */
/* Calendar grid: Su Mo Tu ... + day numbers, sits beside the clock    */
/* ================================================================== */
static GtkWidget *cal_grid = NULL, *cal_cell[7][7];
static int cal_week_start = 0, cal_ymd = -1, cal_px = 0, cal_px_cfg = 0;

static gboolean cal_apply_px(gpointer d) {
    int px = GPOINTER_TO_INT(d);
    PangoAttrList *al = pango_attr_list_new();
    pango_attr_list_insert(al, pango_attr_size_new_absolute(px * PANGO_SCALE));
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 7; c++) gtk_label_set_attributes(GTK_LABEL(cal_cell[r][c]), al);
    pango_attr_list_unref(al);
    return G_SOURCE_REMOVE;
}

/* pick a font size so 7 columns fit the width the layout gave us (it is only 1/N of the card) */
static void cal_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer d) {
    (void)w; (void)d;
    int px = cal_px_cfg > 0 ? cal_px_cfg : CLAMP((int)(a->width / 7 / 1.55), 6, 16);
    if (px == cal_px) return;
    cal_px = px;
    g_idle_add(cal_apply_px, GINT_TO_POINTER(px));           /* not inside the allocation pass */
}

static GtkWidget *calendar_build(void) {
    cal_week_start = g_ascii_strncasecmp(cfg_str("dashboard", "calendar_week_start", "sunday"), "mon", 3) ? 0 : 1;
    cal_px_cfg = cfg_int("dashboard", "calendar_font_px", 0);
    cal_grid = gtk_grid_new();
    gtk_widget_set_name(cal_grid, "calendar");
    gtk_grid_set_column_homogeneous(GTK_GRID(cal_grid), TRUE);
    gtk_grid_set_row_homogeneous(GTK_GRID(cal_grid), TRUE);
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 7; c++) {
            GtkWidget *l = gtk_label_new("");
            gtk_widget_set_halign(l, GTK_ALIGN_CENTER);
            gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_NONE);
            gtk_style_context_add_class(gtk_widget_get_style_context(l), r == 0 ? "cal-head" : "cal-day");
            gtk_grid_attach(GTK_GRID(cal_grid), l, c, r, 1, 1);
            cal_cell[r][c] = l;
        }
    g_signal_connect(cal_grid, "size-allocate", G_CALLBACK(cal_size_allocate), NULL);
    gtk_widget_set_valign(cal_grid, GTK_ALIGN_CENTER);
    return cal_grid;
}

static void calendar_update(const struct tm *now) {
    if (!cal_grid) return;
    int ymd = (now->tm_year + 1900) * 10000 + (now->tm_mon + 1) * 100 + now->tm_mday;
    if (ymd == cal_ymd) return;
    cal_ymd = ymd;
    char b[48];
    for (int c = 0; c < 7; c++) {                             /* two-letter day names, locale aware */
        struct tm t = { 0 };
        t.tm_wday = (cal_week_start + c) % 7;
        strftime(b, sizeof b, "%a", &t);
        char *end = g_utf8_offset_to_pointer(b, MIN(2, (int)g_utf8_strlen(b, -1)));
        *end = '\0';
        gtk_label_set_text(GTK_LABEL(cal_cell[0][c]), b);
    }
    struct tm first = *now;
    first.tm_mday = 1; first.tm_hour = 12; first.tm_isdst = -1;
    mktime(&first);                                           /* fills tm_wday of the 1st */
    int offset = (first.tm_wday - cal_week_start + 7) % 7;
    int dim = g_date_get_days_in_month(now->tm_mon + 1, now->tm_year + 1900);
    for (int i = 0; i < 42; i++) {
        GtkWidget *l = cal_cell[1 + i / 7][i % 7];
        int day = i - offset + 1;
        GtkStyleContext *sc = gtk_widget_get_style_context(l);
        if (day >= 1 && day <= dim) { snprintf(b, sizeof b, "%d", day); gtk_label_set_text(GTK_LABEL(l), b); }
        else gtk_label_set_text(GTK_LABEL(l), "");
        if (day == now->tm_mday) gtk_style_context_add_class(sc, "cal-today");
        else gtk_style_context_remove_class(sc, "cal-today");
    }
}


/* ================================================================== */
/* Sliders: volume + brightness bars (icon, bar, number)               */
/* ================================================================== */
typedef struct {
    const char *name;                       /* "volume" | "brightness" */
    GtkWidget *scale, *val, *icon;
    char *get_cmd, *set_cmd, *mute_cmd;     /* NULL = built-in default */
    char *icon_s, *icon_muted_s;
    GtkWidget *icon_btn;
    int icon_th, val_th;                                   /* percent from which the fill is under the icon / the number */
    int percent, min;
    gboolean muted, updating, dragging, ok;
    guint flush_id;
    gint64 last_set;                        /* ms: do not poll over a value we just set */
} Slider;

static Slider *sliders[2];                              /* the ones in the open card */
static int n_sliders = 0;
static Slider *pool[2];                                 /* kept between opens: only their widgets are rebuilt */
static int n_pool = 0;
static guint slider_poll_id = 0;

static char *fmt_pct(const char *tmpl, int pct) {       /* %d -> number, %% -> % */
    char num[16];
    snprintf(num, sizeof num, "%d", pct);
    char **parts = g_strsplit(tmpl, "%d", -1);
    char *j = g_strjoinv(num, parts);
    g_strfreev(parts);
    parts = g_strsplit(j, "%%", -1);
    char *o = g_strjoinv("%", parts);
    g_strfreev(parts);
    g_free(j);
    return o;
}

static char *fmt_pct(const char *tmpl, int pct);
static char *slider_fmt = NULL;                          /* [dashboard] slider_format, default "%d%%" */
static void slider_class(GtkWidget *w, const char *cls, gboolean on) {
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    if (on) gtk_style_context_add_class(sc, cls); else gtk_style_context_remove_class(sc, cls);
}
static void slider_ui(Slider *s) {
    char *b;
    if (!s->ok) b = g_strdup("n/a");
    else if (s->muted) b = g_strdup("mute");
    else b = fmt_pct(slider_fmt ? slider_fmt : "%d%%", s->percent);
    gtk_label_set_text(GTK_LABEL(s->val), b);
    g_free(b);
    /* the icon sits at the start of the bar and the number at the end: tell the CSS when the fill is under them */
    slider_class(s->icon_btn, "filled", s->ok && s->percent >= s->icon_th);
    slider_class(s->val, "filled", s->ok && !s->muted && s->percent >= s->val_th);
    gtk_label_set_text(GTK_LABEL(s->icon), s->muted && s->icon_muted_s ? s->icon_muted_s : s->icon_s);
    gtk_widget_set_sensitive(s->scale, s->ok);
    s->updating = TRUE;
    gtk_range_set_value(GTK_RANGE(s->scale), s->percent);
    s->updating = FALSE;
}

/* wpctl prints "Volume: 0.45" (+ " [MUTED]"); anything else is read as a plain percent */
static gboolean parse_percent(const char *out, int *pct, gboolean *muted) {
    if (!out) return FALSE;
    const char *v = strstr(out, "Volume:");
    char *end;
    double d;
    if (v) { d = g_ascii_strtod(v + 7, &end); if (end == v + 7) return FALSE; *pct = (int)(d * 100 + 0.5); }
    else {
        const char *p = out;
        while (*p && !g_ascii_isdigit(*p)) p++;
        if (!*p) return FALSE;
        d = g_ascii_strtod(p, &end);
        *pct = (int)(d + 0.5);
    }
    *muted = strstr(out, "[MUTED]") != NULL;
    *pct = CLAMP(*pct, 0, 150);
    return TRUE;
}

static void slider_apply(Slider *s, gboolean ok, int pct, gboolean muted) {
    if (!s->scale) return;                                 /* the card was closed while this was running */
    if (s->dragging || g_get_monotonic_time() / 1000 - s->last_set < 1500) return;   /* the user is in control */
    s->ok = ok; s->muted = muted;
    if (ok) s->percent = pct;
    slider_ui(s);
}

static void on_read_done(GObject *src, GAsyncResult *res, gpointer data) {
    Slider *s = data;
    char *out = NULL;
    GError *e = NULL;
    int pct = 0;
    gboolean muted = FALSE, ok = FALSE;
    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, &e))
        ok = parse_percent(out, &pct, &muted);
    if (e) g_error_free(e);
    g_free(out);
    slider_apply(s, ok, pct, muted);
    g_object_unref(src);
}

static int backlight_percent(void) {                    /* free: plain sysfs, no process */
    char pat[256];
    snprintf(pat, sizeof pat, "%s/class/backlight/*", sysfs_root());
    glob_t g;
    int pct = -1;
    if (glob(pat, 0, NULL, &g) == 0 && g.gl_pathc) {
        char a[400], b[400], *ta = NULL, *tb = NULL;
        snprintf(a, sizeof a, "%s/brightness", g.gl_pathv[0]);
        snprintf(b, sizeof b, "%s/max_brightness", g.gl_pathv[0]);
        if (g_file_get_contents(a, &ta, NULL, NULL) && g_file_get_contents(b, &tb, NULL, NULL)) {
            double cur = atof(ta), max = atof(tb);
            if (max > 0) pct = (int)(cur * 100.0 / max + 0.5);
        }
        g_free(ta); g_free(tb);
    }
    globfree(&g);
    return pct;
}

static void slider_read(Slider *s) {
    if (!strcmp(s->name, "brightness") && !s->get_cmd) {
        int p = backlight_percent();
        slider_apply(s, p >= 0, p, FALSE);
        return;
    }
    const char *cmd = s->get_cmd ? s->get_cmd : "wpctl get-volume @DEFAULT_AUDIO_SINK@";
    gchar **argv = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &argv, NULL)) return;
    GSubprocess *p = g_subprocess_newv((const gchar *const *)argv,
                                       G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    g_strfreev(argv);
    if (!p) { slider_apply(s, FALSE, 0, FALSE); return; }
    g_subprocess_communicate_utf8_async(p, NULL, NULL, on_read_done, s);
}

static void run_cmd(const char *cmd) {
    GError *e = NULL;
    if (!g_spawn_command_line_async(cmd, &e)) {
        g_printerr("barless: cannot run '%s': %s\n", cmd, e->message);
        g_error_free(e);
    }
}

static gboolean slider_flush(gpointer d) {              /* coalesced: at most one command per 60 ms of dragging */
    Slider *s = d;
    s->flush_id = 0;
    const char *t = s->set_cmd ? s->set_cmd
                  : !strcmp(s->name, "volume") ? "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%"
                                               : "brightnessctl set %d%%";
    char *c = fmt_pct(t, s->percent);
    run_cmd(c);
    g_free(c);
    return G_SOURCE_REMOVE;
}

static void on_slider_changed(GtkRange *r, gpointer d) {
    Slider *s = d;
    if (s->updating) return;
    s->percent = MAX(s->min, (int)gtk_range_get_value(r));
    s->muted = FALSE;
    s->last_set = g_get_monotonic_time() / 1000;
    char b[16];
    snprintf(b, sizeof b, "%d%%", s->percent);
    gtk_label_set_text(GTK_LABEL(s->val), b);
    gtk_label_set_text(GTK_LABEL(s->icon), s->icon_s);
    if (!s->flush_id) s->flush_id = g_timeout_add(60, slider_flush, s);
}

static gboolean on_slider_press(GtkWidget *w, GdkEventButton *e, gpointer d) { (void)w; (void)e; ((Slider *)d)->dragging = TRUE; return FALSE; }
static gboolean on_slider_release(GtkWidget *w, GdkEventButton *e, gpointer d) { (void)w; (void)e; ((Slider *)d)->dragging = FALSE; return FALSE; }

static gboolean after_mute(gpointer d) { slider_read(d); return G_SOURCE_REMOVE; }
static void on_icon_clicked(GtkButton *b, gpointer d) {
    (void)b;
    Slider *s = d;
    if (strcmp(s->name, "volume")) return;               /* the brightness icon is just an icon */
    run_cmd(s->mute_cmd ? s->mute_cmd : "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
    s->last_set = 0;
    g_timeout_add(200, after_mute, s);
}

static gboolean slider_poll(gpointer d) {
    (void)d;
    for (int i = 0; i < n_sliders; i++) slider_read(sliders[i]);
    return G_SOURCE_CONTINUE;
}

/* a one-word option: tolerates a trailing "# comment" (key files have no inline comments) and capitals */
static char *cfg_word(const char *key, const char *def) {
    char *v = cfg_str("dashboard", key, def);
    char *e = v + strcspn(v, " \t#");
    *e = '\0';
    char *l = g_ascii_strdown(v, -1);
    g_free(v);
    return l;
}

/* start / center / end run along the bar in the direction it fills (left to right) */
static void place_on_bar(GtkWidget *w, const char *pos) {
    if (!g_ascii_strcasecmp(pos, "hide") || !g_ascii_strcasecmp(pos, "none")) { gtk_widget_set_no_show_all(w, TRUE); gtk_widget_hide(w); return; }
    gtk_widget_set_halign(w, !g_ascii_strcasecmp(pos, "center") ? GTK_ALIGN_CENTER
                           : !g_ascii_strcasecmp(pos, "end") ? GTK_ALIGN_END : GTK_ALIGN_START);
    gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
}

static Slider *slider_new(const char *name, GtkWidget *box) {
    Slider *s = NULL;
    for (int i = 0; i < n_pool; i++) if (!strcmp(pool[i]->name, name)) s = pool[i];
    if (!s) {
        s = g_new0(Slider, 1);
        s->name = name;
        char k[48];
        #define KEY(x) (snprintf(k, sizeof k, "%s_%s", name, x), k)
        s->get_cmd = cfg_str("dashboard", KEY("get"), NULL);
        s->set_cmd = cfg_str("dashboard", KEY("set"), NULL);
        s->mute_cmd = cfg_str("dashboard", KEY("mute"), NULL);
        gboolean vol = !strcmp(name, "volume");
        s->icon_s = cfg_str("dashboard", KEY("icon"), vol ? "\xf3\xb0\x95\xbe" : "\xf3\xb0\x83\x9f");        /* U+F057E / U+F00DF */
        s->icon_muted_s = vol ? cfg_str("dashboard", KEY("icon_muted"), "\xf3\xb0\x96\x81") : NULL;          /* U+F0581 */
        #undef KEY
        s->min = vol ? 0 : 1;                              /* brightness never goes fully dark */
        pool[n_pool++] = s;
    }
    s->flush_id = 0; s->dragging = FALSE; s->updating = FALSE;

    s->icon = gtk_label_new(s->icon_s);
    GtkWidget *ib = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(ib), s->icon);
    gtk_style_context_add_class(gtk_widget_get_style_context(ib), "slider-icon");
    gtk_widget_set_can_focus(ib, FALSE);
    s->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, s->min, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(s->scale), FALSE);
    gtk_range_set_increments(GTK_RANGE(s->scale), 2, 5);
    gtk_widget_set_name(s->scale, name);                   /* #volume, #brightness */
    gtk_style_context_add_class(gtk_widget_get_style_context(s->scale), "slider");
    s->val = gtk_label_new("-");
    gtk_style_context_add_class(gtk_widget_get_style_context(s->val), "slider-val");
    /* icon and number are drawn on the bar itself */
    s->icon_btn = ib;
    GtkWidget *ov = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(ov), s->scale);
    char *ip = cfg_word("slider_icon_pos", "start");
    char *vp = cfg_word("slider_value_pos", "end");
    s->icon_th = !g_ascii_strcasecmp(ip, "center") ? 50 : !g_ascii_strcasecmp(ip, "end") ? 85 : 15;
    s->val_th = !g_ascii_strcasecmp(vp, "center") ? 50 : !g_ascii_strcasecmp(vp, "start") ? 15 : 85;
    place_on_bar(ib, ip);
    place_on_bar(s->val, vp);
    g_free(ip); g_free(vp);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), s->val);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(ov), s->val, TRUE);   /* dragging works through the number */
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), ib);
    gtk_widget_set_hexpand(ov, TRUE);
    gtk_box_pack_start(GTK_BOX(box), ov, TRUE, TRUE, 0);
    g_signal_connect(s->scale, "value-changed", G_CALLBACK(on_slider_changed), s);
    g_signal_connect(s->scale, "button-press-event", G_CALLBACK(on_slider_press), s);
    g_signal_connect(s->scale, "button-release-event", G_CALLBACK(on_slider_release), s);
    g_signal_connect(ib, "clicked", G_CALLBACK(on_icon_clicked), s);
    return s;
}

/* [dashboard] sliders=volume,brightness ; a slider whose tool/device is missing is left out */
static GtkWidget *sliders_build(void) {
    char *list = cfg_str("dashboard", "sliders", "volume,brightness");
    char **names = g_strsplit_set(list, ", ", -1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, MAX(0, cfg_int("dashboard", "slider_gap", 8)));
    gtk_box_set_homogeneous(GTK_BOX(box), TRUE);
    g_free(slider_fmt);
    slider_fmt = cfg_str("dashboard", "slider_format", "%d%%");
    n_sliders = 0;
    for (char **n = names; *n; n++) {
        if (!**n) continue;
        const char *name = !g_ascii_strcasecmp(*n, "volume") ? "volume" : !g_ascii_strcasecmp(*n, "brightness") ? "brightness" : NULL;
        if (!name || n_sliders >= 2) continue;
        char k[48];
        snprintf(k, sizeof k, "%s_get", name);
        char *custom = cfg_str("dashboard", k, NULL);
        gboolean have = custom != NULL;
        g_free(custom);
        if (!have && !strcmp(name, "volume")) {
            char *w = g_find_program_in_path("wpctl");
            have = w != NULL;
            g_free(w);
        } else if (!have) have = backlight_percent() >= 0;          /* a laptop panel exists */
        if (!have) continue;
        sliders[n_sliders++] = slider_new(name, box);
    }
    g_strfreev(names);
    g_free(list);
    if (!n_sliders) { gtk_widget_destroy(box); return NULL; }
    for (int i = 0; i < n_sliders; i++) slider_read(sliders[i]);
    slider_poll_id = g_timeout_add_seconds(2, slider_poll, NULL);
    gtk_widget_set_name(box, "sliders");
    return box;
}

/* the card is closing: stop the timers; the widgets die with the window */
static void sliders_stop(void) {
    if (slider_poll_id) { g_source_remove(slider_poll_id); slider_poll_id = 0; }
    for (int i = 0; i < n_sliders; i++) {
        Slider *s = sliders[i];
        if (s->flush_id) { g_source_remove(s->flush_id); s->flush_id = 0; }
        s->scale = s->val = s->icon = s->icon_btn = NULL;
        s->dragging = FALSE;
    }
    n_sliders = 0;
}
/* accent colour for highlights: [dashboard] accent=, else the Omarchy theme's colors.toml, else the foreground */
static char *accent_color(void) {
    char *c = cfg_str("dashboard", "accent", NULL);
    if (c && *c) return c;
    g_free(c);
    char *path = g_build_filename(g_get_home_dir(), ".config/omarchy/current/theme/colors.toml", NULL);
    char *txt = NULL;
    char *res = NULL;
    if (g_file_get_contents(path, &txt, NULL, NULL)) {
        char **lines = g_strsplit(txt, "\n", -1);
        for (char **l = lines; *l && !res; l++) {
            const char *q = g_strstrip(*l);
            if (!g_str_has_prefix(q, "accent")) continue;
            const char *a = strpbrk(q, "\"'");
            if (!a) continue;
            const char *b = strpbrk(a + 1, "\"'");
            if (b && b > a + 1) res = g_strndup(a + 1, b - a - 1);
        }
        g_strfreev(lines);
    }
    g_free(txt);
    g_free(path);
    return res ? res : g_strdup("@foreground");
}

/* ================================================================== */
/* ====================== EDIT HERE: the layout ===================== */
/* ================================================================== */

static GtkWidget *clock_build(void) {
    char *f = cfg_str("dashboard", "time_format", "%H:%M");
    g_strlcpy(clock_fmt, f, sizeof clock_fmt);
    g_free(f);
    clock_al = align_cfg("clock_align", "left");
    clock_scale = 1.0;
    cal_ymd = -1;

    GtkWidget *tbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    w_clock = gtk_label_new("--:--");
    gtk_widget_set_name(w_clock, "bigclock");
    gtk_widget_set_halign(w_clock, clock_al);
    gtk_label_set_justify(GTK_LABEL(w_clock), clock_al == GTK_ALIGN_CENTER ? GTK_JUSTIFY_CENTER
                          : clock_al == GTK_ALIGN_END ? GTK_JUSTIFY_RIGHT : GTK_JUSTIFY_LEFT);
    gtk_label_set_ellipsize(GTK_LABEL(w_clock), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(w_clock, 1, -1);           /* its width comes from the column, clock_fit() scales the text */
    gtk_widget_set_hexpand(w_clock, TRUE);
    int cdot = CLAMP(cfg_int("dashboard", "clock.halftone_dot", ht_dot), 2, 16);
    gtk_widget_set_margin_top(w_clock, cdot);              /* room for the halftone fog */
    gtk_widget_set_margin_bottom(w_clock, cdot);
    clock_box = tbox;
    g_signal_connect(tbox, "size-allocate", G_CALLBACK(clock_box_allocated), NULL);
    gtk_box_pack_start(GTK_BOX(tbox), w_clock, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(tbox, TRUE);
    if (!cfg_bool("dashboard", "calendar", TRUE)) return tbox;

    /* the calendar gets exactly 1/N of the card (N = calendar_fraction, 3 or 4), the clock the rest */
    int n = CLAMP(cfg_int("dashboard", "calendar_fraction", 3), 2, 6);
    char *side = cfg_str("dashboard", "calendar_side", "right");
    gboolean left = !g_ascii_strcasecmp(side, "left");
    g_free(side);
    GtkWidget *split = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(split), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(split), 12);
    GtkWidget *cal = calendar_build();
    gtk_widget_set_hexpand(cal, TRUE);
    gtk_grid_attach(GTK_GRID(split), tbox, left ? 1 : 0, 0, n - 1, 1);
    gtk_grid_attach(GTK_GRID(split), cal, left ? 0 : n - 1, 0, 1, 1);
    return split;
}

/* ================================================================== */
/* Stats on one line: cpu 12%  temp 54°C  ▼ 0 B/s  ▲ 0 B/s            */
/* ================================================================== */
/* Nerd Font glyph in front of cpu / temp / battery; [dashboard] cpu_icon= temp_icon= battery_icon= (empty = the word) */
static char *stat_icon(const char *key, const char *def_icon, const char *word) {
    char opt[32];
    snprintf(opt, sizeof opt, "%s_icon", key);
    char *ic = cfg_str("dashboard", opt, def_icon);
    if (!*ic) { g_free(ic); ic = g_strdup(word); }
    return ic;
}

/* fixed width: at most 4 characters (0B 12K 1.5M 999M), so the line never changes width */
static void fmt_rate(double bps, char *out, size_t len) {
    static const char u[] = "BKMG";
    int i = 0;
    double v = bps < 0 ? 0 : bps;
    while (v >= 999.5 && i < 3) { v /= 1024.0; i++; }
    if (i > 0 && v < 9.95) snprintf(out, len, "%.1f%c", v, u[i]);
    else snprintf(out, len, "%.0f%c", v, u[i]);
}

static GtkWidget *stat_item(GtkWidget *flow, const char *key, const char *label, GtkWidget **val) {
    GtkWidget *item = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    char cls[48];
    snprintf(cls, sizeof cls, "stat-%s", key);               /* .stat-net, .stat-cpu ... (also what halftone_skip=net matches) */
    if (label && *label) {
        GtkWidget *k = gtk_label_new(label);
        gtk_style_context_add_class(gtk_widget_get_style_context(k), "stat-key");
        gtk_style_context_add_class(gtk_widget_get_style_context(k), cls);
        gtk_box_pack_start(GTK_BOX(item), k, FALSE, FALSE, 0);
    }
    *val = gtk_label_new("-");
    gtk_label_set_xalign(GTK_LABEL(*val), 0.0f);
    gtk_label_set_width_chars(GTK_LABEL(*val), !strcmp(key, "net") ? 12 : !strcmp(key, "battery") ? 12 : !strcmp(key, "temp") ? 3 : 4);   /* constant width: no jumping */
    gtk_style_context_add_class(gtk_widget_get_style_context(*val), "stat-val");
    gtk_style_context_add_class(gtk_widget_get_style_context(*val), cls);
    gtk_box_pack_start(GTK_BOX(item), *val, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(flow), item, FALSE, FALSE, 0);
    return item;
}

/* [dashboard] stats=cpu,temp,net,battery  (order = order shown, leave one out to hide it) */
static GtkWidget *stats_build(void) {
    GtkWidget *flow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, MAX(0, cfg_int("dashboard", "stats_gap", 16)));   /* one line */
    gtk_widget_set_halign(flow, align_cfg("stats_align", "left"));
    w_cpu = w_temp = w_net = w_bat = NULL;
    char *stats_s = cfg_str("dashboard", "stats", "cpu,temp,net,battery");
    char **stats = g_strsplit_set(stats_s, ", ", -1);
    int n = 0;
    for (char **st = stats; *st; st++) {
        if (!**st) continue;
        char *ic = NULL;
        if (!g_ascii_strcasecmp(*st, "cpu") && !w_cpu) { ic = stat_icon("cpu", "\xef\x92\xbc", "cpu"); stat_item(flow, "cpu", ic, &w_cpu); n++; }
        else if (!g_ascii_strcasecmp(*st, "temp") && !w_temp) { ic = stat_icon("temp", "\xef\x8b\x89", "temp"); stat_item(flow, "temp", ic, &w_temp); n++; }
        else if (!g_ascii_strcasecmp(*st, "battery") && !w_bat) { ic = stat_icon("battery", "\xf3\xb0\x81\xb9", "bat"); stat_item(flow, "battery", ic, &w_bat); n++; }
        else if (!g_ascii_strcasecmp(*st, "net") && !w_net) { stat_item(flow, "net", NULL, &w_net); n++; }
        g_free(ic);
    }
    g_strfreev(stats);
    g_free(stats_s);
    if (!n) { gtk_widget_destroy(flow); return NULL; }
    return flow;
}

/* ================================================================== */
/* Buttons: one [button:xxx] group in config.ini each                  */
/* ================================================================== */
static GtkWidget *buttons_build(void) {
    tray_as_buttons = cfg_bool("dashboard", "tray_buttons", TRUE);
    bgrid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(bgrid), btn_gap);
    gtk_grid_set_column_spacing(GTK_GRID(bgrid), btn_gap);
    gtk_widget_set_halign(bgrid, align_cfg("button_align", "left"));
    gboolean show_labels = cfg_bool("dashboard", "show_labels", FALSE);
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
            gtk_style_context_add_class(gtk_widget_get_style_context(b), "action");
            gtk_widget_set_name(b, *g + 7);                       /* #menu, #wifi, #shutdown ... */
            gtk_widget_set_tooltip_text(b, tip ? tip : (*label ? label : *g + 7));
            gtk_widget_set_can_focus(b, FALSE);
            g_signal_connect_data(b, "clicked", G_CALLBACK(on_button_clicked), run, free_cmd, 0);
            g_object_ref_sink(b);
            user_buttons = g_list_append(user_buttons, b);
        }
        g_free(label); g_free(icon); g_free(cmd); g_free(tip);
    }
    g_strfreev(groups);
    relayout_button_grid();
    if (!user_buttons && !tray_get_widgets()) return NULL;
    return bgrid;
}

/* ================================================================== */
/* The card                                                            */
/* ================================================================== */
static void card_build(GtkWidget *card) {
    GtkWidget *w = clock_build();
    gtk_box_pack_start(GTK_BOX(card), w, FALSE, FALSE, 0);
    section_new(w, "clock");
    if ((w = sliders_build())) { gtk_box_pack_start(GTK_BOX(card), w, FALSE, FALSE, 0); section_new(w, "sliders"); }
    if ((w = stats_build())) { gtk_box_pack_start(GTK_BOX(card), w, FALSE, FALSE, 0); section_new(w, "stats"); }
    if ((w = buttons_build())) { gtk_box_pack_start(GTK_BOX(card), w, FALSE, FALSE, 0); section_new(w, "buttons"); }
}

static int stat_every = 5;

/* once per second: the clock; every `interval` seconds: the stats */
static gboolean card_tick(gpointer d) {
    static int n = 0;
    gboolean first = d != NULL;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char b[160];
    if (!strftime(b, sizeof b, clock_fmt, &tm)) b[0] = '\0';
    clock_set_text(b);
    calendar_update(&tm);
    if (!first && ++n % stat_every) return G_SOURCE_CONTINUE;

    if (w_cpu) { snprintf(b, sizeof b, "%d%%", cpu_read()); gtk_label_set_text(GTK_LABEL(w_cpu), b); }
    if (w_temp) {
        int tv = temp_read();
        if (tv >= 0) snprintf(b, sizeof b, "%d\xc2\xb0", tv); else snprintf(b, sizeof b, "n/a");
        gtk_label_set_text(GTK_LABEL(w_temp), b);
    }
    if (w_net) {
        double down = 0, up = 0;
        char ds[32], us[32];
        net_read(&down, &up);
        fmt_rate(down, ds, sizeof ds);
        fmt_rate(up, us, sizeof us);
        snprintf(b, sizeof b, "\xe2\x96\xbc %-4s \xe2\x96\xb2 %-4s", ds, us);   /* down = download, up = upload */
        gtk_label_set_text(GTK_LABEL(w_net), b);
    }
    if (w_bat) {
        BatInfo bat;
        bat_read(&bat);
        if (!bat.present) snprintf(b, sizeof b, "none");
        else if (bat.charging) snprintf(b, sizeof b, "%d%% \xe2\x96\xb2 %.1fW", bat.cap, bat.watts);
        else if (bat.discharging) snprintf(b, sizeof b, "%d%% \xe2\x96\xbc %.1fW", bat.cap, bat.watts);
        else snprintf(b, sizeof b, "%d%%", bat.cap);
        gtk_label_set_text(GTK_LABEL(w_bat), b);
        GtkStyleContext *sc = gtk_widget_get_style_context(w_bat);
        gtk_style_context_remove_class(sc, "ok");
        gtk_style_context_remove_class(sc, "mid");
        gtk_style_context_remove_class(sc, "low");
        if (bat.present) gtk_style_context_add_class(sc, bat.cap <= 20 ? "low" : bat.cap <= 50 ? "mid" : "ok");
    }
    return G_SOURCE_CONTINUE;
}

/* ---------- open / close ---------- */
typedef enum { CS_CLOSED, CS_OPENING, CS_OPEN, CS_CLOSING } CardState;
static CardState cstate = CS_CLOSED;
static guint tick_src = 0, begin_src = 0, finish_src = 0;
static gboolean tick_running = FALSE, want_reopen = FALSE;
static gint64 last_us = 0;
static int c_width = 380, c_margin = 12, c_margin_top = 12, c_interval = 5;

static void card_open_now(void);

static gboolean card_finish(gpointer d) {                  /* the closing film ended: take everything down */
    (void)d;
    finish_src = 0;
    if (cstate != CS_CLOSING) return G_SOURCE_REMOVE;
    if (tick_src) { g_source_remove(tick_src); tick_src = 0; }
    if (begin_src) { g_source_remove(begin_src); begin_src = 0; }
    sliders_stop();
    /* tray buttons outlive the card (the tray keeps them); detach before the window goes */
    GList *tw = tray_get_widgets();
    for (GList *l = tw; l; l = l->next) detach(GTK_WIDGET(l->data));
    g_list_free(tw);
    for (GList *l = user_buttons; l; l = l->next) detach(GTK_WIDGET(l->data));
    g_list_free_full(user_buttons, g_object_unref);
    user_buttons = NULL;
    indicator_collapse(card_w);                            /* the same window turns back into the indicator */
    card_w = bgrid = NULL;
    w_clock = w_cpu = w_temp = w_net = w_bat = clock_box = NULL;
    cal_grid = NULL;
    if (sections) { g_ptr_array_free(sections, TRUE); sections = NULL; }
    cstate = CS_CLOSED;
    malloc_trim(0);                                        /* give the card's memory back */
    if (want_reopen) { want_reopen = FALSE; card_open_now(); }
    return G_SOURCE_REMOVE;
}

static gboolean anim_tick(GtkWidget *w, GdkFrameClock *fc, gpointer d) {
    (void)w; (void)fc; (void)d;
    if (cstate != CS_OPENING) { tick_running = FALSE; return G_SOURCE_REMOVE; }   /* closed meanwhile */
    gint64 now = g_get_monotonic_time();
    double dt = (now - last_us) / 1000.0;
    last_us = now;
    if (anim_now >= 1e8) anim_now = anim_len;              /* closing from "finished": start at the end of the film */
    anim_now += anim_dir * dt;
    if (anim_dir > 0 && anim_now >= anim_len) {
        anim_now = 1e9;
        anim_apply();
        cstate = CS_OPEN;
        tick_running = FALSE;
        return G_SOURCE_REMOVE;
    }
    if (anim_dir < 0 && anim_now <= 0) {
        anim_now = 0;
        anim_apply();
        tick_running = FALSE;
        if (!finish_src) finish_src = g_idle_add(card_finish, NULL);   /* not from inside the tick */
        return G_SOURCE_REMOVE;
    }
    anim_apply();
    return G_SOURCE_CONTINUE;
}

static void anim_run(void) {
    if (tick_running || !win) return;
    tick_running = TRUE;
    last_us = g_get_monotonic_time();
    gtk_widget_add_tick_callback(win, anim_tick, NULL, NULL);
}

static gboolean anim_begin(gpointer d) {                   /* after the first frame: the film starts */
    (void)d;
    begin_src = 0;
    if (cstate != CS_OPENING || !win) return G_SOURCE_REMOVE;
    for (guint i = 0; sections && i < sections->len; i++) {
        Section *c = g_ptr_array_index(sections, i);
        c->elems = g_ptr_array_new();
        collect_elems(c->w, c->elems);
        sort_root = card_w;
        g_ptr_array_sort(c->elems, elem_cmp);
    }
    anim_len = anim_total();
    if (anim_ms > 0) { anim_dir = 1; anim_run(); }
    else { anim_now = 1e9; anim_apply(); cstate = CS_OPEN; }
    return G_SOURCE_REMOVE;
}

static void card_open_now(void) {
    GdkRectangle g = { 0, 0, 1920, 1080 };
    GdkMonitor *m = gdk_display_get_monitor(gdk_display_get_default(), 0);
    if (m) gdk_monitor_get_geometry(m, &g);

    card_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, MAX(0, cfg_int("dashboard", "section_gap", 12)));
    gtk_widget_set_name(card_w, "card");
    gtk_style_context_add_class(gtk_widget_get_style_context(card_w), "card");
    gtk_widget_set_size_request(card_w, c_width, -1);
    gtk_widget_set_valign(card_w, GTK_ALIGN_START);
    g_signal_connect(card_w, "draw", G_CALLBACK(card_draw), NULL);
    win = indicator_expand(card_w, c_margin_top, c_margin);   /* the ONE window: indicator row hides, the card takes its place */
    GtkBorder pad;
    gtk_style_context_get_padding(gtk_widget_get_style_context(card_w), GTK_STATE_FLAG_NORMAL, &pad);
    card_inner_w = c_width - pad.left - pad.right;

    anim_now = anim_ms > 0 ? 0.0 : 1e9;
    anim_dir = 1;
    cstate = CS_OPENING;
    card_build(card_w);
    tray_set_origin(g.x + g.width - c_margin - c_width, g.y + c_margin_top);   /* where tray menus open */
    gtk_widget_show_all(card_w);
    begin_src = g_idle_add_full(G_PRIORITY_LOW, anim_begin, NULL, NULL);
    card_tick(GINT_TO_POINTER(1));
    tick_src = g_timeout_add_seconds(1, card_tick, NULL);
}

void card_open(void) {
    switch (cstate) {
    case CS_CLOSED: card_open_now(); break;
    case CS_CLOSING:                                       /* changed its mind: play it forwards again */
        want_reopen = TRUE;
        break;
    default: break;
    }
}

void card_close(void) {
    switch (cstate) {
    case CS_OPENING: case CS_OPEN:
        cstate = CS_CLOSING;
        if (!finish_src) finish_src = g_idle_add(card_finish, NULL);   /* no reverse film: the indicator just pops back */
        break;
    default: break;
    }
}

/* ================================================================== */
/* Setup (once)                                                        */
/* ================================================================== */
void card_init(void) {
    static gboolean done = FALSE;
    if (done) return;
    done = TRUE;
    c_width = MAX(120, cfg_int("dashboard", "width", 380));
    c_margin = cfg_int("dashboard", "margin", 12);
    c_margin_top = cfg_int("dashboard", "margin_top", 40);   /* leaves room for the indicator */
    c_interval = MAX(1, cfg_int("dashboard", "interval", 5));
    stat_every = c_interval;
    anim_ms = MAX(0, cfg_int("dashboard", "expand_ms", 260));
    card_delay = MAX(0, cfg_int("dashboard", "card_delay_ms", 70));
    elem_delay = MAX(0, cfg_int("dashboard", "element_delay_ms", 25));
    btn_size = MAX(0, cfg_int("dashboard", "button_size", 34));
    btn_gap = MAX(0, cfg_int("dashboard", "button_gap", 6));
    btn_cols_cfg = MAX(0, cfg_int("dashboard", "columns", 0));      /* 0 = as many as fit */

    ht_sections_s = cfg_str("dashboard", "halftone", "clock");
    {
        char *st = cfg_str("dashboard", "clock_style", "halftone");
        if (!g_ascii_strcasecmp(st, "text")) { g_free(ht_sections_s); ht_sections_s = g_strdup("none"); }
        g_free(st);
        char *sk = cfg_str("dashboard", "halftone_skip", "calendar");
        ht_skip = g_strsplit_set(sk, ", ", -1);
        g_free(sk);
        ht_dot = CLAMP(cfg_int("dashboard", "halftone_dot", cfg_int("dashboard", "clock_dot", 5)), 0, 16);
        ht_fog = CLAMP(cfg_int("dashboard", "halftone_fog", cfg_int("dashboard", "clock_fog", 6)), 0, 30);
        ht_sharp = CLAMP(cfg_dbl("dashboard", "halftone_sharp", cfg_dbl("dashboard", "clock_sharp", 0.0)), 0.0, 1.0);
        ht_stagger = cfg_bool("dashboard", "halftone_stagger", cfg_bool("dashboard", "clock_stagger", TRUE));
    }

    /* corner radius from config (lower priority than style.css, so the CSS can still override) */
    {
        char rc[256];
        snprintf(rc, sizeof rc, ".card, button.action, scale.slider trough, scale.slider highlight, scale.slider slider { border-radius: %dpx; }",
                 MAX(0, cfg_int("dashboard", "radius", 0)));
        GtkCssProvider *rp = gtk_css_provider_new();
        gtk_css_provider_load_from_data(rp, rc, -1, NULL);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(rp),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    /* built-in looks: lowest priority, so any style.css overrides them */
    {
        char *accent = accent_color();
        char *css = g_strdup_printf(
            "@define-color accent %s;\n"
            "#calendar label { color: alpha(@foreground, 0.7); }\n"
            "#calendar label.cal-head { color: alpha(@foreground, 0.35); }\n"
            "#calendar label.cal-today { color: @accent; font-weight: bold; }\n"
            "scale.slider, scale.slider contents { padding: 0; margin: 0; }\n"
            "scale.slider.horizontal, scale.slider.horizontal contents { min-height: 0; }\n"
            "scale.slider trough { min-height: 26px; background-color: alpha(@foreground, 0.12); background-image: none; border: none; }\n"
            "scale.slider highlight { min-height: 26px; background-color: @accent; background-image: none; border: none; }\n"
            "scale.slider slider { min-width: 0; min-height: 0; margin: 0; opacity: 0; background-image: none; border: none; box-shadow: none; }\n"
            ".slider-icon { background-image: none; background-color: transparent; border: none; box-shadow: none; padding: 0 8px; min-width: 0; min-height: 0; color: @foreground; }\n"
            ".slider-icon.filled { color: @background; }\n"
            ".slider-val { margin: 0 8px; color: @foreground; font-weight: bold; }\n"
            ".slider-val.filled { color: @background; }\n"
            ".stat-key { color: alpha(@foreground, 0.45); }\n"
            ".stat-val { color: @foreground; }\n", accent);
        g_free(accent);
        GtkCssProvider *cp = gtk_css_provider_new();
        gtk_css_provider_load_from_data(cp, css, -1, NULL);
        g_free(css);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(cp),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    /* window background, card alpha, compact buttons: above style.css, so these keys win */
    {
        GString *css = g_string_new("window#dashboard .card { margin: 0; }\n");
        char *bg = cfg_str("dashboard", "bg", NULL);
        double bg_alpha = cfg_dbl("dashboard", "bg_alpha", -1.0);
        if (bg && *bg && g_ascii_strcasecmp(bg, "transparent") != 0) {
            if (bg_alpha >= 0.0) g_string_append_printf(css, "window#dashboard { background-color: alpha(%s, %.2f); }\n", bg, bg_alpha);
            else g_string_append_printf(css, "window#dashboard { background-color: %s; }\n", bg);
        } else if (bg_alpha > 0.0) g_string_append_printf(css, "window#dashboard { background-color: alpha(@background, %.2f); }\n", bg_alpha);
        else g_string_append(css, "window#dashboard { background-color: transparent; }\n");
        g_free(bg);
        double card_alpha = cfg_dbl("dashboard", "card_alpha", -1.0);
        if (card_alpha >= 0.0) g_string_append_printf(css, ".card { background-color: alpha(@background, %.2f); }\n", card_alpha);
        if (btn_size > 0)
            g_string_append_printf(css, "button.action { min-width: %dpx; min-height: %dpx; padding: 0; }\n", btn_size, btn_size);
        GtkCssProvider *bp = gtk_css_provider_new();
        gtk_css_provider_load_from_data(bp, css->str, -1, NULL);
        g_string_free(css, TRUE);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(bp),
                                                  GTK_STYLE_PROVIDER_PRIORITY_USER + 10);
    }

    /* the tray lives for the whole session (apps keep a stable watcher); its buttons are placed in the card when it opens */
    if (cfg_bool("dashboard", "tray_buttons", TRUE)) tray_init(NULL, cfg_int("dashboard", "tray_icon", 18), on_tray_changed);
    else tray_init(NULL, 0, NULL);
}
