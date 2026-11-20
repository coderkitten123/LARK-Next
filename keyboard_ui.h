#ifndef KEYBOARD_UI_H
#define KEYBOARD_UI_H

#include <gtk/gtk.h>

// On-screen keyboard screen. It is a page of the main window (no separate
// window, no nested main loop), opened with app_keyboard() from app_actions.h.
GtkWidget* keyboard_page_new();

#endif // KEYBOARD_UI_H
