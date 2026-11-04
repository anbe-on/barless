/* tray.c - StatusNotifierItem tray.
 *
 * Watcher side (always): answers org.kde.StatusNotifierWatcher if nobody else
 * (e.g. waybar) owns it. The always-on indicator process holds this role so tray
 * apps see a stable watcher even while the dashboard is closed.
 *
 * Host side (only when a box is given): renders the items as icons. Only the
 * dashboard process does this, so no icon theme or pixbuf is loaded in the
 * always-on process. */
#include "barless.h"
#include <string.h>
#include <unistd.h>

#define SNI_WATCHER  "org.kde.StatusNotifierWatcher"
#define SNI_WPATH    "/StatusNotifierWatcher"
#define SNI_ITEM     "org.kde.StatusNotifierItem"

typedef struct {
    int refs;
    gboolean gone;
    char *id, *service, *path;                /* id = "service/path" as registered */
    char *icon_name, *theme_path, *title, *status, *menu_path;
    gboolean is_menu;                         /* ItemIsMenu: a click should open the menu, not Activate */
    GdkPixbuf *src;                           /* only for apps that send raw pixels */
    GtkWidget *ebox, *img;
    guint sig;
} TrayItem;

static GDBusConnection *bus = NULL;
static GtkWidget *tray_box = NULL;
static GList *tray_items = NULL;
static char *host_name = NULL;
static int icon_px = 18;
static int origin_x = 0, origin_y = 0;
static void (*changed_cb)(int) = NULL;

void tray_set_origin(int x, int y) { origin_x = x; origin_y = y; }

static void notify_changed(void) {
    if (!changed_cb) return;
    int n = 0;
    for (GList *l = tray_items; l; l = l->next)
        if (gtk_widget_get_visible(((TrayItem *)l->data)->ebox)) n++;
    changed_cb(n);
}

static TrayItem *item_ref(TrayItem *t) { t->refs++; return t; }
static void item_unref(gpointer p) {
    TrayItem *t = p;
    if (--t->refs > 0) return;
    g_free(t->id); g_free(t->service); g_free(t->path);
    g_free(t->icon_name); g_free(t->theme_path); g_free(t->title); g_free(t->status); g_free(t->menu_path);
    if (t->src) g_object_unref(t->src);
    g_free(t);
}

static void add_theme_path(const char *p) {
    static GHashTable *seen = NULL;
    if (!p || !*p) return;
    if (!seen) seen = g_hash_table_new(g_str_hash, g_str_equal);
    if (g_hash_table_contains(seen, p)) return;
    g_hash_table_add(seen, g_strdup(p));
    gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), p);
}

/* Raw-pixel icons are ARGB32 big-endian; pick the smallest one >= px, else the largest. */
static GdkPixbuf *pick_pixmap(GVariant *pm, int px) {
    GVariantIter it;
    g_variant_iter_init(&it, pm);
    GdkPixbuf *best = NULL;
    int bw = 0;
    GVariant *c;
    while ((c = g_variant_iter_next_value(&it))) {
        int w, h;
        GVariant *bytes;
        g_variant_get(c, "(ii@ay)", &w, &h, &bytes);
        gsize n = 0;
        const guchar *d = g_variant_get_fixed_array(bytes, &n, 1);
        gboolean better = !best || (bw < px ? w > bw : (w >= px && w < bw));
        if (w > 0 && h > 0 && n >= (gsize)w * h * 4 && better) {
            GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
            guchar *o = gdk_pixbuf_get_pixels(pb);
            int rs = gdk_pixbuf_get_rowstride(pb);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const guchar *s = d + ((gsize)y * w + x) * 4;
                    guchar *q = o + (gsize)y * rs + x * 4;
                    q[0] = s[1]; q[1] = s[2]; q[2] = s[3]; q[3] = s[0];
                }
            if (best) g_object_unref(best);
            best = pb;
            bw = w;
        }
        g_variant_unref(bytes);
        g_variant_unref(c);
    }
    return best;
}

