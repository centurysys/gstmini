#include "gstmini.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct GstMiniPipeline {
    GstElement *pipeline;
    GstElement *appsrc;
    GstElement *appsink;
};

typedef struct GstMiniMapHolder {
    GstMapInfo map;
} GstMiniMapHolder;

static char g_last_error[512];

static void set_error(const char *msg)
{
    if (msg == NULL) msg = "unknown error";
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}

static void set_gerror(const char *prefix, const GError *err)
{
    if (err && err->message) {
        snprintf(g_last_error, sizeof(g_last_error), "%s: %s", prefix, err->message);
    } else {
        snprintf(g_last_error, sizeof(g_last_error), "%s", prefix);
    }
}

static int64_t clock_time_to_i64(GstClockTime value)
{
    if (!GST_CLOCK_TIME_IS_VALID(value)) {
        return -1;
    }
    if (value > (GstClockTime)INT64_MAX) {
        return INT64_MAX;
    }
    return (int64_t)value;
}

static GstClockTime i64_to_clock_time(int64_t value)
{
    if (value < 0) {
        return GST_CLOCK_TIME_NONE;
    }
    return (GstClockTime)value;
}

static void event_clear(GstMiniEvent *event)
{
    if (!event) return;
    memset(event, 0, sizeof(*event));
    event->type = GSTMINI_EVENT_NONE;
}

static void view_clear(GstMiniFrameView *view)
{
    if (!view) return;
    memset(view, 0, sizeof(*view));
    view->format = GSTMINI_FORMAT_UNKNOWN;
    view->pts_ns = -1;
    view->dts_ns = -1;
    view->duration_ns = -1;
    view->offset = UINT64_MAX;
    view->offset_end = UINT64_MAX;
}

static void copy_gst_object_name(char *dst, size_t dst_size, GstObject *obj)
{
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!obj) return;

    const gchar *name = gst_object_get_name(obj);
    if (name) {
        snprintf(dst, dst_size, "%s", name);
    }
}

static GstMiniPixelFormat gstmini_format_from_video_format(GstVideoFormat fmt)
{
    switch (fmt) {
    case GST_VIDEO_FORMAT_RGBx:
        return GSTMINI_FORMAT_RGBX;
    case GST_VIDEO_FORMAT_RGBA:
        return GSTMINI_FORMAT_RGBA;
    case GST_VIDEO_FORMAT_NV12:
        return GSTMINI_FORMAT_NV12;
    default:
        return GSTMINI_FORMAT_UNKNOWN;
    }
}

static int status_from_flow_return(GstFlowReturn flow)
{
    switch (flow) {
    case GST_FLOW_OK:
        return GSTMINI_OK;
    case GST_FLOW_EOS:
        set_error("appsrc returned EOS");
        return GSTMINI_EOS;
    case GST_FLOW_FLUSHING:
        set_error("appsrc is flushing");
        return GSTMINI_AGAIN;
    case GST_FLOW_NOT_NEGOTIATED:
        set_error("appsrc not negotiated");
        return GSTMINI_ERR;
    case GST_FLOW_ERROR:
        set_error("appsrc flow error");
        return GSTMINI_ERR;
    case GST_FLOW_NOT_LINKED:
        set_error("appsrc not linked");
        return GSTMINI_ERR;
    default:
        set_error("appsrc unexpected flow return");
        return GSTMINI_ERR;
    }
}

static void gstmini_fill_view_buffer_metadata(GstMiniFrameView *view, GstBuffer *buffer)
{
    if (!view || !buffer) return;

    view->pts_ns = clock_time_to_i64(GST_BUFFER_PTS(buffer));
    view->dts_ns = clock_time_to_i64(GST_BUFFER_DTS(buffer));
    view->duration_ns = clock_time_to_i64(GST_BUFFER_DURATION(buffer));
    view->offset = GST_BUFFER_OFFSET(buffer);
    view->offset_end = GST_BUFFER_OFFSET_END(buffer);
}

