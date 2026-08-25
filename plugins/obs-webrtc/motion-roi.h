#pragma once

#include <obs.h>

#include <cstdint>
#include <mutex>
#include <vector>

/*
 * Zero-dependency dynamic ROI source for the WHIP output.
 *
 * Taps the composited program feed via obs_add_raw_video_callback2 at a
 * small detection resolution and a reduced frame rate, marks blocks
 * whose luma changed between frames (moving lottery balls and people
 * light up, the static background wall does not), clusters the active
 * blocks into up to a few bounding rectangles, and applies those to
 * every video encoder on the output as quality-priority regions - with
 * a full-frame negative-priority region behind them so the background
 * gets compressed harder.
 *
 * The encoder-facing side only consumes normalized rectangles, so a
 * smarter detector (e.g. OpenCV ball/person detection in a private
 * build) can replace the motion heuristic without touching the
 * ROI-application path.
 */
class MotionRoiDetector {
public:
	~MotionRoiDetector();

	// Idempotent; safe to call again on reconnect Start()s.
	void Start(obs_output_t *output, float priority, float bg_priority);
	void Stop();

private:
	static void RawVideo(void *param, struct video_data *frame);
	void ProcessLocked(const uint8_t *luma, uint32_t linesize, uint64_t timestamp);

	std::mutex mutex;
	bool started = false;
	obs_output_t *output = nullptr;
	float priority = 0.3f;
	float bg_priority = -0.25f;

	std::vector<uint8_t> prev; // previous downscaled luma frame
	std::vector<uint8_t> census; // census codes of the current frame
	std::vector<uint8_t> prev_census; // census codes of the previous frame
	int prev_mean = 0; // mean sampled luma of the previous frame
	bool prev_valid = false;
	std::vector<uint8_t> hold; // per-block linger counters
	std::vector<uint32_t> last_applied; // packed block rects last sent to encoders
	uint64_t last_update_ns = 0;
};
