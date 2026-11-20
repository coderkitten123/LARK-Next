#ifndef OPTIONS_UI_H
#define OPTIONS_UI_H

#include <gtk/gtk.h>

// Options screen (canva mockup 3): Bluetooth, Audiobookshelf login, sleep
// timer, display/backlight/suspend switches and Exit.
GtkWidget* options_page_new();

// Re-reads the current state (sleep timer mode may have changed while the
// screen was hidden). Called whenever the Options tab is shown.
void options_page_refresh();

#endif // OPTIONS_UI_H
