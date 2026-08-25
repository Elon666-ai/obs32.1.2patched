#include "motion-roi.h"

#include <util/base.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>

/* Detection geometry/tuning. 320x180 luma at 1/3 of the program frame
 * rate localizes moving balls/people at (better than) macroblock
 * granularity while costing well under a millisecond per processed
 * frame. */
static constexpr uint32_t DET_W = 320;
static constexpr uint32_t DET_H = 180;
static constexpr uint32_t BLOCK = 8;
static constexpr uint32_t GRID_W = DET_W / BLOCK;
static constexpr uint32_t GRID_H = (DET_H + BLOCK - 1) / BLOCK;
static constexpr uint32_t FRAME_DIVISOR = 3;
/* Mean absolute luma difference (after global-brightness compensation)
 * for a block to count as moving. The temporal denoise filter upstream
 * flattens sensor noise well below this; real motion is far above it. */
static constexpr uint32_t ACT_THRESH = 6;
/* Census (texture-structure) requirements. Stage lighting changes a
 * block's brightness but not which pixels are brighter than their
 * neighbors; real objects moving through change that ordering. A block
 * only counts as moving when BOTH the compensated luma difference and
 * the census bit-flip rate are above threshold, so light sweeping over
 * static scenery does not activate it. */
static constexpr int CENSUS_MARGIN = 8; /* neighbor must differ by this much to set a bit */
static constexpr uint32_t CENSUS_NUM = 2; /* flipped-bits-per-sample threshold: */
static constexpr uint32_t CENSUS_DEN = 5; /* active when flips >= samples * 2/5   */
static constexpr uint32_t SAMPLE_W = DET_W / 2;
static constexpr uint32_t SAMPLE_H = DET_H / 2;
/* Blocks stay in the ROI this many processed frames after they last
 * moved (~1s at 60fps/3), so balls between bounces and briefly-still
 * people don't flicker out. */
static constexpr uint8_t HOLD_TICKS = 20;
static constexpr size_t MIN_BLOCKS = 2; /* ignore single-block speckle */
static constexpr size_t MAX_RECTS = 4;
static constexpr uint64_t MIN_UPDATE_INTERVAL_NS = 200000000; /* 200ms */

MotionRoiDetector::~MotionRoiDetector()
{
	Stop();
}

void MotionRoiDetector::Start(obs_output_t *out, float prio, float bg_prio)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (started) {
		/* Reconnect path: ApplyRoi just cleared the encoders' regions,
		 * so force the next processed frame to reapply ours. */
		last_applied.clear();
		return;
	}

	output = out;
	priority = prio;
	bg_priority = bg_prio;
	prev.assign((size_t)DET_W * DET_H, 0);
	census.assign((size_t)SAMPLE_W * SAMPLE_H, 0);
	prev_census.assign((size_t)SAMPLE_W * SAMPLE_H, 0);
	prev_mean = 0;
	prev_valid = false;
	hold.assign((size_t)GRID_W * GRID_H, 0);
	last_applied.clear();
	last_update_ns = 0;

	struct video_scale_info conversion = {};
	conversion.format = VIDEO_FORMAT_I420;
	conversion.width = DET_W;
	conversion.height = DET_H;
	conversion.range = VIDEO_RANGE_DEFAULT;
	conversion.colorspace = VIDEO_CS_DEFAULT;
	obs_add_raw_video_callback2(&conversion, FRAME_DIVISOR, RawVideo, this);

	started = true;
	blog(LOG_INFO, "[obs-webrtc] motion ROI detector started (%ux%u grid, priority %.2f, background %.2f)", GRID_W,
	     GRID_H, priority, bg_priority);
}

void MotionRoiDetector::Stop()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!started)
			return;
		started = false;
	}

	/* Outside the lock: the raw video callback takes the same lock, and
	 * removal waits for in-flight callbacks. */
	obs_remove_raw_video_callback(RawVideo, this);

	std::lock_guard<std::mutex> lock(mutex);
	output = nullptr;
	blog(LOG_INFO, "[obs-webrtc] motion ROI detector stopped");
}

