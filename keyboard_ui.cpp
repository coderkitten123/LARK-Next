// =================================================================================
// keyboard_ui.cpp - on-screen keyboard (ink style)
//
// Lives inside the main window as a notebook page instead of a dialog window:
// a second top-level window is not treated as part of the app by the Kindle
// window manager, and a nested dialog loop is fragile. Text length is not
// limited, so long API keys fit.
// =================================================================================

#include <functional>
#include <string>
#include <vector>

#include "keyboard_ui.h"
#include "app_actions.h"
#include "ui_kit.h"

namespace {

typedef std::function<void(bool, const std::string&)> KbDone;

struct Kb {
    std::string text;
    bool caps = false;
    bool symbols = false;
    bool password = false;
    bool reveal = false;
    bool open = false;
    KbDone done;

    GtkWidget* caption = NULL;
    GtkWidget* textbox = NULL;
    GtkWidget* alpha = NULL;      // letter keys
    GtkWidget* sym = NULL;        // symbol keys
    GtkWidget* shift = NULL;
    GtkWidget* show_btn = NULL;
    std::vector<GtkWidget*> letters;
};
Kb kb;

const int KEY_H = 108;
const int KEY_W = 98;
const int KEY_GAP = 6;

void refresh_text() {
    std::string shown;
    if (kb.password && !kb.reveal) {
        for (const char* p = kb.text.c_str(); *p; p = g_utf8_next_char(p)) shown += "\xE2\x80\xA2";
    } else {
        shown = kb.text;
    }
    ui_textbox_set(kb.textbox, shown + "|");
}

void apply_caps() {
    for (size_t i = 0; i < kb.letters.size(); i++) {
        std::string t = ui_button_get_text(kb.letters[i]);
        if (t.empty()) continue;
        t[0] = kb.caps ? g_ascii_toupper(t[0]) : g_ascii_tolower(t[0]);
        ui_button_set_text(kb.letters[i], t);
    }
    ui_button_set_text(kb.shift, kb.caps ? "SHIFT" : "Shift");
}

void apply_symbols() {
    if (kb.symbols) { gtk_widget_hide(kb.alpha); gtk_widget_show(kb.sym); }
    else            { gtk_widget_show(kb.alpha); gtk_widget_hide(kb.sym); }
}

void finish(bool ok) {
    if (!kb.open) return;
    kb.open = false;
    KbDone cb = kb.done;
    kb.done = NULL;
    std::string text = kb.text;
    app_set_keyboard_visible(false);
    if (cb) cb(ok, text);
}

// --- key callbacks (run from idle, never inside the press handler) --------------

void on_key(GtkWidget* w, gpointer is_letter) {
    kb.text += ui_button_get_text(w);
    if (GPOINTER_TO_INT(is_letter) && kb.caps) {   // shift is one-shot
        kb.caps = false;
        apply_caps();
    }
    refresh_text();
}

void on_space(GtkWidget*, gpointer) { kb.text += " "; refresh_text(); }

void on_backspace(GtkWidget*, gpointer) {
    if (kb.text.empty()) return;
    const char* s = kb.text.c_str();
    const char* last = g_utf8_find_prev_char(s, s + kb.text.size());
    kb.text.erase(last ? (size_t)(last - s) : 0);
    refresh_text();
}

void on_shift(GtkWidget*, gpointer) { kb.caps = !kb.caps; apply_caps(); }

void on_symbols(GtkWidget*, gpointer) { kb.symbols = !kb.symbols; apply_symbols(); }

void on_clear(GtkWidget*, gpointer) { kb.text.clear(); refresh_text(); }

void on_show(GtkWidget* w, gpointer) {
    kb.reveal = !kb.reveal;
    ui_button_set_text(w, kb.reveal ? "Hide" : "Show");
    refresh_text();
}

void on_cancel(GtkWidget*, gpointer) { finish(false); }
void on_done(GtkWidget*, gpointer) { finish(true); }

// --- layout ------------------------------------------------------------------------

GtkWidget* key(const std::string& label, int mock_w, UiCb cb, gpointer data, bool letter = false) {
    GtkWidget* b = ui_button_new(label, 38, mock_w, KEY_H, 3, 10);
    ui_connect(b, cb, data);
    if (letter) kb.letters.push_back(b);
    return b;
}

// One row of keys, centered
GtkWidget* row_of(const std::vector<GtkWidget*>& keys) {
    GtkWidget* row = gtk_hbox_new(FALSE, ui_px(KEY_GAP));
    for (size_t i = 0; i < keys.size(); i++)
        gtk_box_pack_start(GTK_BOX(row), keys[i], FALSE, FALSE, 0);
    GtkWidget* a = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(a), row);
    return a;
}

std::vector<GtkWidget*> chars_row(const char* chars, bool letters) {
    std::vector<GtkWidget*> v;
    for (int i = 0; chars[i]; i++) {
        char l[2] = { chars[i], 0 };
        v.push_back(key(l, KEY_W, on_key, GINT_TO_POINTER(letters ? 1 : 0), letters));
    }
    return v;
}

void append(std::vector<GtkWidget*>& to, const std::vector<GtkWidget*>& from) {
    to.insert(to.end(), from.begin(), from.end());
}