static int gstmini_fill_view_from_video_info(
    GstMiniFrameView *view,
    const GstVideoInfo *info,
    GstMapInfo *map
)
{
    if (!view || !info || !map || !map->data) {
        set_error("bad argument for video view");
        return GSTMINI_BAD_ARG;
    }

    const int n_planes = GST_VIDEO_INFO_N_PLANES(info);
    if (n_planes <= 0 || n_planes > 4) {
        set_error("unsupported number of planes");
        return GSTMINI_UNSUPPORTED;
    }

    view->width = GST_VIDEO_INFO_WIDTH(info);
    view->height = GST_VIDEO_INFO_HEIGHT(info);
    view->format = gstmini_format_from_video_format(GST_VIDEO_INFO_FORMAT(info));
    view->n_planes = n_planes;
    view->size = map->size;

    if (view->format == GSTMINI_FORMAT_UNKNOWN) {
        set_error("unsupported video format");
        return GSTMINI_UNSUPPORTED;
    }

    for (int i = 0; i < n_planes; i++) {
        const size_t offset = (size_t)GST_VIDEO_INFO_PLANE_OFFSET(info, i);
        const int stride = GST_VIDEO_INFO_PLANE_STRIDE(info, i);

        if (offset > map->size) {
            set_error("plane offset exceeds buffer size");
            return GSTMINI_ERR;
        }

        view->planes[i].data = map->data + offset;
        view->planes[i].offset = offset;
        view->planes[i].stride = stride;

        if (i + 1 < n_planes) {
            const size_t next_offset = (size_t)GST_VIDEO_INFO_PLANE_OFFSET(info, i + 1);
            if (next_offset >= offset && next_offset <= map->size) {
                view->planes[i].size = next_offset - offset;
            } else {
                view->planes[i].size = map->size - offset;
            }
        } else {
            view->planes[i].size = map->size - offset;
        }
    }

    return GSTMINI_OK;
}

void gstmini_init(void)
{
    static int initialized = 0;
    if (!initialized) {
        gst_init(NULL, NULL);
        initialized = 1;
    }
}

const char *gstmini_last_error(void)
{
    return g_last_error;
}

GstMiniPipeline *gstmini_pipeline_open(const GstMiniOpenOptions *opts)
{
    if (!opts || !opts->launch_desc || opts->launch_desc[0] == '\0') {
        set_error("launch_desc is empty");
        return NULL;
    }

    gstmini_init();

    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(opts->launch_desc, &err);
    if (!pipeline) {
        set_gerror("gst_parse_launch failed", err);
        if (err) g_error_free(err);
        return NULL;
    }
    if (err) {
        set_gerror("gst_parse_launch warning", err);
        g_error_free(err);
    }

    GstMiniPipeline *p = (GstMiniPipeline *)calloc(1, sizeof(*p));
    if (!p) {
        set_error("calloc failed");
        gst_object_unref(pipeline);
        return NULL;
    }

    p->pipeline = pipeline;

    if (opts->appsrc_name && opts->appsrc_name[0] != '\0') {
        p->appsrc = gst_bin_get_by_name(GST_BIN(pipeline), opts->appsrc_name);
        if (!p->appsrc) {
            set_error("failed to get appsrc by name");
            gstmini_pipeline_close(p);
            return NULL;
        }
        if (!GST_IS_APP_SRC(p->appsrc)) {
            set_error("named appsrc element is not an appsrc");
            gstmini_pipeline_close(p);
            return NULL;
        }
    }

    if (opts->appsink_name && opts->appsink_name[0] != '\0') {
        p->appsink = gst_bin_get_by_name(GST_BIN(pipeline), opts->appsink_name);
        if (!p->appsink) {
            set_error("failed to get appsink by name");
            gstmini_pipeline_close(p);
            return NULL;
        }
        if (!GST_IS_APP_SINK(p->appsink)) {
            set_error("named appsink element is not an appsink");
            gstmini_pipeline_close(p);
            return NULL;
        }
    }

    return p;
}

int gstmini_pipeline_start(GstMiniPipeline *p)
{
    if (!p || !p->pipeline) {
        set_error("pipeline is null");
        return GSTMINI_BAD_ARG;
    }

    GstStateChangeReturn ret = gst_element_set_state(p->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        set_error("failed to set pipeline to PLAYING");
        return GSTMINI_ERR;
    }

    return GSTMINI_OK;
}

int gstmini_pipeline_stop(GstMiniPipeline *p)
{
    if (!p || !p->pipeline) {
        set_error("pipeline is null");
        return GSTMINI_BAD_ARG;
    }

    GstStateChangeReturn ret = gst_element_set_state(p->pipeline, GST_STATE_NULL);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        set_error("failed to set pipeline to NULL");
        return GSTMINI_ERR;
    }

    return GSTMINI_OK;
}

void gstmini_pipeline_close(GstMiniPipeline *p)
{
    if (!p) return;

    if (p->pipeline) {
        gst_element_set_state(p->pipeline, GST_STATE_NULL);
    }
    if (p->appsrc) {
        gst_object_unref(p->appsrc);
        p->appsrc = NULL;
    }
    if (p->appsink) {
        gst_object_unref(p->appsink);
        p->appsink = NULL;
    }
    if (p->pipeline) {
        gst_object_unref(p->pipeline);
        p->pipeline = NULL;
    }

    free(p);
}