static void refresh_icon(TrayItem *t) {
    GtkIconTheme *th = gtk_icon_theme_get_default();
    GdkPixbuf *pb = NULL;
    if (t->icon_name && *t->icon_name) {
        if (t->icon_name[0] == '/')
            pb = gdk_pixbuf_new_from_file_at_size(t->icon_name, icon_px, icon_px, NULL);
        else
            pb = gtk_icon_theme_load_icon(th, t->icon_name, icon_px, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    }
    if (!pb && t->src) pb = gdk_pixbuf_scale_simple(t->src, icon_px, icon_px, GDK_INTERP_BILINEAR);
    if (!pb) pb = gtk_icon_theme_load_icon(th, "image-missing", icon_px, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    gtk_image_set_from_pixbuf(GTK_IMAGE(t->img), pb);
    if (pb) g_object_unref(pb);
    gtk_widget_set_tooltip_text(t->ebox, t->title);
    gtk_widget_set_visible(t->ebox, g_strcmp0(t->status, "Passive") != 0);
}

static void on_item_props(GObject *src, GAsyncResult *res, gpointer data) {
    TrayItem *t = data;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (ret && !t->gone) {
        GVariant *d = g_variant_get_child_value(ret, 0);
        const char *s;
        g_free(t->icon_name);  t->icon_name  = g_variant_lookup(d, "IconName",      "&s", &s) ? g_strdup(s) : NULL;
        g_free(t->theme_path); t->theme_path = g_variant_lookup(d, "IconThemePath", "&s", &s) ? g_strdup(s) : NULL;
        g_free(t->title);      t->title      = g_variant_lookup(d, "Title",         "&s", &s) ? g_strdup(s) : NULL;
        g_free(t->status);     t->status     = g_variant_lookup(d, "Status",        "&s", &s) ? g_strdup(s) : NULL;
        g_free(t->menu_path);
        t->menu_path = g_variant_lookup(d, "Menu", "&o", &s) ? g_strdup(s) : NULL;
        if (!g_variant_lookup(d, "ItemIsMenu", "b", &t->is_menu)) t->is_menu = FALSE;
        if (t->src) { g_object_unref(t->src); t->src = NULL; }
        GVariant *pm = g_variant_lookup_value(d, "IconPixmap", G_VARIANT_TYPE("a(iiay)"));
        if (pm) { t->src = pick_pixmap(pm, icon_px); g_variant_unref(pm); }
        g_variant_unref(d);
        add_theme_path(t->theme_path);
        refresh_icon(t);
        notify_changed();
    }
    if (ret) g_variant_unref(ret);
    item_unref(t);
}

static void item_fetch(TrayItem *t) {
    g_dbus_connection_call(bus, t->service, t->path, "org.freedesktop.DBus.Properties", "GetAll",
                           g_variant_new("(s)", SNI_ITEM), G_VARIANT_TYPE("(a{sv})"),
                           G_DBUS_CALL_FLAGS_NONE, 3000, NULL, on_item_props, item_ref(t));
}

static void on_item_signal(GDBusConnection *c, const gchar *sender, const gchar *op,
                           const gchar *iface, const gchar *sig, GVariant *p, gpointer data) {
    (void)c; (void)sender; (void)op; (void)iface; (void)sig; (void)p;
    TrayItem *t = data;
    if (!t->gone) item_fetch(t);       /* NewIcon / NewTitle / NewStatus ...: just re-read */
}

/* ---- dbusmenu: apps such as Steam, fcitx5 and ibus expect the host to draw their menu ---- */
#define DBM "com.canonical.dbusmenu"

typedef struct { char *svc, *path; int id; } MenuAct;

static void act_free(gpointer d, GClosure *c) { (void)c; MenuAct *a = d; g_free(a->svc); g_free(a->path); g_free(a); }

static void on_menu_activate(GtkMenuItem *mi, gpointer d) {
    (void)mi;
    MenuAct *a = d;
    if (g_getenv("PL_DBG")) g_printerr("menu activate id=%d svc=%s path=%s\n", a->id, a->svc, a->path);
    g_dbus_connection_call(bus, a->svc, a->path, DBM, "Event",
                           g_variant_new("(isvu)", a->id, "clicked", g_variant_new_int32(0),
                                         (guint32)(g_get_real_time() / 1000000)),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void build_menu(GtkWidget *shell, GVariant *node, const char *svc, const char *path) {
    GVariant *kids = g_variant_get_child_value(node, 2);          /* av */
    GVariantIter it;
    g_variant_iter_init(&it, kids);
    GVariant *v;
    while ((v = g_variant_iter_next_value(&it))) {
        GVariant *n = g_variant_get_variant(v);                   /* (ia{sv}av) */
        gint32 id = 0;
        g_variant_get_child(n, 0, "i", &id);
        GVariant *pr = g_variant_get_child_value(n, 1);
        const char *label = "", *type = "", *toggle = "", *disp = "";
        gboolean enabled = TRUE, visible = TRUE;
        gint32 tstate = 0;
        g_variant_lookup(pr, "label", "&s", &label);
        g_variant_lookup(pr, "type", "&s", &type);
        g_variant_lookup(pr, "toggle-type", "&s", &toggle);
        g_variant_lookup(pr, "children-display", "&s", &disp);
        g_variant_lookup(pr, "enabled", "b", &enabled);
        g_variant_lookup(pr, "visible", "b", &visible);
        g_variant_lookup(pr, "toggle-state", "i", &tstate);
        if (visible) {
            GtkWidget *mi;
            if (!strcmp(type, "separator")) mi = gtk_separator_menu_item_new();
            else if (!strcmp(toggle, "checkmark") || !strcmp(toggle, "radio")) {
                mi = gtk_check_menu_item_new_with_mnemonic(label);
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), tstate == 1);
                if (!strcmp(toggle, "radio")) gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(mi), TRUE);
            } else mi = gtk_menu_item_new_with_mnemonic(label);
            gtk_widget_set_sensitive(mi, enabled);
            if (!strcmp(disp, "submenu")) {
                GtkWidget *sub = gtk_menu_new();
                build_menu(sub, n, svc, path);
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), sub);
            } else if (strcmp(type, "separator")) {
                MenuAct *a = g_new0(MenuAct, 1);
                a->svc = g_strdup(svc); a->path = g_strdup(path); a->id = id;
                g_signal_connect_data(mi, "activate", G_CALLBACK(on_menu_activate), a, act_free, 0);
            }
            gtk_menu_shell_append(GTK_MENU_SHELL(shell), mi);
        }
        g_variant_unref(pr);
        g_variant_unref(n);
        g_variant_unref(v);
    }
    g_variant_unref(kids);
}

