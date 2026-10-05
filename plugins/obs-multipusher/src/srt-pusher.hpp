#pragma once

/// Register the custom "mp_srt_pusher" output type during plugin load.
/// This output uses FFmpeg avformat directly to push encoded data via SRT,
/// bypassing OBS's service/output restrictions entirely.
void RegisterSRTPusherOutput();
