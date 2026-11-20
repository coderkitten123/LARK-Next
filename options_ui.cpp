// =================================================================================
// options_ui.cpp - Options screen (canva mockup 3)
//
// Text entry goes through the on-screen keyboard dialog: tapping a field opens
// it, and the new value is saved straight away. Settings keys:
//   abs_enabled  "1"/"0"   Audiobookshelf library on/off (off = local files)
//   abs_server   http://host:port
//   abs_user / abs_pass    username + password login
//   abs_token              API key (used instead of the login when set)
// =================================================================================

#include <string>
#include <vector>

#include "options_ui.h"
#include "app_actions.h"
#include "abs_ui.h"
#include "database_manager.h"
#include "ui_kit.h"

extern DatabaseManager db;

static GtkWidget* g_abs_toggle = NULL;
static GtkWidget* g_abs_box = NULL;          // server/login fields (hidden when ABS is off)
static GtkWidget* g_status = NULL;
static std::vector<GtkWidget*> g_radios;
static GtkWidget* g_refresh_toggle = NULL;
static GtkWidget* g_backlight_toggle = NULL;
static GtkWidget* g_suspend_toggle = NULL;

enum { F_SERVER = 0, F_USER, F_PASS, F_KEY, F_COUNT };
static GtkWidget* g_fields[F_COUNT];
static const char* kFieldKeys[F_COUNT] = { "abs_server", "abs_user", "abs_pass", "abs_token" };
static const char* kFieldCaptions[F_COUNT] = { "Server address", "Username", "Password", "API key" };
static const char* kFieldHints[F_COUNT] = { "http://192.168.0.10:13378", "username", "password",
                                            "paste an API key instead of logging in" };

static void set_status(const std::string& text) {
    if (g_status) ui_label_set(g_status, text);
}

// ---------------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------------

static void on_bluetooth(GtkWidget*, gpointer) { app_open_bluetooth_settings(); }
static void on_exit(GtkWidget*, gpointer) { app_exit(); }

static void apply_abs_visibility() {
    if (ui_toggle_get(g_abs_toggle)) gtk_widget_show(g_abs_box);
    else gtk_widget_hide(g_abs_box);
}

static void on_abs_toggle(GtkWidget* w, gpointer) {
    db.setSetting("abs_enabled", ui_toggle_get(w) ? "1" : "0");
    apply_abs_visibility();
    abs_settings_changed();
    app_schedule_refresh(true);
}

static void on_field(GtkWidget*, gpointer idx) {
    int i = GPOINTER_TO_INT(idx);
    std::string cur = db.getSetting(kFieldKeys[i], "");
    if (i == F_SERVER && cur.empty()) cur = "http://";
    app_keyboard(kFieldCaptions[i], cur, i == F_PASS, [i](bool ok, const std::string& value) {
        if (!ok) return;
        std::string text = value;
        if (i == F_SERVER && text == "http://") text.clear();
        if (text == db.getSetting(kFieldKeys[i], "")) return;
        db.setSetting(kFieldKeys[i], text);
        ui_field_set(g_fields[i], text);
        abs_settings_changed();
        set_status("Tap \"Test connection\" to check the new details.");
    });
}

static void test_done(bool ok, const std::string& msg, gpointer) {
    (void)ok;
    set_status(msg);
}

static void on_test(GtkWidget*, gpointer) {
    set_status("Connecting...");
    abs_test_connection(test_done, NULL);
}

static void refresh_radios() {
    for (size_t i = 0; i < g_radios.size(); i++)
        ui_radio_set(g_radios[i], (int)i == app_sleep_mode());
}

static void on_radio(GtkWidget*, gpointer idx) {
    app_set_sleep_mode(GPOINTER_TO_INT(idx));
    refresh_radios();
}

static void on_refresh_toggle(GtkWidget* w, gpointer) { app_set_refresh_disabled(ui_toggle_get(w)); }
static void on_backlight_toggle(GtkWidget* w, gpointer) { app_set_backlight_disabled(ui_toggle_get(w)); }
static void on_suspend_toggle(GtkWidget* w, gpointer) { app_set_auto_suspend(ui_toggle_get(w)); }

// ---------------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------------

// "Label ............ [widget]" in a fixed-height row
static void add_row(GtkWidget* box, const std::string& label, GtkWidget* right, int mock_h) {
    GtkWidget* row = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), ui_label_new(label, 30, false, false, 0.0), TRUE, TRUE, 0);
    GtkWidget* a = gtk_alignment_new(1, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(a), right);
    gtk_box_pack_start(GTK_BOX(row), a, FALSE, FALSE, 0);
    GtkWidget* slot = gtk_alignment_new(0, 0.5, 1, 0);
    gtk_container_add(GTK_CONTAINER(slot), row);
    gtk_widget_set_size_request(slot, -1, ui_px(mock_h));
    gtk_box_pack_start(GTK_BOX(box), slot, FALSE, FALSE, 0);
}