/* The menu hides *before* the clicked item's "activate" fires, so destroy it a moment later. */
static gboolean destroy_later(gpointer w) { gtk_widget_destroy(GTK_WIDGET(w)); g_object_unref(w); return G_SOURCE_REMOVE; }
static void menu_hidden(GtkWidget *menu, gpointer d) { (void)d; g_idle_add(destroy_later, g_object_ref(menu)); }

typedef struct { TrayItem *t; GtkWidget *anchor; GdkEvent *ev; } MenuReq;

static void on_layout(GObject *src, GAsyncResult *res, gpointer data) {
    MenuReq *r = data;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (ret && !r->t->gone) {
        GVariant *root = g_variant_get_child_value(ret, 1);       /* (ia{sv}av) */
        GtkWidget *menu = gtk_menu_new();
        build_menu(menu, root, r->t->service, r->t->menu_path);
        g_variant_unref(root);
        if (gtk_container_get_children(GTK_CONTAINER(menu))) {
            gtk_widget_show_all(menu);
            g_signal_connect(menu, "hide", G_CALLBACK(menu_hidden), NULL);   /* freed after it closes */
            gtk_menu_popup_at_widget(GTK_MENU(menu), r->anchor, GDK_GRAVITY_SOUTH_WEST,
                                     GDK_GRAVITY_NORTH_WEST, r->ev);
        } else gtk_widget_destroy(menu);
    }
    if (ret) g_variant_unref(ret);
    gdk_event_free(r->ev);
    g_object_unref(r->anchor);
    item_unref(r->t);
    g_free(r);
}

