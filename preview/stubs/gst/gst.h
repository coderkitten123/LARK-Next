/* Host-preview stub: just enough of gstreamer-0.10 for music_backend.h and the UI sources. */
#ifndef PREVIEW_STUB_GST_H
#define PREVIEW_STUB_GST_H

#include <glib.h>

typedef guint64 GstClockTime;
typedef gint64  GstClockTimeDiff;

#define GST_SECOND       ((GstClockTime)G_GINT64_CONSTANT(1000000000))
#define GST_MSECOND      ((GstClockTime)(GST_SECOND / 1000))
#define GST_CLOCK_TIME_NONE ((GstClockTime)-1)
#define GST_CLOCK_TIME_IS_VALID(t) ((GstClockTime)(t) != GST_CLOCK_TIME_NONE)

typedef struct _GstElement GstElement;
typedef struct _GstBus     GstBus;
typedef struct _GstMessage GstMessage;
typedef struct _GstClock   GstClock;

typedef enum { GST_FORMAT_UNDEFINED = 0, GST_FORMAT_TIME = 3 } GstFormat;
typedef enum {
    GST_STATE_VOID_PENDING = 0, GST_STATE_NULL = 1, GST_STATE_READY = 2,
    GST_STATE_PAUSED = 3, GST_STATE_PLAYING = 4
} GstState;
typedef enum {
    GST_MESSAGE_UNKNOWN = 0, GST_MESSAGE_EOS = 1, GST_MESSAGE_ERROR = 2
} GstMessageType;
#define GST_MESSAGE_TYPE(m) GST_MESSAGE_UNKNOWN

#endif