static void add_caption(GtkWidget* box, const std::string& text, int mock_h = 50) {
    GtkWidget* l = ui_label_new(text, 28, false, false, 0.0);
    GtkWidget* slot = gtk_alignment_new(0, 0.5, 1, 0);
    gtk_container_add(GTK_CONTAINER(slot), l);
    gtk_widget_set_size_request(slot, -1, ui_px(mock_h));
    gtk_box_pack_start(GTK_BOX(box), slot, FALSE, FALSE, 0);
}

static void add_field(GtkWidget* box, int idx, const char* caption) {
    add_caption(box, caption);
    std::string v = db.getSetting(kFieldKeys[idx], "");
    g_fields[idx] = ui_field_new(v, kFieldHints[idx], 28, 62, idx == F_PASS);
    ui_connect(g_fields[idx], on_field, GINT_TO_POINTER(idx));
    gtk_box_pack_start(GTK_BOX(box), g_fields[idx], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui_vspace(10), FALSE, FALSE, 0);
}

// ---------------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------------

GtkWidget* options_page_new() {
    GtkWidget* page = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), ui_inset_new(), FALSE, FALSE, 0);

    GtkWidget* col = gtk_vbox_new(FALSE, 0);

    // Bluetooth
    GtkWidget* bt = ui_button_new("Open built-in Bluetooth Settings", 28, 480, 66, 3, 0);
    ui_connect(bt, on_bluetooth, NULL);
    add_row(col, "Bluetooth", bt, 90);

    // Audiobookshelf
    bool abs_on = abs_settings_enabled();
    g_abs_toggle = ui_toggle_new(abs_on);
    ui_connect(g_abs_toggle, on_abs_toggle, NULL);
    add_row(col, "AudioBookShelf", g_abs_toggle, 84);

    g_abs_box = gtk_vbox_new(FALSE, 0);
    add_field(g_abs_box, F_SERVER, "AudioBookShelf server:");
    add_field(g_abs_box, F_USER, "Username:");
    add_field(g_abs_box, F_PASS, "Password:");
    add_field(g_abs_box, F_KEY, "API key (optional, replaces username/password):");

    GtkWidget* test_row = gtk_hbox_new(FALSE, ui_px(20));
    GtkWidget* test = ui_button_new("Test connection", 28, 300, 62, 3, 0);
    ui_connect(test, on_test, NULL);
    gtk_box_pack_start(GTK_BOX(test_row), test, FALSE, FALSE, 0);
    g_status = ui_label_new("", 24, false, true, 0.0, 600, 77, 2);
    gtk_box_pack_start(GTK_BOX(test_row), g_status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(g_abs_box), test_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_abs_box), ui_vspace(24), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), g_abs_box, FALSE, FALSE, 0);

    // Sleep timer
    gtk_box_pack_start(GTK_BOX(col), ui_vspace(16), FALSE, FALSE, 0);
    add_caption(col, "Sleep timer", 64);
    g_radios.clear();
    for (int i = 0; i < app_sleep_option_count(); i++) {
        GtkWidget* r = ui_radio_new(app_sleep_option(i).label, 30, 62, i == app_sleep_mode());
        ui_connect(r, on_radio, GINT_TO_POINTER(i));
        g_radios.push_back(r);
        gtk_box_pack_start(GTK_BOX(col), r, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(col), ui_vspace(20), FALSE, FALSE, 0);

    // Switches
    g_refresh_toggle = ui_toggle_new(app_refresh_disabled());
    ui_connect(g_refresh_toggle, on_refresh_toggle, NULL);
    add_row(col, "Disable screen refresh", g_refresh_toggle, 82);

    g_backlight_toggle = ui_toggle_new(app_backlight_disabled());
    ui_connect(g_backlight_toggle, on_backlight_toggle, NULL);
    add_row(col, "Disable Backlight", g_backlight_toggle, 82);

    g_suspend_toggle = ui_toggle_new(app_auto_suspend());
    ui_connect(g_suspend_toggle, on_suspend_toggle, NULL);
    add_row(col, "Auto suspend 10m after sleep timer", g_suspend_toggle, 82);

    // Exit
    GtkWidget* ex = ui_button_new("Exit", 28, 480, 66, 3, 0);
    ui_connect(ex, on_exit, NULL);
    add_row(col, "Exit AudioBookPlayer", ex, 100);
    gtk_box_pack_start(GTK_BOX(col), ui_vspace(30), FALSE, FALSE, 0);

    GtkWidget* pad = gtk_alignment_new(0, 0, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(pad), 0, 0, ui_px(40), ui_px(40));
    gtk_container_add(GTK_CONTAINER(pad), col);
    gtk_box_pack_start(GTK_BOX(page), ui_scroll_new(pad), TRUE, TRUE, 0);
    return page;
}

void options_page_refresh() {
    refresh_radios();
    ui_toggle_set(g_abs_toggle, abs_settings_enabled());
    apply_abs_visibility();
}
