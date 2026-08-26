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
 * Everything the encoders see is deliberately sluggish, because the
 * QP map is what the viewer perceives as "sharpness" and a step in it
 * reads as the picture changing quality:
 *   - the whole foreground/background contrast fades in and out over a
 *     ramp instead of switching on with the first moving block,
 *   - a rectangle growing is applied at once but shrinking needs the
 *     edges to have moved a few blocks, so a deflating region does not
 *     re-program the encoders every update tick,
 *   - rectangles covering so much of the frame that prioritizing them
 *     buys nothing are dropped, which fades the effect out rather than
 *     leaving a boost over most of the picture.
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
	/* Bounding rectangle in detection-grid block coordinates, both
	 * corners inclusive. */
	struct Rect {
		uint32_t min_x, min_y, max_x, max_y;
	};

	static void RawVideo(void *param, struct video_data *frame);
	void ProcessLocked(const uint8_t *luma, uint32_t linesize, uint64_t timestamp);
	void ApplyLocked(const std::vector<Rect> &rects, float scale);

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
	std::vector<Rect> applied; // rects currently programmed into the encoders
	float applied_scale = 0.0f; // ramp factor `applied` was programmed with
	uint64_t last_update_ns = 0;
};