int gstmini_pipeline_poll_event(GstMiniPipeline *p, GstMiniEvent *event, int timeout_ms)
{
    if (!p || !p->pipeline || !event) {
        set_error("bad argument");
        return GSTMINI_BAD_ARG;
    }

    event_clear(event);

    GstBus *bus = gst_element_get_bus(p->pipeline);
    if (!bus) {
        set_error("failed to get pipeline bus");
        return GSTMINI_ERR;
    }

    GstClockTime timeout;
    if (timeout_ms < 0) {
        timeout = GST_CLOCK_TIME_NONE;
    } else {
        timeout = (GstClockTime)timeout_ms * GST_MSECOND;
    }

    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus,
        timeout,
        GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_WARNING | GST_MESSAGE_STATE_CHANGED
    );

    gst_object_unref(bus);

    if (!msg) {
        event->type = GSTMINI_EVENT_TIMEOUT;
        return GSTMINI_TIMEOUT;
    }

    copy_gst_object_name(event->src_name, sizeof(event->src_name), GST_MESSAGE_SRC(msg));

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(msg, &err, &debug);
        event->type = GSTMINI_EVENT_ERROR;
        if (err && err->message) {
            snprintf(event->message, sizeof(event->message), "%s", err->message);
            set_gerror("pipeline error", err);
        } else {
            snprintf(event->message, sizeof(event->message), "unknown GStreamer error");
            set_error("pipeline error");
        }
        if (debug) g_free(debug);
        if (err) g_error_free(err);
        gst_message_unref(msg);
        return GSTMINI_ERR;
    }
    case GST_MESSAGE_WARNING: {
        GError *err = NULL;
        gchar *debug = NULL;
        gst_message_parse_warning(msg, &err, &debug);
        event->type = GSTMINI_EVENT_WARNING;
        if (err && err->message) {
            snprintf(event->message, sizeof(event->message), "%s", err->message);
        } else {
            snprintf(event->message, sizeof(event->message), "GStreamer warning");
        }
        if (debug) g_free(debug);
        if (err) g_error_free(err);
        gst_message_unref(msg);
        return GSTMINI_OK;
    }
    case GST_MESSAGE_EOS:
        event->type = GSTMINI_EVENT_EOS;
        snprintf(event->message, sizeof(event->message), "end of stream");
        gst_message_unref(msg);
        return GSTMINI_EOS;
    case GST_MESSAGE_STATE_CHANGED: {
        GstState old_state;
        GstState new_state;
        GstState pending_state;
        gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
        event->type = GSTMINI_EVENT_STATE_CHANGED;
        event->old_state = (int)old_state;
        event->new_state = (int)new_state;
        event->pending_state = (int)pending_state;
        snprintf(
            event->message,
            sizeof(event->message),
            "state changed: %s -> %s",
            gst_element_state_get_name(old_state),
            gst_element_state_get_name(new_state)
        );
        gst_message_unref(msg);
        return GSTMINI_OK;
    }
    default:
        event->type = GSTMINI_EVENT_UNKNOWN;
        snprintf(event->message, sizeof(event->message), "unknown message");
        gst_message_unref(msg);
        return GSTMINI_OK;
    }
}

int gstmini_appsink_pull_view(GstMiniPipeline *p, GstMiniFrameView *view, int timeout_ms)
{
    if (!p || !p->appsink || !view) {
        set_error("bad argument");
        return GSTMINI_BAD_ARG;
    }

    view_clear(view);

    GstSample *sample = NULL;
    if (timeout_ms < 0) {
        sample = gst_app_sink_pull_sample(GST_APP_SINK(p->appsink));
    } else {
        sample = gst_app_sink_try_pull_sample(
            GST_APP_SINK(p->appsink),
            (GstClockTime)timeout_ms * GST_MSECOND
        );
    }

    if (!sample) {
        return GSTMINI_TIMEOUT;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        set_error("sample has no buffer");
        return GSTMINI_ERR;
    }
    if (!caps) {
        gst_sample_unref(sample);
        set_error("sample has no caps");
        return GSTMINI_ERR;
    }

    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps)) {
        gst_sample_unref(sample);
        set_error("failed to parse video info from caps");
        return GSTMINI_UNSUPPORTED;
    }

    GstMiniMapHolder *holder = (GstMiniMapHolder *)calloc(1, sizeof(*holder));
    if (!holder) {
        gst_sample_unref(sample);
        set_error("calloc map holder failed");
        return GSTMINI_ERR;
    }

    if (!gst_buffer_map(buffer, &holder->map, GST_MAP_READ)) {
        free(holder);
        gst_sample_unref(sample);
        set_error("gst_buffer_map failed");
        return GSTMINI_ERR;
    }

    int ret = gstmini_fill_view_from_video_info(view, &info, &holder->map);
    if (ret != GSTMINI_OK) {
        gst_buffer_unmap(buffer, &holder->map);
        free(holder);
        gst_sample_unref(sample);
        return ret;
    }

    gstmini_fill_view_buffer_metadata(view, buffer);

    view->sample = sample;
    view->buffer = buffer;
    view->map_info = holder;

    return GSTMINI_OK;
}

