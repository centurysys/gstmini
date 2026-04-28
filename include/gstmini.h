#ifndef GSTMINI_H
#define GSTMINI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GstMiniPipeline GstMiniPipeline;

typedef enum GstMiniStatus {
    GSTMINI_OK = 0,
    GSTMINI_ERR = -1,
    GSTMINI_TIMEOUT = -2,
    GSTMINI_EOS = -3,
    GSTMINI_BAD_ARG = -4,
    GSTMINI_AGAIN = -5,
    GSTMINI_UNSUPPORTED = -6,
} GstMiniStatus;

typedef enum GstMiniEventType {
    GSTMINI_EVENT_NONE = 0,
    GSTMINI_EVENT_ERROR = 1,
    GSTMINI_EVENT_EOS = 2,
    GSTMINI_EVENT_STATE_CHANGED = 3,
    GSTMINI_EVENT_WARNING = 4,
    GSTMINI_EVENT_TIMEOUT = 5,
    GSTMINI_EVENT_UNKNOWN = 6,
} GstMiniEventType;

typedef enum GstMiniPixelFormat {
    GSTMINI_FORMAT_UNKNOWN = 0,
    GSTMINI_FORMAT_RGBX = 1,
    GSTMINI_FORMAT_RGBA = 2,
    GSTMINI_FORMAT_NV12 = 3,
    GSTMINI_FORMAT_JPEG = 4,
    GSTMINI_FORMAT_H264 = 5,
} GstMiniPixelFormat;

typedef struct GstMiniOpenOptions {
    const char *launch_desc;
    const char *appsrc_name;
    const char *appsink_name;
} GstMiniOpenOptions;

typedef struct GstMiniEvent {
    GstMiniEventType type;
    char message[512];
    char src_name[128];
    int old_state;
    int new_state;
    int pending_state;
} GstMiniEvent;

typedef struct GstMiniPlaneView {
    uint8_t *data;
    size_t size;
    int stride;
    size_t offset;
} GstMiniPlaneView;

typedef struct GstMiniFrameView {
    int width;
    int height;
    GstMiniPixelFormat format;
    int n_planes;
    size_t size;
    GstMiniPlaneView planes[4];

    /*
     * Buffer timing/position metadata.
     *
     * Time values are in nanoseconds.
     * If the underlying GstBuffer field is GST_CLOCK_TIME_NONE, gstmini stores -1.
     *
     * offset / offset_end are copied from GstBuffer.
     * If unknown, gstmini stores UINT64_MAX, matching GST_BUFFER_OFFSET_NONE.
     */
    int64_t pts_ns;
    int64_t dts_ns;
    int64_t duration_ns;
    uint64_t offset;
    uint64_t offset_end;

    /*
     * Opaque fields owned by gstmini.
     * Call gstmini_frame_view_release() exactly once after successful pull_view.
     * Do not read or write these fields from Nim or application code.
     */
    void *sample;
    void *buffer;
    void *map_info;
} GstMiniFrameView;

/*
 * Callback invoked when a wrapped appsrc buffer is finally released by GStreamer.
 *
 * The callback may be called from a GStreamer streaming thread. Keep it minimal;
 * for example, notify the application via pipe/eventfd and return immediately.
 */
typedef void (*GstMiniAppsrcReleaseCallback)(void *user_data);

void gstmini_init(void);
const char *gstmini_last_error(void);

GstMiniPipeline *gstmini_pipeline_open(const GstMiniOpenOptions *opts);
int gstmini_pipeline_start(GstMiniPipeline *p);
int gstmini_pipeline_stop(GstMiniPipeline *p);
void gstmini_pipeline_close(GstMiniPipeline *p);
int gstmini_pipeline_poll_event(GstMiniPipeline *p, GstMiniEvent *event, int timeout_ms);

/*
 * Push a copy of caller-owned memory into the named appsrc stored in GstMiniPipeline.
 *
 * data remains owned by the caller and may be reused immediately after this call returns.
 * pts_ns and duration_ns are nanoseconds. Pass a negative value to leave the GstBuffer
 * field as GST_CLOCK_TIME_NONE.
 */
int gstmini_appsrc_push_buffer(GstMiniPipeline *p, const uint8_t *data, size_t size,
    int64_t pts_ns, int64_t duration_ns);

/*
 * Push caller-owned memory into the named appsrc without copying it.
 *
 * data must remain valid until release_cb is invoked. gstmini never frees data.
 * release_cb may be NULL, but buffer-pool users should provide one to reclaim
 * the buffer after GStreamer has finished using it.
 */
int gstmini_appsrc_push_wrapped_buffer(GstMiniPipeline *p, uint8_t *data, size_t size,
    int64_t pts_ns, int64_t duration_ns,
    GstMiniAppsrcReleaseCallback release_cb, void *user_data);

/*
 * Push caller-owned video memory into appsrc without copying it and attach
 * GstVideoMeta describing the buffer layout.
 *
 * offsets and strides must point to arrays with at least n_planes entries.
 * data must remain valid until release_cb is invoked. gstmini never frees data.
 */
int gstmini_appsrc_push_wrapped_video_buffer(GstMiniPipeline *p, uint8_t *data, size_t size,
    GstMiniPixelFormat format, int width, int height, int n_planes,
    const size_t *offsets, const int *strides,
    int64_t pts_ns, int64_t duration_ns,
    GstMiniAppsrcReleaseCallback release_cb, void *user_data);

/* Send EOS to the named appsrc stored in GstMiniPipeline. */
int gstmini_appsrc_end_of_stream(GstMiniPipeline *p);

int gstmini_appsink_pull_view(GstMiniPipeline *p, GstMiniFrameView *view, int timeout_ms);
void gstmini_frame_view_release(GstMiniFrameView *view);

#ifdef __cplusplus
}
#endif

#endif
