# gstmini Step2 FrameView timestamp patch

Changed files:

```text
include/gstmini.h
src/gstmini.c
gstmini_c_frameview_patch.nim
```

`GstMiniFrameView` now includes:

```c
int64_t pts_ns;
int64_t dts_ns;
int64_t duration_ns;
uint64_t offset;
uint64_t offset_end;
```

Use this for cooldown / saved-frame timing instead of assuming fixed FPS.

Semantics:

- time values are nanoseconds
- invalid/unknown time is `-1`
- unknown offset is `UINT64_MAX`