static void show_item_menu(TrayItem *t, GtkWidget *anchor, GdkEventButton *e) {
    static const char *const no_props[] = { NULL };
    MenuReq *r = g_new0(MenuReq, 1);
    r->t = item_ref(t);
    r->anchor = g_object_ref(anchor);
    r->ev = gdk_event_copy((GdkEvent *)e);
    /* AboutToShow is a courtesy ping (some apps refresh their menu on it); we don't wait for it */
    g_dbus_connection_call(bus, t->service, t->menu_path, DBM, "AboutToShow", g_variant_new("(i)", 0),
                           NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
    g_dbus_connection_call(bus, t->service, t->menu_path, DBM, "GetLayout",
                           g_variant_new("(ii^as)", 0, -1, no_props), G_VARIANT_TYPE("(u(ia{sv}av))"),
                           G_DBUS_CALL_FLAGS_NONE, 3000, NULL, on_layout, r);
}

static gboolean on_item_click(GtkWidget *wd, GdkEventButton *e, gpointer data) {
    TrayItem *t = data;
    /* menu-only items (input methods) open their menu on any left/right click;
     * everything else: left = Activate, right = the app's menu (dbusmenu) or ContextMenu */
    gboolean want_menu = e->button == 3 || (e->button == 1 && t->is_menu);
    if (want_menu && t->menu_path && *t->menu_path) { show_item_menu(t, wd, e); return TRUE; }
    const char *m = (e->button == 1 && !t->is_menu) ? "Activate" : e->button == 2 ? "SecondaryActivate" : "ContextMenu";
    int tx = 0, ty = 0;      /* e->x/y are relative to the icon; apps use the position to place menus */
    GtkWidget *top = gtk_widget_get_toplevel(wd);
    gtk_widget_translate_coordinates(wd, top, (int)e->x, (int)e->y, &tx, &ty);
    g_dbus_connection_call(bus, t->service, t->path, SNI_ITEM, m,
                           g_variant_new("(ii)", origin_x + tx, origin_y + ty),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    return TRUE;
}

static void item_add(const char *reg) {
    if (!tray_box) return;
    for (GList *l = tray_items; l; l = l->next)
        if (!strcmp(((TrayItem *)l->data)->id, reg)) return;

    TrayItem *t = g_new0(TrayItem, 1);
    t->refs = 1;
    t->id = g_strdup(reg);
    const char *slash = strchr(reg, '/');
    t->service = slash ? g_strndup(reg, slash - reg) : g_strdup(reg);
    t->path    = slash ? g_strdup(slash) : g_strdup("/StatusNotifierItem");

    t->ebox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(t->ebox), FALSE);
    t->img = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(t->ebox), t->img);
    gtk_widget_add_events(t->ebox, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(t->ebox, "button-press-event", G_CALLBACK(on_item_click), t);
    gtk_box_pack_start(GTK_BOX(tray_box), t->ebox, FALSE, FALSE, 0);
    gtk_widget_show_all(t->ebox);

    t->sig = g_dbus_connection_signal_subscribe(bus, t->service, SNI_ITEM, NULL, t->path, NULL,
                 G_DBUS_SIGNAL_FLAGS_NONE, on_item_signal, item_ref(t), item_unref);
    tray_items = g_list_append(tray_items, t);
    item_fetch(t);
}

static void item_remove(const char *reg) {
    for (GList *l = tray_items; l; l = l->next) {
        TrayItem *t = l->data;
        if (strcmp(t->id, reg)) continue;
        tray_items = g_list_delete_link(tray_items, l);
        t->gone = TRUE;
        g_dbus_connection_signal_unsubscribe(bus, t->sig);
        gtk_widget_destroy(t->ebox);
        item_unref(t);
        notify_changed();
        return;
    }
}

static void items_clear(void) {
    while (tray_items) item_remove(((TrayItem *)tray_items->data)->id);
}

/* ---- host side: follow whichever watcher owns the well-known name ---- */
static void on_items_list(GObject *src, GAsyncResult *res, gpointer data) {
    (void)data;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!ret) return;
    GVariant *v = NULL;
    g_variant_get(ret, "(v)", &v);             /* v holds the "as" list */
    gsize n = 0;
    const gchar **items = g_variant_get_strv(v, &n);
    for (gsize i = 0; i < n; i++) item_add(items[i]);
    g_free(items);
    g_variant_unref(v);
    g_variant_unref(ret);
}

static void on_w_appeared(GDBusConnection *c, const gchar *name, const gchar *owner, gpointer d) {
    (void)c; (void)name; (void)owner; (void)d;
    g_dbus_connection_call(bus, SNI_WATCHER, SNI_WPATH, SNI_WATCHER, "RegisterStatusNotifierHost",
                           g_variant_new("(s)", host_name), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                           NULL, NULL, NULL);
    g_dbus_connection_call(bus, SNI_WATCHER, SNI_WPATH, "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", SNI_WATCHER, "RegisteredStatusNotifierItems"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
                           on_items_list, NULL);
}

static void on_w_vanished(GDBusConnection *c, const gchar *name, gpointer d) {
    (void)c; (void)name; (void)d;
    items_clear();          /* apps re-register with the next watcher */
}

static void on_w_signal(GDBusConnection *c, const gchar *sender, const gchar *op, const gchar *iface,
                        const gchar *sig, GVariant *p, gpointer d) {
    (void)c; (void)sender; (void)op; (void)iface; (void)d;
    const char *reg;
    if (!strcmp(sig, "StatusNotifierItemRegistered")) {
        g_variant_get(p, "(&s)", &reg);
        item_add(reg);
    } else if (!strcmp(sig, "StatusNotifierItemUnregistered")) {
        g_variant_get(p, "(&s)", &reg);
        item_remove(reg);
    }
}