void gstmini_frame_view_release(GstMiniFrameView *view)
{
    if (!view) return;

    GstSample *sample = (GstSample *)view->sample;
    GstBuffer *buffer = (GstBuffer *)view->buffer;
    GstMiniMapHolder *holder = (GstMiniMapHolder *)view->map_info;

    if (buffer && holder) {
        gst_buffer_unmap(buffer, &holder->map);
    }
    if (holder) {
        free(holder);
    }
    if (sample) {
        gst_sample_unref(sample);
    }

    view_clear(view);
}

int gstmini_appsrc_push_buffer(
    GstMiniPipeline *p,
    const uint8_t *data,
    size_t size,
    int64_t pts_ns,
    int64_t duration_ns
)
{
    if (!p || !p->appsrc || !data || size == 0) {
        set_error("bad argument for appsrc push buffer");
        return GSTMINI_BAD_ARG;
    }

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
    if (!buffer) {
        set_error("gst_buffer_new_allocate failed");
        return GSTMINI_ERR;
    }

    const gsize written = gst_buffer_fill(buffer, 0, data, size);
    if (written != size) {
        gst_buffer_unref(buffer);
        set_error("gst_buffer_fill failed");
        return GSTMINI_ERR;
    }

    GST_BUFFER_PTS(buffer) = i64_to_clock_time(pts_ns);
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = i64_to_clock_time(duration_ns);
    GST_BUFFER_OFFSET(buffer) = GST_BUFFER_OFFSET_NONE;
    GST_BUFFER_OFFSET_END(buffer) = GST_BUFFER_OFFSET_NONE;

    GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(p->appsrc), buffer);
    return status_from_flow_return(flow);
}

typedef struct GstMiniWrappedBufferNotify {
    GstMiniAppsrcReleaseCallback release_cb;
    void *user_data;
} GstMiniWrappedBufferNotify;

static void gstmini_wrapped_buffer_notify(gpointer user_data)
{
    GstMiniWrappedBufferNotify *notify = (GstMiniWrappedBufferNotify *)user_data;
    if (!notify) return;

    if (notify->release_cb) {
        notify->release_cb(notify->user_data);
    }

    free(notify);
}

int gstmini_appsrc_push_wrapped_buffer(
    GstMiniPipeline *p,
    uint8_t *data,
    size_t size,
    int64_t pts_ns,
    int64_t duration_ns,
    GstMiniAppsrcReleaseCallback release_cb,
    void *user_data
)
{
    if (!p || !p->appsrc || !data || size == 0) {
        set_error("bad argument for appsrc wrapped buffer");
        return GSTMINI_BAD_ARG;
    }

    GstMiniWrappedBufferNotify *notify =
        (GstMiniWrappedBufferNotify *)calloc(1, sizeof(*notify));
    if (!notify) {
        set_error("calloc wrapped buffer notify failed");
        return GSTMINI_ERR;
    }

    notify->release_cb = release_cb;
    notify->user_data = user_data;

    GstBuffer *buffer = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY,
        data,
        size,
        0,
        size,
        notify,
        gstmini_wrapped_buffer_notify
    );
    if (!buffer) {
        free(notify);
        set_error("gst_buffer_new_wrapped_full failed");
        return GSTMINI_ERR;
    }

    GST_BUFFER_PTS(buffer) = i64_to_clock_time(pts_ns);
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = i64_to_clock_time(duration_ns);
    GST_BUFFER_OFFSET(buffer) = GST_BUFFER_OFFSET_NONE;
    GST_BUFFER_OFFSET_END(buffer) = GST_BUFFER_OFFSET_NONE;

    GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(p->appsrc), buffer);
    return status_from_flow_return(flow);
}

int gstmini_appsrc_end_of_stream(GstMiniPipeline *p)
{
    if (!p || !p->appsrc) {
        set_error("bad argument for appsrc EOS");
        return GSTMINI_BAD_ARG;
    }

    GstFlowReturn flow = gst_app_src_end_of_stream(GST_APP_SRC(p->appsrc));
    return status_from_flow_return(flow);
}
