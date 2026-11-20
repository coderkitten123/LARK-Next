#ifndef UI_KIT_H
#define UI_KIT_H

#include <gtk/gtk.h>
#include <string>
#include <vector>

// =================================================================================
// ui_kit - "black ink on white paper" widget kit for the e-ink UI.
//
// Everything is laid out in MOCKUP pixels (the canva mockups are 1072x1448, the
// Kindle Paperwhite panel) and scaled to the real screen by ui_px(). Controls
// are cairo-drawn GtkDrawingAreas (no GTK theme bevels leaking through), with
// thick black outlines and flat white fills.
//
// Taps activate on release (only if the finger barely moved, so kinetic
// scrolling over a button never fires it) and the callback runs from an idle
// handler, so a callback may safely destroy/rebuild the widget it came from.
// =================================================================================

typedef void (*UiCb)(GtkWidget* widget, gpointer user_data);

// --- scaling / fonts ---------------------------------------------------------

// Screen scale relative to the 1072x1448 mockup.
double ui_scale();
// Mockup pixels -> screen pixels (never 0 for non-zero input).
int ui_px(int mock_px);

// Preferred UI font family: the Kindle's own (Amazon Ember / Bookerly) when
// installed, plain "Sans" otherwise. Cached after the first call.
const char* lark_ui_font();

// --- labels --------------------------------------------------------------------

// Text label drawn with Pango at an absolute pixel size (mockup px). GTK2's own
// wrapped labels ignore horizontal alignment, so the kit draws text itself.
// wrap_w > 0 wraps at that width (mockup px) and max_lines > 0 ends the text
// with "..." after that many lines. gray = 0 (black) .. 255 (white).
GtkWidget* ui_label_new(const std::string& text, int font_px, bool bold = false,
                        bool italic = false, double xalign = 0.5, int wrap_w = 0,
                        int gray = 26, int max_lines = 0);
// Sets text only when it changed (every redraw costs an e-ink refresh).
void ui_label_set(GtkWidget* label, const std::string& text);
// Re-styles an existing label.
void ui_label_style(GtkWidget* label, int font_px, bool bold = false,
                    bool italic = false, int gray = 26);

// Space reserved at the top of every screen for the Kindle's own status bar
// (clock, wifi, battery), which overlays the app window. ~66 mockup px unless
// the window manager already places the window below the bar.
GtkWidget* ui_inset_new();
int ui_status_inset();                 // current inset in screen px
void ui_set_status_inset(int px);      // updates every ui_inset_new() spacer

// Screen size in pixels (honours the LARK_WINDOW_SIZE=WxH preview override).
void ui_screen_size(int* w, int* h);

// Blank spacer of a fixed size (mockup px).
GtkWidget* ui_vspace(int mock_h);
GtkWidget* ui_hspace(int mock_w);
// Solid black rule (mockup px thickness).
GtkWidget* ui_rule(int mock_px);

// --- drawn controls -------------------------------------------------------------

enum UiIcon {
    UI_ICON_NONE = 0,
    UI_ICON_PLAY,
    UI_ICON_PAUSE,
    UI_ICON_REWIND,
    UI_ICON_FORWARD,
    UI_ICON_BOOKS,
    UI_ICON_DOTS
};

// Outlined button. border/radius in mockup px. Text is centered unless an icon
// is set with ui_button_set_icon (icon at the left edge, text right-aligned).
GtkWidget* ui_button_new(const std::string& text, int font_px, int mock_w, int mock_h,
                         int border = 3, int radius = 0, bool bold = false);
void ui_button_set_text(GtkWidget* button, const std::string& text);
const std::string& ui_button_get_text(GtkWidget* button);
void ui_button_set_icon(GtkWidget* button, UiIcon icon, int icon_h);
void ui_button_set_visible_border(GtkWidget* button, bool visible);

// Borderless big transport glyph (rewind / play / forward ...).
GtkWidget* ui_glyph_button_new(UiIcon icon, int glyph_h, int mock_w, int mock_h);
void ui_glyph_set_icon(GtkWidget* glyph, UiIcon icon);

// Switch (pill with knob).
GtkWidget* ui_toggle_new(bool on);
void ui_toggle_set(GtkWidget* toggle, bool on);
bool ui_toggle_get(GtkWidget* toggle);

// Full-width radio row ("( ) Label"). The caller keeps selection state.
GtkWidget* ui_radio_new(const std::string& text, int font_px, int mock_h, bool selected);
void ui_radio_set(GtkWidget* radio, bool selected);

// List row: text at the left, optional right-aligned text, hairline underneath.
// highlighted rows are shaded and bold (e.g. the current chapter).
GtkWidget* ui_row_new(const std::string& left, const std::string& right, int font_px,
                      int mock_h, bool highlighted);

// Boxed text field showing a value; tapping it fires the callback (the caller
// opens the on-screen keyboard). password shows bullets instead of the text.
GtkWidget* ui_field_new(const std::string& value, const std::string& placeholder,
                        int font_px, int mock_h, bool password = false);
void ui_field_set(GtkWidget* field, const std::string& value);

// Boxed, wrapping, multi-line text display (used by the keyboard screen).
GtkWidget* ui_textbox_new(int font_px, int mock_h);
void ui_textbox_set(GtkWidget* box, const std::string& text);

// Connect the tap callback of any widget above.
void ui_connect(GtkWidget* w, UiCb cb, gpointer user_data);

// Bottom tab bar (Play / Library / Options): three framed cells, icon + label.
// cb receives the tapped tab index through GPOINTER_TO_INT(user_data).
GtkWidget* ui_tabbar_new(int mock_h, UiCb cb);
void ui_tabbar_set_active(GtkWidget* bar, int index);

// Full-screen modal list picker (title bar + Close + scrolling rows). Returns the
// tapped row index, or -1 when closed. right_texts may be empty or shorter than
// rows. The row at `highlight` is shaded and scrolled into view.
int ui_pick_dialog(GtkWindow* parent, const std::string& title,
                   const std::vector<std::string>& rows,
                   const std::vector<std::string>& right_texts, int highlight);

// --- scrolling ---------------------------------------------------------------------

// Vertical scroll area with finger-drag kinetic scrolling and a thin scroll
// indicator. `content` is added inside a viewport.
GtkWidget* ui_scroll_new(GtkWidget* content);
GtkAdjustment* ui_scroll_adjustment(GtkWidget* scroll);
void ui_scroll_to_top(GtkWidget* scroll);

// Install the global GTK rc style (flat white theme, thin scroll bars).
void ui_install_theme();

#endif // UI_KIT_H