/* ---- watcher side: only answers if no other watcher owns the name ---- */
static GPtrArray *w_items = NULL;

static const char watcher_xml[] =
    "<node><interface name='org.kde.StatusNotifierWatcher'>"
    "<method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>"
    "<method name='RegisterStatusNotifierHost'><arg type='s' direction='in'/></method>"
    "<property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "<property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "<property name='ProtocolVersion' type='i' access='read'/>"
    "<signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    "<signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    "<signal name='StatusNotifierHostRegistered'/>"
    "</interface></node>";

static gboolean w_find(const char *reg, guint *idx) {
    return g_ptr_array_find_with_equal_func(w_items, reg, g_str_equal, idx);
}

static void w_item_vanished(GDBusConnection *c, const gchar *name, gpointer reg) {
    (void)name; (void)c;
    guint i;
    if (!bus || !w_find(reg, &i)) return;
    g_ptr_array_remove_index(w_items, i);
    g_dbus_connection_emit_signal(bus, NULL, SNI_WPATH, SNI_WATCHER, "StatusNotifierItemUnregistered",
                                  g_variant_new("(s)", (const char *)reg), NULL);
}

static void w_method(GDBusConnection *c, const gchar *sender, const gchar *op, const gchar *iface,
                     const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer d) {
    (void)op; (void)iface; (void)d;
    if (!strcmp(method, "RegisterStatusNotifierItem")) {
        const char *svc;
        g_variant_get(params, "(&s)", &svc);
        gboolean by_path = (svc[0] == '/');
        char *reg = by_path ? g_strdup_printf("%s%s", sender, svc)
                            : g_strdup_printf("%s/StatusNotifierItem", svc);
        if (!w_find(reg, NULL)) {
            g_ptr_array_add(w_items, g_strdup(reg));
            g_bus_watch_name_on_connection(c, by_path ? sender : svc, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                           NULL, w_item_vanished, g_strdup(reg), g_free);
            g_dbus_connection_emit_signal(c, NULL, SNI_WPATH, SNI_WATCHER,
                                          "StatusNotifierItemRegistered", g_variant_new("(s)", reg), NULL);
        }
        g_free(reg);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *w_get_prop(GDBusConnection *c, const gchar *sender, const gchar *op,
                            const gchar *iface, const gchar *prop, GError **err, gpointer d) {
    (void)c; (void)sender; (void)op; (void)iface; (void)err; (void)d;
    if (!strcmp(prop, "RegisteredStatusNotifierItems")) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        for (guint i = 0; i < w_items->len; i++) g_variant_builder_add(&b, "s", g_ptr_array_index(w_items, i));
        return g_variant_builder_end(&b);
    }
    if (!strcmp(prop, "IsStatusNotifierHostRegistered")) return g_variant_new_boolean(TRUE);
    return g_variant_new_int32(0);
}

static void on_bus(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src; (void)data;
    bus = g_bus_get_finish(res, NULL);
    if (!bus) return;

    /* watcher: queued behind any existing owner, takes over if that one exits */
    w_items = g_ptr_array_new_with_free_func(g_free);
    GDBusNodeInfo *ni = g_dbus_node_info_new_for_xml(watcher_xml, NULL);
    static const GDBusInterfaceVTable vt = { w_method, w_get_prop, NULL, { 0 } };
    g_dbus_connection_register_object(bus, SNI_WPATH, ni->interfaces[0], &vt, NULL, NULL, NULL);
    g_bus_own_name_on_connection(bus, SNI_WATCHER, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);

    if (!tray_box) return;       /* watcher-only mode */

    host_name = g_strdup_printf("org.kde.StatusNotifierHost-%d-1", (int)getpid());
    g_bus_own_name_on_connection(bus, host_name, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
    g_dbus_connection_signal_subscribe(bus, SNI_WATCHER, SNI_WATCHER, NULL, SNI_WPATH, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_w_signal, NULL, NULL);
    g_bus_watch_name_on_connection(bus, SNI_WATCHER, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   on_w_appeared, on_w_vanished, NULL, NULL);
}

void tray_init(GtkWidget *box, int px, void (*changed)(int n)) {
    tray_box = box;
    icon_px = px > 0 ? px : 18;
    changed_cb = changed;
    g_bus_get(G_BUS_TYPE_SESSION, NULL, on_bus, NULL);
}
