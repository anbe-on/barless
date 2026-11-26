/* One shape that changes position and size like a fluid: each of its four edges is a damped spring.
 * Edges that move outwards are a little stiffer than the ones that move inwards, so the shape stretches towards where
 * it goes and then settles (morph_stretch); morph_bounce is the overshoot. It is driven by the frame clock and
 * removes itself when everything has settled: nothing runs while the shape rests.
 * [dashboard] morph_ms (0 = off: the old growing box), morph_bounce, morph_stretch. */
#include "barless.h"
#include <math.h>

static int m_ms = -1;
static double m_bounce = 0.5, m_stretch = 0.5;

static void morph_cfg(void) {
    if (m_ms >= 0) return;
    m_ms = MAX(0, cfg_int("dashboard", "morph_ms", 300));
    m_bounce = CLAMP(cfg_dbl("dashboard", "morph_bounce", 0.5), 0.0, 1.0);
    m_stretch = CLAMP(cfg_dbl("dashboard", "morph_stretch", 0.5), 0.0, 1.0);
}
int morph_ms(void) { morph_cfg(); return m_ms; }

void morph_init(Morph *m, GtkWidget *w, void (*frame)(gpointer, gboolean), gpointer data) {
    memset(m, 0, sizeof *m);
    m->w = w;
    m->frame = frame;
    m->data = data;
    m->speed = 1.0;
}

MRect morph_rect(const Morph *m) {
    MRect r = { m->e[0], m->e[1], m->e[2] - m->e[0], m->e[3] - m->e[1] };
    if (r.w < 2) r.w = 2;
    if (r.h < 2) r.h = 2;
    return r;
}

static void edges_of(MRect r, double *e) { e[0] = r.x; e[1] = r.y; e[2] = r.x + r.w; e[3] = r.y + r.h; }

static void dirty(Morph *m, MRect a, MRect b) {
    double x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y), x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    gtk_widget_queue_draw_area(m->w, (int)floor(x0) - 4, (int)floor(y0) - 4, (int)ceil(x1 - x0) + 8, (int)ceil(y1 - y0) + 8);
}

void morph_jump(Morph *m, MRect r) {
    MRect old = morph_rect(m);
    edges_of(r, m->e);
    edges_of(r, m->t);
    memset(m->v, 0, sizeof m->v);
    dirty(m, old, morph_rect(m));
}

static gboolean morph_tick(GtkWidget *w, GdkFrameClock *fc, gpointer d) {
    (void)w;
    Morph *m = d;
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    double dt = m->last_us ? (now - m->last_us) / 1e6 : 1.0 / 60.0;
    m->last_us = now;
    dt = MIN(dt, 0.034) * m->speed;
    MRect before = morph_rect(m);
    double T = MAX(60, m_ms) / 1000.0;
    double zeta = 1.0 - 0.5 * m_bounce;
    double omega = 4.4 / (zeta * T);
    int steps = MAX(1, (int)ceil(dt / 0.004));
    double h = dt / steps;
    for (int s = 0; s < steps; s++) {
        for (int i = 0; i < 4; i++) {
            double diff = m->t[i] - m->e[i];
            gboolean outward = (i < 2) ? diff < 0 : diff > 0;          /* left / top move outwards towards smaller values */
            double om = omega * (outward ? 1.0 + 0.7 * m_stretch : 1.0 - 0.4 * m_stretch);
            m->v[i] += (om * om * diff - 2.0 * zeta * om * m->v[i]) * h;
            m->e[i] += m->v[i] * h;
        }
    }
    gboolean settled = TRUE;
    for (int i = 0; i < 4; i++) if (fabs(m->t[i] - m->e[i]) > 0.3 || fabs(m->v[i]) > 10.0) settled = FALSE;
    if (settled) { for (int i = 0; i < 4; i++) { m->e[i] = m->t[i]; m->v[i] = 0; } m->tick_id = 0; m->last_us = 0; }
    dirty(m, before, morph_rect(m));
    if (m->frame) m->frame(m->data, settled);                     /* may retarget with morph_go() */
    return settled ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

void morph_go(Morph *m, MRect to) {
    edges_of(to, m->t);
    if (!m->tick_id) {
        m->last_us = 0;
        m->tick_id = gtk_widget_add_tick_callback(m->w, morph_tick, m, NULL);
    }
}

void morph_stop(Morph *m) {
    if (m->tick_id && m->w) gtk_widget_remove_tick_callback(m->w, m->tick_id);
    m->tick_id = 0;
    m->frame = NULL;
}

void morph_draw_shape(GtkWidget *style, cairo_t *cr, MRect r) {
    GtkStyleContext *sc = gtk_widget_get_style_context(style);
    gtk_render_background(sc, cr, r.x, r.y, r.w, r.h);
    gtk_render_frame(sc, cr, r.x, r.y, r.w, r.h);
}