GtkWidget* rows_box(const std::vector<GtkWidget*>& rows) {
    GtkWidget* box = gtk_vbox_new(FALSE, ui_px(8));
    for (size_t i = 0; i < rows.size(); i++)
        gtk_box_pack_start(GTK_BOX(box), rows[i], FALSE, FALSE, 0);
    return box;
}

GtkWidget* build_alpha() {
    std::vector<GtkWidget*> rows;
    rows.push_back(row_of(chars_row("1234567890", false)));
    rows.push_back(row_of(chars_row("qwertyuiop", true)));
    rows.push_back(row_of(chars_row("asdfghjkl", true)));

    std::vector<GtkWidget*> r4;
    kb.shift = key("Shift", 150, on_shift, NULL);
    r4.push_back(kb.shift);
    append(r4, chars_row("zxcvbnm", true));
    r4.push_back(key("Del", 150, on_backspace, NULL));
    rows.push_back(row_of(r4));

    std::vector<GtkWidget*> r5;
    r5.push_back(key("?123", 150, on_symbols, NULL));
    append(r5, chars_row("@.", false));
    r5.push_back(key("space", 272, on_space, NULL));
    append(r5, chars_row("-_/:", false));
    rows.push_back(row_of(r5));
    return rows_box(rows);
}

GtkWidget* build_symbols() {
    std::vector<GtkWidget*> rows;
    rows.push_back(row_of(chars_row("1234567890", false)));
    rows.push_back(row_of(chars_row("@#$%&*+=()", false)));
    rows.push_back(row_of(chars_row("!?\"';:~^_-", false)));

    std::vector<GtkWidget*> r4;
    append(r4, chars_row("[]{}<>\\|", false));
    r4.push_back(key("Del", 150, on_backspace, NULL));
    rows.push_back(row_of(r4));

    std::vector<GtkWidget*> r5;
    r5.push_back(key("abc", 150, on_symbols, NULL));
    append(r5, chars_row(",.", false));
    r5.push_back(key("space", 376, on_space, NULL));
    append(r5, chars_row("/`", false));
    rows.push_back(row_of(r5));
    return rows_box(rows);
}

}  // namespace

GtkWidget* keyboard_page_new() {
    GtkWidget* page = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), ui_inset_new(), FALSE, FALSE, 0);

    kb.caption = ui_label_new("", 40, true);
    GtkWidget* cap_a = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(cap_a), kb.caption);
    gtk_widget_set_size_request(cap_a, -1, ui_px(84));
    gtk_box_pack_start(GTK_BOX(page), cap_a, FALSE, FALSE, 0);

    // Action row: Cancel | Clear | Show (passwords) | Done
    GtkWidget* actions = gtk_hbox_new(FALSE, ui_px(16));
    GtkWidget* cancel = ui_button_new("Cancel", 30, 230, 78, 4, 10);
    ui_connect(cancel, on_cancel, NULL);
    GtkWidget* clear = ui_button_new("Clear", 30, 230, 78, 4, 10);
    ui_connect(clear, on_clear, NULL);
    kb.show_btn = ui_button_new("Show", 30, 230, 78, 4, 10);
    ui_connect(kb.show_btn, on_show, NULL);
    GtkWidget* done = ui_button_new("Done", 30, 230, 78, 5, 10, true);
    ui_connect(done, on_done, NULL);
    gtk_box_pack_start(GTK_BOX(actions), cancel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), kb.show_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), done, FALSE, FALSE, 0);
    GtkWidget* act_a = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(act_a), actions);
    gtk_box_pack_start(GTK_BOX(page), act_a, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), ui_vspace(16), FALSE, FALSE, 0);

    // Text being edited: takes whatever height the keys leave over
    kb.textbox = ui_textbox_new(36, 200);
    GtkWidget* tb_pad = gtk_alignment_new(0, 0, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(tb_pad), 0, 0, ui_px(24), ui_px(24));
    gtk_container_add(GTK_CONTAINER(tb_pad), kb.textbox);
    gtk_box_pack_start(GTK_BOX(page), tb_pad, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), ui_vspace(16), FALSE, FALSE, 0);

    kb.letters.clear();
    kb.alpha = build_alpha();
    kb.sym = build_symbols();
    GtkWidget* keys = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(keys), kb.alpha, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(keys), kb.sym, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), keys, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), ui_vspace(20), FALSE, FALSE, 0);

    gtk_widget_show_all(page);
    gtk_widget_hide(kb.sym);
    return page;
}

void app_keyboard(const std::string& caption, const std::string& initial, bool password,
                  std::function<void(bool ok, const std::string& text)> done) {
    if (kb.open) return;
    kb.open = true;
    kb.done = done;
    kb.text = initial;
    kb.caps = false;
    kb.symbols = false;
    kb.password = password;
    kb.reveal = false;
    ui_label_set(kb.caption, caption);
    ui_button_set_text(kb.show_btn, "Show");
    if (password) gtk_widget_show(kb.show_btn); else gtk_widget_hide(kb.show_btn);
    apply_caps();
    apply_symbols();
    refresh_text();
    app_set_keyboard_visible(true);
}