void MotionRoiDetector::RawVideo(void *param, struct video_data *frame)
{
	MotionRoiDetector *self = static_cast<MotionRoiDetector *>(param);

	std::lock_guard<std::mutex> lock(self->mutex);
	if (!self->started || !frame->data[0])
		return;

	self->ProcessLocked(frame->data[0], frame->linesize[0], frame->timestamp);
}

/* 8-bit census code for one sample: for each of the 4 neighbors (2px
 * away on the detection frame), two bits record "clearly brighter" /
 * "clearly darker" (with a margin so flat areas stay all-zero). Smooth
 * illumination changes scale center and neighbors together and leave
 * the code unchanged; an object edge moving through flips bits. */
static inline uint8_t census_code(const uint8_t *luma, uint32_t linesize, uint32_t x, uint32_t y)
{
	const int c = luma[(size_t)y * linesize + x];
	const int n[4] = {
		luma[(size_t)y * linesize + (x - 2)],
		luma[(size_t)y * linesize + (x + 2)],
		luma[(size_t)(y - 2) * linesize + x],
		luma[(size_t)(y + 2) * linesize + x],
	};

	uint8_t code = 0;
	for (int i = 0; i < 4; i++) {
		if (c > n[i] + CENSUS_MARGIN)
			code |= (uint8_t)(1 << (i * 2));
		else if (c < n[i] - CENSUS_MARGIN)
			code |= (uint8_t)(1 << (i * 2 + 1));
	}
	return code;
}

static inline uint32_t popcount8(uint8_t v)
{
	v = (uint8_t)(v - ((v >> 1) & 0x55));
	v = (uint8_t)((v & 0x33) + ((v >> 2) & 0x33));
	return (uint32_t)((v + (v >> 4)) & 0x0F);
}

