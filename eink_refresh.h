#ifndef EINK_REFRESH_H
#define EINK_REFRESH_H

// Full-screen e-ink refresh with a GC16 flash waveform. Use sparingly:
// on track changes, book finish and display-mode toggles. Clears ghosting.
void eink_full_refresh();

// Full-screen repaint WITHOUT a flash (GC16-fast partial waveform over the
// whole screen). Cheap enough to use for recovering the screen after the
// Kindle framework (Quick Settings drawer, screensaver, dialog overlays)
// painted over our framebuffer.
void eink_soft_refresh();

#endif
