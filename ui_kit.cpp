// =================================================================================
// ui_kit.cpp - ink-on-paper widget kit (see ui_kit.h)
// =================================================================================

#include "ui_kit.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------------
// Scaling and fonts
// ---------------------------------------------------------------------------------

double ui_scale() {
    static double s = 0;
    if (s > 0) return s;
    // Width only: the Kindle gives the app window less than the full screen
    // height (status bar), and the layouts flex vertically.
    int w, h;
    ui_screen_size(&w, &h);
    s = w / 1072.0;
    if (s < 0.3) s = 0.3;
    return s;
}

void ui_screen_size(int* w, int* h) {
    int sw = 1072, sh = 1448;
    GdkScreen* sc = gdk_screen_get_default();
    if (sc) { sw = gdk_screen_get_width(sc); sh = gdk_screen_get_height(sc); }
    const char* env = getenv("LARK_WINDOW_SIZE");
    int ew = 0, eh = 0;
    if (env && sscanf(env, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) { sw = ew; sh = eh; }
    if (w) *w = sw;
    if (h) *h = sh;
}

int ui_px(int mock_px) {
    if (mock_px == 0) return 0;
    int v = (int)(mock_px * ui_scale() + 0.5);
    if (v == 0) v = mock_px > 0 ? 1 : -1;
    return v;
}

const char* lark_ui_font() {
    static const char* fam = NULL;
    if (fam) return fam;
    static const char* prefs[] = { "Amazon Ember", "Bookerly", "DejaVu Sans", NULL };
    PangoContext* pc = gdk_pango_context_get();
    PangoFontFamily** fams = NULL;
    int n = 0;
    pango_context_list_families(pc, &fams, &n);
    for (int p = 0; !fam && prefs[p]; p++) {
        for (int i = 0; i < n; i++) {
            if (g_ascii_strcasecmp(pango_font_family_get_name(fams[i]), prefs[p]) == 0) {
                fam = prefs[p];
                break;
            }
        }
    }
    g_free(fams);
    g_object_unref(pc);
    if (!fam) fam = "Sans";
    return fam;
}

static PangoFontDescription* make_font(int screen_px, bool bold, bool italic) {
    PangoFontDescription* fd = pango_font_description_new();
    pango_font_description_set_family(fd, lark_ui_font());
    pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_style(fd, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_font_description_set_absolute_size(fd, screen_px * PANGO_SCALE);
    return fd;
}

// ---------------------------------------------------------------------------------
// Labels and spacers
// ---------------------------------------------------------------------------------

// (ui_label_* are implemented below, after the drawn-control machinery)

static int g_status_inset = -1;
static std::vector<GtkWidget*> g_insets;

int ui_status_inset() { return g_status_inset >= 0 ? g_status_inset : ui_px(66); }

static void inset_destroyed(GtkWidget* w, gpointer) {
    for (size_t i = 0; i < g_insets.size(); i++)
        if (g_insets[i] == w) { g_insets.erase(g_insets.begin() + i); break; }
}

GtkWidget* ui_inset_new() {
    GtkWidget* a = gtk_alignment_new(0, 0, 0, 0);
    gtk_widget_set_size_request(a, 1, ui_status_inset());
    g_insets.push_back(a);
    g_signal_connect(a, "destroy", G_CALLBACK(inset_destroyed), NULL);
    return a;
}

void ui_set_status_inset(int px) {
    if (px < 0) px = 0;
    g_status_inset = px;
    for (size_t i = 0; i < g_insets.size(); i++)
        gtk_widget_set_size_request(g_insets[i], 1, px);
}

GtkWidget* ui_vspace(int mock_h) {
    GtkWidget* a = gtk_alignment_new(0, 0, 0, 0);
    gtk_widget_set_size_request(a, 1, mock_h > 0 ? ui_px(mock_h) : 0);
    return a;
}

GtkWidget* ui_hspace(int mock_w) {
    GtkWidget* a = gtk_alignment_new(0, 0, 0, 0);
    gtk_widget_set_size_request(a, mock_w > 0 ? ui_px(mock_w) : 0, 1);
    return a;
}

GtkWidget* ui_rule(int mock_px) {
    GtkWidget* eb = gtk_event_box_new();
    GdkColor black = { 0, 0, 0, 0 };
    gtk_widget_modify_bg(eb, GTK_STATE_NORMAL, &black);
    gtk_widget_set_size_request(eb, -1, ui_px(mock_px));
    return eb;
}

// ---------------------------------------------------------------------------------
// Drawn control state
// ---------------------------------------------------------------------------------

enum UiKind { K_BUTTON, K_GLYPH, K_FIELD, K_TOGGLE, K_RADIO, K_TABBAR, K_ROW, K_TEXT, K_TEXTBOX };

struct UiBox {
    UiKind      kind = K_BUTTON;
    std::string text;
    std::string hint;
    UiIcon      icon = UI_ICON_NONE;
    int         icon_h = 0;        // screen px
    int         font_px = 20;      // screen px
    bool        bold = false;
    int         border = 3;        // screen px
    int         radius = 0;        // screen px
    bool        border_visible = true;
    bool        checked = false;
    bool        password = false;
    UiCb        cb = NULL;
    gpointer    user = NULL;
    bool        pressed = false;
    double      press_x = 0, press_y = 0;
    int         tab_active = 0;
    // K_TEXT
    bool        italic = false;
    int         wrap_w = 0;        // screen px, 0 = single line at natural width
    int         max_lines = 0;
    double      xalign = 0.5;
    int         gray = 26;
};

static UiBox* box_of(GtkWidget* w) {
    return (UiBox*)g_object_get_data(G_OBJECT(w), "uibox");
}

static void box_free(gpointer p) { delete (UiBox*)p; }

// ---------------------------------------------------------------------------------
// Deferred (idle) callback invocation, safe against widget destruction
// ---------------------------------------------------------------------------------

struct Deferred {
    GtkWidget* w;
    UiCb       cb;
    gpointer   arg;
    bool       dead;
    gulong     sig;
};

static void deferred_widget_destroyed(GtkWidget*, gpointer data) {
    ((Deferred*)data)->dead = true;
}

static gboolean deferred_run(gpointer data) {
    Deferred* d = (Deferred*)data;
    if (g_signal_handler_is_connected(d->w, d->sig))
        g_signal_handler_disconnect(d->w, d->sig);
    if (!d->dead) d->cb(d->w, d->arg);
    g_object_unref(d->w);
    delete d;
    return FALSE;
}

static void defer_call(GtkWidget* w, UiCb cb, gpointer arg) {
    if (!cb) return;
    Deferred* d = new Deferred();
    d->w = (GtkWidget*)g_object_ref(w);
    d->cb = cb;
    d->arg = arg;
    d->dead = false;
    d->sig = g_signal_connect(w, "destroy", G_CALLBACK(deferred_widget_destroyed), d);
    g_idle_add(deferred_run, d);
}

// ---------------------------------------------------------------------------------
// Cairo drawing helpers
// ---------------------------------------------------------------------------------

static void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    if (r <= 0) { cairo_rectangle(cr, x, y, w, h); return; }
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

// Filled triangle with softly rounded corners (round line joins).
static void tri(cairo_t* cr, double x1, double y1, double x2, double y2,
                double x3, double y3, double lw) {
    cairo_set_line_width(cr, lw);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);
    cairo_line_to(cr, x3, y3);
    cairo_close_path(cr);
    cairo_fill_preserve(cr);
    cairo_stroke(cr);
}

// Draws an icon centered on (cx, cy) with nominal height h (screen px).
static void draw_icon(cairo_t* cr, UiIcon icon, double cx, double cy, double h) {
    cairo_save(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    switch (icon) {
    case UI_ICON_PLAY: {
        double lw = h * 0.08;
        double ih = h - lw, iw = h * 0.88 - lw;
        double x0 = cx - iw / 2;
        tri(cr, x0, cy - ih / 2, x0 + iw, cy, x0, cy + ih / 2, lw);
        break;
    }
    case UI_ICON_REWIND: {
        double lw = h * 0.08;
        double ih = h - lw, tw = h * 0.70 - lw, off = h * 0.72;
        double L = cx - (h * 1.42 - lw) / 2;
        for (int i = 0; i < 2; i++) {
            double tip = L + i * off;
            tri(cr, tip, cy, tip + tw, cy - ih / 2, tip + tw, cy + ih / 2, lw);
        }
        break;
    }
    case UI_ICON_FORWARD: {
        double lw = h * 0.08;
        double ih = h - lw, tw = h * 0.70 - lw, off = h * 0.72;
        double L = cx - (h * 1.42 - lw) / 2;
        for (int i = 0; i < 2; i++) {
            double base = L + i * off;
            tri(cr, base, cy - ih / 2, base, cy + ih / 2, base + tw, cy, lw);
        }
        break;
    }
    case UI_ICON_PAUSE: {
        double bw = h * 0.28, gap = h * 0.22;
        double x0 = cx - (2 * bw + gap) / 2;
        rounded_rect(cr, x0, cy - h / 2, bw, h, h * 0.05);
        cairo_fill(cr);
        rounded_rect(cr, x0 + bw + gap, cy - h / 2, bw, h, h * 0.05);
        cairo_fill(cr);
        break;
    }
    case UI_ICON_BOOKS: {
        // Four outlined books, the last one leaning on its neighbour.
        double lw = MAX(1.5, h * 0.045);
        cairo_set_line_width(cr, lw);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        double x0 = cx - h / 2, y0 = cy - h / 2;
        static const double bx[3] = { 0.03, 0.27, 0.51 };
        static const double bh[3] = { 0.98, 0.88, 0.98 };
        for (int i = 0; i < 3; i++) {
            double bwid = h * 0.21, bhei = h * bh[i];
            double bx0 = x0 + h * bx[i], by0 = y0 + h - bhei;
            cairo_rectangle(cr, bx0, by0, bwid, bhei);
            cairo_stroke(cr);
            for (int k = 0; k < 2; k++) {
                double yy = by0 + h * (0.10 + 0.07 * k);
                cairo_move_to(cr, bx0, yy);
                cairo_line_to(cr, bx0 + bwid, yy);
                cairo_move_to(cr, bx0, by0 + bhei - h * (0.10 + 0.07 * k));
                cairo_line_to(cr, bx0 + bwid, by0 + bhei - h * (0.10 + 0.07 * k));
                cairo_stroke(cr);
            }
        }
        cairo_save(cr);
        cairo_translate(cr, x0 + h * 0.98, y0 + h);
        cairo_rotate(cr, -0.30);
        cairo_rectangle(cr, -h * 0.21, -h * 0.90, h * 0.21, h * 0.90);
        cairo_stroke(cr);
        for (int k = 0; k < 2; k++) {
            double yy = -h * 0.90 + h * (0.10 + 0.07 * k);
            cairo_move_to(cr, -h * 0.21, yy);
            cairo_line_to(cr, 0, yy);
            cairo_stroke(cr);
        }
        cairo_restore(cr);
        break;
    }
    case UI_ICON_DOTS: {
        double r = h / 2, step = h * 1.05;
        for (int i = -1; i <= 1; i++) {
            cairo_arc(cr, cx + i * step, cy, r, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        break;
    }
    default:
        break;
    }
    cairo_restore(cr);
}

static PangoLayout* make_layout(GtkWidget* w, const std::string& text, int px,
                                bool bold, PangoAlignment align = PANGO_ALIGN_LEFT,
                                bool italic = false) {
    PangoLayout* lay = gtk_widget_create_pango_layout(w, text.c_str());
    PangoFontDescription* fd = make_font(px, bold, italic);
    pango_layout_set_font_description(lay, fd);
    pango_font_description_free(fd);
    pango_layout_set_alignment(lay, align);
    return lay;
}

// Draws text vertically centered in [y, y+h) starting at x (or right-aligned
// to x when right is set, centered around x when center is set).
static void draw_text(cairo_t* cr, GtkWidget* w, const std::string& text, int px, bool bold,
                      double x, double y, double h, int mode /*0 left,1 center,2 right*/,
                      double gray = 0.1) {
    PangoLayout* lay = make_layout(w, text, px, bold);
    int tw, th;
    pango_layout_get_pixel_size(lay, &tw, &th);
    double tx = x;
    if (mode == 1) tx = x - tw / 2.0;
    else if (mode == 2) tx = x - tw;
    cairo_set_source_rgb(cr, gray, gray, gray);
    cairo_move_to(cr, tx, y + (h - th) / 2.0);
    pango_cairo_show_layout(cr, lay);
    g_object_unref(lay);
}

// ---------------------------------------------------------------------------------
// Expose handler (all drawn kinds)
// ---------------------------------------------------------------------------------

static const char* kTabNames[3] = { "Play", "Library", "Options" };
static const UiIcon kTabIcons[3] = { UI_ICON_PLAY, UI_ICON_BOOKS, UI_ICON_DOTS };
static const int kTabIconMock[3] = { 52, 56, 24 };
static const int kTabIconCy[3] = { 42, 38, 43 };   // icon centre, mockup px from bar top

static gboolean on_expose(GtkWidget* w, GdkEventExpose* ev, gpointer) {
    UiBox* b = box_of(w);
    if (!b) return FALSE;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    double W = a.width, H = a.height;

    cairo_t* cr = gdk_cairo_create(w->window);
    gdk_cairo_region(cr, ev->region);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);

    switch (b->kind) {
    case K_TEXT: {
        PangoAlignment al = b->xalign < 0.25 ? PANGO_ALIGN_LEFT :
                            b->xalign > 0.75 ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_CENTER;
        PangoLayout* lay = make_layout(w, b->text, b->font_px, b->bold, al, b->italic);
        if (b->wrap_w > 0) {
            pango_layout_set_width(lay, (int)W * PANGO_SCALE);
            pango_layout_set_wrap(lay, PANGO_WRAP_WORD_CHAR);
            if (b->max_lines > 0) {
                pango_layout_set_height(lay, -b->max_lines);
                pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);
            }
        }
        int tw, th;
        pango_layout_get_pixel_size(lay, &tw, &th);
        double g = b->gray / 255.0;
        cairo_set_source_rgb(cr, g, g, g);
        double tx = b->wrap_w > 0 ? 0 : (W - tw) * b->xalign;
        cairo_move_to(cr, tx, (H - th) / 2.0);
        pango_cairo_show_layout(cr, lay);
        g_object_unref(lay);
        break;
    }
    case K_TEXTBOX: {
        double bw = b->border, pad = ui_px(20);
        cairo_set_line_width(cr, bw);
        cairo_rectangle(cr, bw / 2, bw / 2, W - bw, H - bw);
        cairo_stroke(cr);
        PangoLayout* lay = make_layout(w, b->text, b->font_px, false);
        pango_layout_set_width(lay, (int)(W - 2 * pad) * PANGO_SCALE);
        pango_layout_set_wrap(lay, PANGO_WRAP_CHAR);
        int tw, th;
        pango_layout_get_pixel_size(lay, &tw, &th);
        // Long text: keep the end (where typing happens) in view
        double ty = pad;
        if (th > H - 2 * pad) ty = H - pad - th;
        cairo_rectangle(cr, bw, bw, W - 2 * bw, H - 2 * bw);
        cairo_clip(cr);
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_move_to(cr, pad, ty);
        pango_cairo_show_layout(cr, lay);
        g_object_unref(lay);
        break;
    }
    case K_BUTTON: {
        double bw = b->border;
        if (b->border_visible && bw > 0) {
            cairo_set_line_width(cr, bw);
            rounded_rect(cr, bw / 2, bw / 2, W - bw, H - bw, b->radius);
            cairo_stroke(cr);
        }
        if (b->icon != UI_ICON_NONE) {
            draw_icon(cr, b->icon, W * 0.25, H / 2, b->icon_h);
            draw_text(cr, w, b->text, b->font_px, b->bold, W - bw - ui_px(14), 0, H, 2);
        } else {
            draw_text(cr, w, b->text, b->font_px, b->bold, W / 2, 0, H, 1);
        }
        break;
    }
    case K_GLYPH:
        draw_icon(cr, b->icon, W / 2, H / 2, b->icon_h);
        break;
    case K_FIELD: {
        double bw = b->border;
        cairo_set_line_width(cr, bw);
        cairo_rectangle(cr, bw / 2, bw / 2, W - bw, H - bw);
        cairo_stroke(cr);
        std::string shown = b->text;
        if (b->password) {
            shown.clear();
            for (size_t i = 0; i < b->text.size() && i < 40; i++) shown += "\xE2\x80\xA2";
        }
        if (shown.empty())
            draw_text(cr, w, b->hint, b->font_px, false, ui_px(16), 0, H, 0, 0.55);
        else
            draw_text(cr, w, shown, b->font_px, false, ui_px(16), 0, H, 0);
        break;
    }
    case K_TOGGLE: {
        double bw = MAX(2.0, H * 0.10);
        double r = H / 2;
        rounded_rect(cr, bw / 2, bw / 2, W - bw, H - bw, r - bw / 2);
        if (b->checked) {
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_arc(cr, W - r, H / 2, r - bw * 1.6, 0, 2 * M_PI);
            cairo_fill(cr);
        } else {
            cairo_set_line_width(cr, bw);
            cairo_stroke(cr);
            cairo_arc(cr, r, H / 2, r - bw * 1.6, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        break;
    }
    case K_RADIO: {
        double ring = H * 0.62;
        double cx = ui_px(14) + ring / 2, cy = H / 2;
        double lw = MAX(2.0, ring * 0.12);
        cairo_set_line_width(cr, lw);
        cairo_arc(cr, cx, cy, ring / 2 - lw / 2, 0, 2 * M_PI);
        cairo_stroke(cr);
        if (b->checked) {
            cairo_arc(cr, cx, cy, ring / 2 - lw * 2.0, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        draw_text(cr, w, b->text, b->font_px, b->checked, cx + ring / 2 + ui_px(26), 0, H, 0);
        break;
    }
    case K_ROW: {
        if (b->checked) {
            cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
            cairo_rectangle(cr, 0, 0, W, H);
            cairo_fill(cr);
        }
        double pad = ui_px(28);
        double right_w = 0;
        if (!b->hint.empty()) {
            PangoLayout* rl = make_layout(w, b->hint, b->font_px, false);
            int rw, rh;
            pango_layout_get_pixel_size(rl, &rw, &rh);
            g_object_unref(rl);
            right_w = rw + ui_px(20);
            draw_text(cr, w, b->hint, b->font_px, false, W - pad, 0, H, 2, 0.25);
        }
        PangoLayout* ll = make_layout(w, b->text, b->font_px, b->checked);
        pango_layout_set_width(ll, (int)(W - 2 * pad - right_w) * PANGO_SCALE);
        pango_layout_set_ellipsize(ll, PANGO_ELLIPSIZE_END);
        int tw, th;
        pango_layout_get_pixel_size(ll, &tw, &th);
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_move_to(cr, pad, (H - th) / 2.0);
        pango_cairo_show_layout(cr, ll);
        g_object_unref(ll);
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_rectangle(cr, 0, H - 2, W, 2);
        cairo_fill(cr);
        break;
    }
    case K_TABBAR: {
        double bw = MAX(2.0, ui_px(6));
        double cell = W / 3.0;
        for (int i = 0; i < 3; i++) {
            double x0 = i * cell;
            if (i == b->tab_active) {
                cairo_set_source_rgb(cr, 0.86, 0.86, 0.86);
                cairo_rectangle(cr, x0, 0, cell, H);
                cairo_fill(cr);
                cairo_set_source_rgb(cr, 0, 0, 0);
            }
            double icon_h = ui_px(kTabIconMock[i]);
            draw_icon(cr, kTabIcons[i], x0 + cell / 2, ui_px(kTabIconCy[i]), icon_h);
            draw_text(cr, w, kTabNames[i], ui_px(28), i == b->tab_active,
                      x0 + cell / 2, H * 0.56, H * 0.40, 1);
        }
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_set_line_width(cr, bw);
        cairo_rectangle(cr, bw / 2, bw / 2, W - bw, H - bw);
        cairo_stroke(cr);
        for (int i = 1; i < 3; i++) {
            cairo_move_to(cr, i * cell, 0);
            cairo_line_to(cr, i * cell, H);
            cairo_stroke(cr);
        }
        break;
    }
    }
    cairo_destroy(cr);
    return TRUE;
}

// ---------------------------------------------------------------------------------
// Tap handling
// ---------------------------------------------------------------------------------

static gboolean on_press(GtkWidget* w, GdkEventButton* e, gpointer) {
    UiBox* b = box_of(w);
    if (!b || e->button != 1 || e->type != GDK_BUTTON_PRESS) return FALSE;
    b->pressed = true;
    b->press_x = e->x_root;
    b->press_y = e->y_root;
    return FALSE;   // let a surrounding scroll area see the press too
}

static gboolean on_release(GtkWidget* w, GdkEventButton* e, gpointer) {
    UiBox* b = box_of(w);
    if (!b || e->button != 1 || !b->pressed) return FALSE;
    b->pressed = false;

    double dx = e->x_root - b->press_x, dy = e->y_root - b->press_y;
    double thr = ui_px(14);
    if (dx * dx + dy * dy > thr * thr) return FALSE;   // drag, not a tap

    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    if (e->x < 0 || e->y < 0 || e->x > a.width || e->y > a.height) return FALSE;

    if (b->kind == K_TOGGLE) {
        b->checked = !b->checked;
        gtk_widget_queue_draw(w);
    }
    if (b->kind == K_TABBAR) {
        int idx = (int)(e->x * 3 / MAX(1, a.width));
        if (idx < 0) idx = 0;
        if (idx > 2) idx = 2;
        defer_call(w, b->cb, GINT_TO_POINTER(idx));
    } else {
        defer_call(w, b->cb, b->user);
    }
    return FALSE;
}

static GtkWidget* box_widget_new(UiBox* b, int w_px, int h_px) {
    GtkWidget* da = gtk_drawing_area_new();
    g_object_set_data_full(G_OBJECT(da), "uibox", b, box_free);
    gtk_widget_set_size_request(da, w_px, h_px);
    // Button-motion is needed so a finger drag that starts on a control still
    // reaches the surrounding scroll area (unselected events are dropped
    // during the implicit grab).
    gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                              GDK_BUTTON_MOTION_MASK);
    g_signal_connect(da, "expose-event", G_CALLBACK(on_expose), NULL);
    g_signal_connect(da, "button-press-event", G_CALLBACK(on_press), NULL);
    g_signal_connect(da, "button-release-event", G_CALLBACK(on_release), NULL);
    return da;
}

// Measures the text and (re)sets the size request of a K_TEXT widget.
static void text_measure(GtkWidget* w) {
    UiBox* b = box_of(w);
    PangoAlignment al = b->xalign < 0.25 ? PANGO_ALIGN_LEFT :
                        b->xalign > 0.75 ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_CENTER;
    PangoLayout* lay = make_layout(w, b->text.empty() ? " " : b->text, b->font_px, b->bold, al, b->italic);
    if (b->wrap_w > 0) {
        pango_layout_set_width(lay, b->wrap_w * PANGO_SCALE);
        pango_layout_set_wrap(lay, PANGO_WRAP_WORD_CHAR);
        if (b->max_lines > 0) {
            pango_layout_set_height(lay, -b->max_lines);
            pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);
        }
    }
    int tw, th;
    pango_layout_get_pixel_size(lay, &tw, &th);
    g_object_unref(lay);
    gtk_widget_set_size_request(w, b->wrap_w > 0 ? b->wrap_w : tw, th);
    gtk_widget_queue_draw(w);
}

GtkWidget* ui_label_new(const std::string& text, int font_px, bool bold, bool italic,
                        double xalign, int wrap_w, int gray, int max_lines) {
    UiBox* b = new UiBox();
    b->kind = K_TEXT;
    b->text = text;
    b->font_px = ui_px(font_px);
    b->bold = bold;
    b->italic = italic;
    b->xalign = xalign;
    b->wrap_w = wrap_w > 0 ? ui_px(wrap_w) : 0;
    b->max_lines = max_lines;
    b->gray = gray;
    GtkWidget* da = gtk_drawing_area_new();
    g_object_set_data_full(G_OBJECT(da), "uibox", b, box_free);
    g_signal_connect(da, "expose-event", G_CALLBACK(on_expose), NULL);
    text_measure(da);
    return da;
}

void ui_label_set(GtkWidget* label, const std::string& text) {
    UiBox* b = box_of(label);
    if (!b || b->text == text) return;   // every redraw costs an e-ink refresh
    b->text = text;
    text_measure(label);
}

void ui_label_style(GtkWidget* label, int font_px, bool bold, bool italic, int gray) {
    UiBox* b = box_of(label);
    if (!b) return;
    b->font_px = ui_px(font_px);
    b->bold = bold;
    b->italic = italic;
    b->gray = gray;
    text_measure(label);
}

void ui_connect(GtkWidget* w, UiCb cb, gpointer user_data) {
    UiBox* b = box_of(w);
    if (!b) return;
    b->cb = cb;
    b->user = user_data;
}

// ---------------------------------------------------------------------------------
// Control constructors / setters
// ---------------------------------------------------------------------------------

GtkWidget* ui_button_new(const std::string& text, int font_px, int mock_w, int mock_h,
                         int border, int radius, bool bold) {
    UiBox* b = new UiBox();
    b->kind = K_BUTTON;
    b->text = text;
    b->font_px = ui_px(font_px);
    b->border = border > 0 ? MAX(1, ui_px(border)) : 0;
    b->radius = ui_px(radius);
    b->bold = bold;
    return box_widget_new(b, mock_w > 0 ? ui_px(mock_w) : -1, ui_px(mock_h));
}

void ui_button_set_text(GtkWidget* button, const std::string& text) {
    UiBox* b = box_of(button);
    if (!b || b->text == text) return;
    b->text = text;
    gtk_widget_queue_draw(button);
}

const std::string& ui_button_get_text(GtkWidget* button) {
    static const std::string empty;
    UiBox* b = box_of(button);
    return b ? b->text : empty;
}

GtkWidget* ui_textbox_new(int font_px, int mock_h) {
    UiBox* b = new UiBox();
    b->kind = K_TEXTBOX;
    b->font_px = ui_px(font_px);
    b->border = MAX(2, ui_px(3));
    GtkWidget* da = gtk_drawing_area_new();
    g_object_set_data_full(G_OBJECT(da), "uibox", b, box_free);
    gtk_widget_set_size_request(da, -1, ui_px(mock_h));
    g_signal_connect(da, "expose-event", G_CALLBACK(on_expose), NULL);
    return da;
}

void ui_textbox_set(GtkWidget* box, const std::string& text) {
    UiBox* b = box_of(box);
    if (!b || b->text == text) return;
    b->text = text;
    gtk_widget_queue_draw(box);
}

void ui_button_set_icon(GtkWidget* button, UiIcon icon, int icon_h) {
    UiBox* b = box_of(button);
    if (!b) return;
    b->icon = icon;
    b->icon_h = ui_px(icon_h);
    gtk_widget_queue_draw(button);
}

void ui_button_set_visible_border(GtkWidget* button, bool visible) {
    UiBox* b = box_of(button);
    if (!b) return;
    b->border_visible = visible;
    gtk_widget_queue_draw(button);
}

GtkWidget* ui_glyph_button_new(UiIcon icon, int glyph_h, int mock_w, int mock_h) {
    UiBox* b = new UiBox();
    b->kind = K_GLYPH;
    b->icon = icon;
    b->icon_h = ui_px(glyph_h);
    return box_widget_new(b, ui_px(mock_w), ui_px(mock_h));
}

void ui_glyph_set_icon(GtkWidget* glyph, UiIcon icon) {
    UiBox* b = box_of(glyph);
    if (!b || b->icon == icon) return;
    b->icon = icon;
    gtk_widget_queue_draw(glyph);
}

GtkWidget* ui_toggle_new(bool on) {
    UiBox* b = new UiBox();
    b->kind = K_TOGGLE;
    b->checked = on;
    return box_widget_new(b, ui_px(78), ui_px(44));
}

void ui_toggle_set(GtkWidget* toggle, bool on) {
    UiBox* b = box_of(toggle);
    if (!b || b->checked == on) return;
    b->checked = on;
    gtk_widget_queue_draw(toggle);
}

bool ui_toggle_get(GtkWidget* toggle) {
    UiBox* b = box_of(toggle);
    return b && b->checked;
}

GtkWidget* ui_radio_new(const std::string& text, int font_px, int mock_h, bool selected) {
    UiBox* b = new UiBox();
    b->kind = K_RADIO;
    b->text = text;
    b->font_px = ui_px(font_px);
    b->checked = selected;
    return box_widget_new(b, -1, ui_px(mock_h));
}

void ui_radio_set(GtkWidget* radio, bool selected) {
    UiBox* b = box_of(radio);
    if (!b || b->checked == selected) return;
    b->checked = selected;
    gtk_widget_queue_draw(radio);
}

GtkWidget* ui_field_new(const std::string& value, const std::string& placeholder,
                        int font_px, int mock_h, bool password) {
    UiBox* b = new UiBox();
    b->kind = K_FIELD;
    b->text = value;
    b->hint = placeholder;
    b->font_px = ui_px(font_px);
    b->border = MAX(2, ui_px(3));
    b->password = password;
    return box_widget_new(b, -1, ui_px(mock_h));
}

void ui_field_set(GtkWidget* field, const std::string& value) {
    UiBox* b = box_of(field);
    if (!b || b->text == value) return;
    b->text = value;
    gtk_widget_queue_draw(field);
}

GtkWidget* ui_row_new(const std::string& left, const std::string& right, int font_px,
                      int mock_h, bool highlighted) {
    UiBox* b = new UiBox();
    b->kind = K_ROW;
    b->text = left;
    b->hint = right;
    b->font_px = ui_px(font_px);
    b->checked = highlighted;
    return box_widget_new(b, -1, ui_px(mock_h));
}

GtkWidget* ui_tabbar_new(int mock_h, UiCb cb) {
    UiBox* b = new UiBox();
    b->kind = K_TABBAR;
    b->cb = cb;
    return box_widget_new(b, -1, ui_px(mock_h));
}

void ui_tabbar_set_active(GtkWidget* bar, int index) {
    UiBox* b = box_of(bar);
    if (!b || b->tab_active == index) return;
    b->tab_active = index;
    gtk_widget_queue_draw(bar);
}

// ---------------------------------------------------------------------------------
// Kinetic scroll area
// ---------------------------------------------------------------------------------

struct Kinetic {
    bool           pressed = false;   // finger down
    bool           engaged = false;   // moved past the tap threshold
    double         press_y = 0;
    double         last_y = 0;
    double         velocity = 0;      // px per 16 ms tick
    guint          coast_id = 0;
    GtkAdjustment* adj = NULL;
};

static void ks_stop_coast(Kinetic* k) {
    if (k->coast_id) { g_source_remove(k->coast_id); k->coast_id = 0; }
}

static void ks_apply(Kinetic* k, double delta) {
    double val = gtk_adjustment_get_value(k->adj) + delta;
    double max = gtk_adjustment_get_upper(k->adj) - gtk_adjustment_get_page_size(k->adj);
    if (max < 0) max = 0;
    if (val < 0) val = 0;
    if (val > max) val = max;
    gtk_adjustment_set_value(k->adj, val);
}

static gboolean ks_coast_tick(gpointer data) {
    Kinetic* k = (Kinetic*)data;
    if (fabs(k->velocity) < 0.5) { k->coast_id = 0; return FALSE; }
    ks_apply(k, -k->velocity);
    k->velocity *= 0.85;
    return TRUE;
}

static gboolean ks_press(GtkWidget*, GdkEventButton* e, gpointer data) {
    Kinetic* k = (Kinetic*)data;
    ks_stop_coast(k);
    k->pressed = true;
    k->engaged = false;
    k->press_y = k->last_y = e->y_root;
    k->velocity = 0;
    return FALSE;
}

static gboolean ks_motion(GtkWidget*, GdkEventMotion* e, gpointer data) {
    Kinetic* k = (Kinetic*)data;
    if (!k->pressed) return FALSE;
    if (!k->engaged) {
        if (fabs(e->y_root - k->press_y) < ui_px(14)) return FALSE;
        k->engaged = true;
        k->last_y = e->y_root;
        return FALSE;
    }
    double dy = k->last_y - e->y_root;   // positive = scroll down
    k->velocity = -dy;
    k->last_y = e->y_root;
    ks_apply(k, dy);
    return FALSE;
}

static gboolean ks_release(GtkWidget*, GdkEventButton*, gpointer data) {
    Kinetic* k = (Kinetic*)data;
    if (!k->pressed) return FALSE;
    k->pressed = false;
    if (k->engaged && fabs(k->velocity) > 1.0 && !k->coast_id)
        k->coast_id = g_timeout_add(16, ks_coast_tick, k);
    k->engaged = false;
    return FALSE;
}

static void ks_free(gpointer p) {
    Kinetic* k = (Kinetic*)p;
    ks_stop_coast(k);
    delete k;
}

GtkWidget* ui_scroll_new(GtkWidget* content) {
    GtkWidget* sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(sw, "uiscroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_NONE);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(sw), content);

    GtkWidget* vp = gtk_bin_get_child(GTK_BIN(sw));
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(vp), GTK_SHADOW_NONE);
    gtk_widget_add_events(vp, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                              GDK_POINTER_MOTION_MASK | GDK_BUTTON_MOTION_MASK);

    Kinetic* k = new Kinetic();
    k->adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    g_object_set_data_full(G_OBJECT(sw), "kinetic", k, ks_free);
    g_signal_connect(vp, "button-press-event", G_CALLBACK(ks_press), k);
    g_signal_connect(vp, "motion-notify-event", G_CALLBACK(ks_motion), k);
    g_signal_connect(vp, "button-release-event", G_CALLBACK(ks_release), k);
    return sw;
}

GtkAdjustment* ui_scroll_adjustment(GtkWidget* scroll) {
    return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
}

void ui_scroll_to_top(GtkWidget* scroll) {
    Kinetic* k = (Kinetic*)g_object_get_data(G_OBJECT(scroll), "kinetic");
    if (k) ks_stop_coast(k);
    gtk_adjustment_set_value(ui_scroll_adjustment(scroll), 0);
}

// ---------------------------------------------------------------------------------
// List picker dialog
// ---------------------------------------------------------------------------------

struct PickRow {
    GtkWidget* dlg;
    int        index;
    int*       chosen;
};

static void pick_row_cb(GtkWidget*, gpointer p) {
    PickRow* r = (PickRow*)p;
    *r->chosen = r->index;
    gtk_dialog_response(GTK_DIALOG(r->dlg), GTK_RESPONSE_ACCEPT);
}

static void pick_close_cb(GtkWidget*, gpointer dlg) {
    gtk_dialog_response(GTK_DIALOG(dlg), GTK_RESPONSE_CANCEL);
}

static gboolean pick_scroll_to(gpointer data) {
    GtkWidget* sw = GTK_WIDGET(data);
    int y = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sw), "target-y"));
    GtkAdjustment* adj = ui_scroll_adjustment(sw);
    double max = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
    double v = y - gtk_adjustment_get_page_size(adj) / 3;
    if (v > max) v = max;
    if (v < 0) v = 0;
    gtk_adjustment_set_value(adj, v);
    g_object_unref(sw);
    return FALSE;
}

int ui_pick_dialog(GtkWindow* parent, const std::string& title,
                   const std::vector<std::string>& rows,
                   const std::vector<std::string>& right_texts, int highlight) {
    int chosen = -1;
    GtkWidget* dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "L:A_N:application_PC:T_ID:com.kbarni.larkplayer");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);
        gtk_window_set_destroy_with_parent(GTK_WINDOW(dlg), TRUE);
    }
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    int sw_px, sh_px;
    ui_screen_size(&sw_px, &sh_px);
    gtk_window_set_default_size(GTK_WINDOW(dlg), sw_px, sh_px);
    gtk_widget_set_size_request(dlg, sw_px, sh_px);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(dlg), 0);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_widget_hide(gtk_dialog_get_action_area(GTK_DIALOG(dlg)));
    gtk_box_set_spacing(GTK_BOX(content), 0);

    GtkWidget* inset = gtk_alignment_new(0, 0, 0, 0);
    gtk_widget_set_size_request(inset, 1, ui_status_inset());
    gtk_box_pack_start(GTK_BOX(content), inset, FALSE, FALSE, 0);

    // Title row: title at the left, Close button at the right
    GtkWidget* head = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), ui_hspace(28), FALSE, FALSE, 0);
    GtkWidget* tl = ui_label_new(title, 46, true, false, 0.0);
    gtk_box_pack_start(GTK_BOX(head), tl, TRUE, TRUE, 0);
    GtkWidget* close = ui_button_new("Close", 30, 190, 76, 4, 8);
    ui_connect(close, pick_close_cb, dlg);
    GtkWidget* close_a = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(close_a), close);
    gtk_box_pack_start(GTK_BOX(head), close_a, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), ui_hspace(28), FALSE, FALSE, 0);
    GtkWidget* head_a = gtk_alignment_new(0.5, 0.5, 1, 0);
    gtk_widget_set_size_request(head_a, -1, ui_px(110));
    gtk_container_add(GTK_CONTAINER(head_a), head);
    gtk_box_pack_start(GTK_BOX(content), head_a, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), ui_rule(4), FALSE, FALSE, 0);

    GtkWidget* list = gtk_vbox_new(FALSE, 0);
    std::vector<PickRow*> pr;
    const int row_h = 92;
    for (size_t i = 0; i < rows.size(); i++) {
        std::string right = i < right_texts.size() ? right_texts[i] : "";
        GtkWidget* row = ui_row_new(rows[i], right, 30, row_h, (int)i == highlight);
        PickRow* r = new PickRow{ dlg, (int)i, &chosen };
        pr.push_back(r);
        ui_connect(row, pick_row_cb, r);
        gtk_box_pack_start(GTK_BOX(list), row, FALSE, FALSE, 0);
    }
    GtkWidget* scroll = ui_scroll_new(list);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

    gtk_widget_show_all(dlg);
    if (highlight >= 0) {
        g_object_set_data(G_OBJECT(scroll), "target-y", GINT_TO_POINTER(highlight * ui_px(row_h)));
        g_idle_add(pick_scroll_to, g_object_ref(scroll));
    }
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    for (size_t i = 0; i < pr.size(); i++) delete pr[i];
    return chosen;
}

