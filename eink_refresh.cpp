// =====================================================================
// eink_refresh.cpp - screen refreshes for Kindle PW4/KT4 (Rex driver)
//
// Sends MXCFB_SEND_UPDATE_REX ioctls (definitions from FBInk's
// eink/mxcfb-kindle.h). Two flavours:
//   eink_full_refresh()  - UPDATE_MODE_FULL + AUTO waveform: full repaint
//                          with flash; clears all ghosting.
//   eink_soft_refresh()  - UPDATE_MODE_PARTIAL + GC16_FAST over the whole
//                          screen: repaints stale pixels without flash.
// =====================================================================

#include "eink_refresh.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <glib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

// --- minimal mxcfb Rex definitions (from FBInk eink/mxcfb-kindle.h) ---

struct mxcfb_rect {
    uint32_t top;
    uint32_t left;
    uint32_t width;
    uint32_t height;
};

struct mxcfb_alt_buffer_data {
    uint32_t          phys_addr;
    uint32_t          width;
    uint32_t          height;
    struct mxcfb_rect alt_update_region;
};

struct mxcfb_update_data_rex {
    struct mxcfb_rect            update_region;
    uint32_t                     waveform_mode;
    uint32_t                     update_mode;
    uint32_t                     update_marker;
    int                          temp;
    unsigned int                 flags;
    int                          dither_mode;
    int                          quant_bit;
    struct mxcfb_alt_buffer_data alt_buffer_data;
    uint32_t                     hist_bw_waveform_mode;
    uint32_t                     hist_gray_waveform_mode;
};

// _IOW('F', 0x2E, struct mxcfb_update_data_rex)
#define MXCFB_SEND_UPDATE_REX _IOW('F', 0x2E, struct mxcfb_update_data_rex)

#define WAVEFORM_MODE_AUTO     257
#define WAVEFORM_MODE_GC16_FAST 0x3
#define UPDATE_MODE_PARTIAL 0x0
#define UPDATE_MODE_FULL    0x1
#define TEMP_USE_AMBIENT    0x1000

static void send_update(uint32_t waveform, uint32_t mode) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        g_print("eink_refresh: cannot open /dev/fb0\n");
        return;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        close(fd);
        return;
    }

    static uint32_t marker = 0;
    marker++;

    struct mxcfb_update_data_rex upd;
    memset(&upd, 0, sizeof(upd));
    upd.update_region.top    = 0;
    upd.update_region.left   = 0;
    upd.update_region.width  = vinfo.xres;
    upd.update_region.height = vinfo.yres;
    upd.waveform_mode        = waveform;
    upd.update_mode          = mode;
    upd.update_marker        = marker;
    upd.temp                 = TEMP_USE_AMBIENT;
    upd.flags                = 0;

    if (ioctl(fd, MXCFB_SEND_UPDATE_REX, &upd) != 0) {
        g_print("eink_refresh: ioctl failed (waveform %u)\n", waveform);
    }
    close(fd);
}

void eink_full_refresh() {
    send_update(WAVEFORM_MODE_AUTO, UPDATE_MODE_FULL);
}

void eink_soft_refresh() {
    send_update(WAVEFORM_MODE_GC16_FAST, UPDATE_MODE_PARTIAL);
}