void MotionRoiDetector::ProcessLocked(const uint8_t *luma, uint32_t linesize, uint64_t timestamp)
{
	uint32_t sums[GRID_W * GRID_H] = {0};
	uint32_t counts[GRID_W * GRID_H] = {0};
	uint32_t census_flips[GRID_W * GRID_H] = {0};

	/* Pass 1: census codes and mean luma of the current frame, on a
	 * every-2nd-pixel sample grid (borders skipped for the neighbor
	 * lookups). */
	uint64_t cur_sum = 0;
	uint32_t cur_samples = 0;
	for (uint32_t y = 2; y < DET_H - 2; y += 2) {
		for (uint32_t x = 2; x < DET_W - 2; x += 2) {
			census[(size_t)(y / 2) * SAMPLE_W + x / 2] = census_code(luma, linesize, x, y);
			cur_sum += luma[(size_t)y * linesize + x];
			cur_samples++;
		}
	}
	const int cur_mean = (int)(cur_sum / cur_samples);

	/* Pass 2: per-block activity. The global mean shift (auto exposure,
	 * master dimmer) is subtracted from every luma difference, and each
	 * block additionally needs its census bits flipping - both gates
	 * have to open for a block to count as moving. */
	if (prev_valid) {
		const int global_shift = cur_mean - prev_mean;

		for (uint32_t y = 2; y < DET_H - 2; y += 2) {
			const uint8_t *cur = luma + (size_t)y * linesize;
			const uint8_t *prv = prev.data() + (size_t)y * DET_W;
			const uint32_t gy = y / BLOCK;

			for (uint32_t x = 2; x < DET_W - 2; x += 2) {
				const uint32_t gidx = gy * GRID_W + x / BLOCK;
				const size_t sidx = (size_t)(y / 2) * SAMPLE_W + x / 2;

				sums[gidx] += (uint32_t)std::abs((int)cur[x] - (int)prv[x] - global_shift);
				census_flips[gidx] += popcount8((uint8_t)(census[sidx] ^ prev_census[sidx]));
				counts[gidx]++;
			}
		}
	}

	for (uint32_t y = 0; y < DET_H; y++)
		memcpy(prev.data() + (size_t)y * DET_W, luma + (size_t)y * linesize, DET_W);
	census.swap(prev_census);
	prev_mean = cur_mean;

	if (!prev_valid) {
		prev_valid = true;
		return;
	}

	for (size_t i = 0; i < GRID_W * GRID_H; i++) {
		const bool active = counts[i] && sums[i] / counts[i] >= ACT_THRESH &&
				    census_flips[i] * CENSUS_DEN >= counts[i] * CENSUS_NUM;
		hold[i] = active ? HOLD_TICKS : (hold[i] ? (uint8_t)(hold[i] - 1) : 0);
	}

	/* Cluster held blocks into bounding rectangles (4-connectivity),
	 * keep the largest few. */
	struct Component {
		uint32_t min_x, min_y, max_x, max_y;
		size_t blocks;
	};
	std::vector<Component> comps;
	std::vector<bool> visited((size_t)GRID_W * GRID_H, false);
	std::deque<uint32_t> queue;

	for (uint32_t start = 0; start < GRID_W * GRID_H; start++) {
		if (!hold[start] || visited[start])
			continue;

		Component c = {GRID_W, GRID_H, 0, 0, 0};
		visited[start] = true;
		queue.push_back(start);

		while (!queue.empty()) {
			const uint32_t idx = queue.front();
			queue.pop_front();
			const uint32_t x = idx % GRID_W;
			const uint32_t y = idx / GRID_W;

			c.min_x = std::min(c.min_x, x);
			c.min_y = std::min(c.min_y, y);
			c.max_x = std::max(c.max_x, x);
			c.max_y = std::max(c.max_y, y);
			c.blocks++;

			const uint32_t neighbors[4] = {x ? idx - 1 : idx, x + 1 < GRID_W ? idx + 1 : idx,
						       y ? idx - GRID_W : idx, y + 1 < GRID_H ? idx + GRID_W : idx};
			for (uint32_t n : neighbors) {
				if (n != idx && hold[n] && !visited[n]) {
					visited[n] = true;
					queue.push_back(n);
				}
			}
		}

		if (c.blocks >= MIN_BLOCKS)
			comps.push_back(c);
	}

	std::sort(comps.begin(), comps.end(), [](const Component &a, const Component &b) { return a.blocks > b.blocks; });
	if (comps.size() > MAX_RECTS)
		comps.resize(MAX_RECTS);

	/* Only touch the encoders when the block-quantized rects actually
	 * changed, and no more often than every 200ms - the hold counters
	 * already keep the rects stable between bounces/steps. */
	std::vector<uint32_t> sig;
	sig.reserve(comps.size());
	for (const Component &c : comps)
		sig.push_back((c.min_x << 24) | (c.min_y << 16) | (c.max_x << 8) | c.max_y);

	if (sig == last_applied)
		return;
	if (timestamp - last_update_ns < MIN_UPDATE_INTERVAL_NS)
		return;

	last_applied = sig;
	last_update_ns = timestamp;

	if (comps.empty()) {
		blog(LOG_INFO, "[obs-webrtc] motion ROI: no active region (cleared)");
	} else {
		for (const Component &c : comps)
			blog(LOG_INFO,
			     "[obs-webrtc] motion ROI: region at block (%u,%u)-(%u,%u) [%zu blocks] of %ux%u grid",
			     c.min_x, c.min_y, c.max_x, c.max_y, c.blocks, GRID_W, GRID_H);
	}

	for (uint32_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
		obs_encoder_t *encoder = obs_output_get_video_encoder2(output, idx);
		if (encoder == nullptr)
			break;

		const uint32_t enc_w = obs_encoder_get_width(encoder);
		const uint32_t enc_h = obs_encoder_get_height(encoder);
		if (!enc_w || !enc_h)
			continue;

		obs_encoder_clear_roi(encoder);

		bool any = false;
		for (const Component &c : comps) {
			struct obs_encoder_roi region = {};
			region.left = c.min_x * BLOCK * enc_w / DET_W;
			region.top = c.min_y * BLOCK * enc_h / DET_H;
			region.right = std::min((c.max_x + 1) * BLOCK, DET_W) * enc_w / DET_W;
			region.bottom = std::min((c.max_y + 1) * BLOCK, DET_H) * enc_h / DET_H;
			region.priority = priority;

			if (obs_encoder_add_roi(encoder, &region))
				any = true;
		}

		if (any && bg_priority < 0.0f) {
			struct obs_encoder_roi background = {};
			background.right = enc_w;
			background.bottom = enc_h;
			background.priority = bg_priority;
			obs_encoder_add_roi(encoder, &background);
		}
	}
}
