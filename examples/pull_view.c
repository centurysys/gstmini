#include "gstmini.h"

#include <stdio.h>

static const char *fmt_name(GstMiniPixelFormat fmt) {
    switch (fmt) {
    case GSTMINI_FORMAT_RGBX:
        return "RGBX";
    case GSTMINI_FORMAT_RGBA:
        return "RGBA";
    case GSTMINI_FORMAT_NV12:
        return "NV12";
    case GSTMINI_FORMAT_JPEG:
        return "JPEG";
    case GSTMINI_FORMAT_H264:
        return "H264";
    default:
        return "UNKNOWN";
    }
}

int main(void) {
    const char *desc =
        "videotestsrc num-buffers=1 "
        "! video/x-raw,format=NV12,width=320,height=240 "
        "! appsink name=sink sync=false max-buffers=1 drop=false";

    GstMiniOpenOptions opts = {
        .launch_desc = desc,
        .appsrc_name = NULL,
        .appsink_name = "sink",
    };

    GstMiniPipeline *p = gstmini_pipeline_open(&opts);
    if (!p) {
        fprintf(stderr, "open failed: %s\n", gstmini_last_error());
        return 1;
    }

    int ret = gstmini_pipeline_start(p);
    if (ret != GSTMINI_OK) {
        fprintf(stderr, "start failed: %s\n", gstmini_last_error());
        gstmini_pipeline_close(p);
        return 1;
    }

    GstMiniFrameView view;
    ret = gstmini_appsink_pull_view(p, &view, 5000);
    if (ret != GSTMINI_OK) {
        fprintf(stderr, "pull_view failed: ret=%d err=%s\n", ret, gstmini_last_error());
        gstmini_pipeline_close(p);
        return 1;
    }

    printf("frame: %dx%d format=%s n_planes=%d size=%zu\n",
           view.width,
           view.height,
           fmt_name(view.format),
           view.n_planes,
           view.size);

    for (int i = 0; i < view.n_planes; i++) {
        printf("  plane[%d]: data=%p size=%zu stride=%d offset=%zu\n",
               i,
               (void *)view.planes[i].data,
               view.planes[i].size,
               view.planes[i].stride,
               view.planes[i].offset);
    }

    gstmini_frame_view_release(&view);

    /*
     * Drain EOS if available. Not mandatory for this example.
     */
    for (;;) {
        GstMiniEvent ev;
        ret = gstmini_pipeline_poll_event(p, &ev, 100);
        if (ret == GSTMINI_TIMEOUT) break;
        if (ret == GSTMINI_EOS) {
            printf("EOS\n");
            break;
        }
        if (ret == GSTMINI_ERR) {
            fprintf(stderr, "ERROR: %s\n", ev.message);
            break;
        }
    }

    gstmini_pipeline_stop(p);
    gstmini_pipeline_close(p);

    return 0;
}