// ---------------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------------

void ui_install_theme() {
    // E-ink friendly, Kindle-flavoured theme: white surfaces, near-black text,
    // and flat controls. GTK2's default theme is a chunky 1990s bevel, which
    // looks out of place next to the Kindle UI and dithers badly on e-ink.
    char scroll_rc[1024];
    snprintf(scroll_rc, sizeof(scroll_rc),
        "style \"lark_scrollwin\" {\n"
        "  GtkScrolledWindow::scrollbar-spacing = 0\n"
        "}\n"
        "style \"lark_thinbar\" {\n"
        "  GtkRange::slider-width = %d\n"
        "  GtkRange::trough-border = 0\n"
        "  GtkRange::stepper-size = 0\n"
        "  GtkRange::stepper-spacing = 0\n"
        "  GtkScrollbar::has-backward-stepper = 0\n"
        "  GtkScrollbar::has-forward-stepper = 0\n"
        "  GtkScrollbar::has-secondary-backward-stepper = 0\n"
        "  GtkScrollbar::has-secondary-forward-stepper = 0\n"
        "  GtkScrollbar::min-slider-length = %d\n"
        "  bg[NORMAL]   = { 0.45, 0.45, 0.45 }\n"
        "  bg[PRELIGHT] = { 0.45, 0.45, 0.45 }\n"
        "  bg[ACTIVE]   = { 1.00, 1.00, 1.00 }\n"
        "}\n"
        "widget \"*uiscroll\" style \"lark_scrollwin\"\n"
        "widget \"*uiscroll.GtkVScrollbar\" style \"lark_thinbar\"\n",
        MAX(4, ui_px(7)), ui_px(60));

    gtk_rc_parse_string(
        "style \"lark_flat\" {\n"
        "  bg[NORMAL]      = { 1.00, 1.00, 1.00 }\n"
        "  bg[PRELIGHT]    = { 1.00, 1.00, 1.00 }\n"
        "  bg[INSENSITIVE] = { 1.00, 1.00, 1.00 }\n"
        "  bg[ACTIVE]      = { 0.88, 0.88, 0.88 }\n"
        "  bg[SELECTED]    = { 0.30, 0.30, 0.30 }\n"
        "  fg[NORMAL]      = { 0.10, 0.10, 0.10 }\n"
        "  fg[PRELIGHT]    = { 0.10, 0.10, 0.10 }\n"
        "  fg[ACTIVE]      = { 0.10, 0.10, 0.10 }\n"
        "  fg[SELECTED]    = { 1.00, 1.00, 1.00 }\n"
        "  base[NORMAL]    = { 1.00, 1.00, 1.00 }\n"
        "  base[SELECTED]  = { 0.30, 0.30, 0.30 }\n"
        "  text[NORMAL]    = { 0.10, 0.10, 0.10 }\n"
        "  text[SELECTED]  = { 1.00, 1.00, 1.00 }\n"
        "}\n"
        "class \"GtkWidget\" style \"lark_flat\"\n"
        "style \"lark_slim\" {\n"
        "  xthickness = 4\n"
        "  ythickness = 4\n"
        "}\n"
        "class \"GtkButton\"   style \"lark_slim\"\n"
        "class \"GtkFrame\"    style \"lark_slim\"\n"
        "class \"GtkTreeView\" style \"lark_slim\"\n");
    gtk_rc_parse_string(scroll_rc);
}
