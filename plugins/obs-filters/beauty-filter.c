/*
 * Basic face beauty filter (CPU, async frame path, zero-dependency).
 *
 * Finds a face by skin-tone segmentation on a downsampled grid (largest
 * connected skin blob with plausible size/density/aspect, temporally
 * smoothed and held across brief detection dropouts), then applies the
 * classic "basic beauty" set only while a face is present:
 *
 *   - rosy skin + deeper lip tint : chroma-domain Cr boost, feathered
 *     by a face ellipse; lip pixels self-select as the redder-than-
 *     average-skin region in the lower-central face.
 *   - light skin smoothing        : epsilon filter (box mean blended
 *     only where it differs little from the pixel, so edges survive).
 *   - eye enlargement             : local magnify warp at eye positions
 *     found as the two dark clusters in the upper face half.
 *   - chin slimming ("V face")    : horizontal squeeze toward the face
 *     center line, ramping up toward the detected chin tip.
 *
 * With no face in frame the filter is an exact passthrough, which is
 * what makes blanket auto-attachment to every camera source safe (see
 * OBSBasic::ApplyBeautyFilterSetting in the frontend).
 *
 * Landmarks here are heuristic (skin blob + dark-cluster eyes), not an
 * ML detector, so everything geometry-driven is aggressively
 * stabilized: estimates move through a dead-band + slow-EMA filter
 * (sub-dead-band jitter freezes the anchor entirely instead of
 * re-warping every frame), the chin is derived anthropometrically from
 * the eye line and pupil distance (the skin blob's bottom edge is
 * usually the neck, not the chin), eye clusters must pass compactness
 * and jump-rejection checks, and warp strength fades in only after the
 * eye estimate has been continuously stable - and fades back out on
 * loss - so the warps never pop or wobble frame-to-frame.
 *
 * Supported formats: I420, NV12, YUY2, YVYU, UYVY (everything a dshow
 * camera realistically delivers). Others pass through with a one-time
 * log line.
 */

#include <obs-module.h>
#include <util/threading.h>
#include <math.h>
#include <string.h>

#define S_SMOOTH "smooth_strength"
#define S_ROSY "rosy_strength"
#define S_LIPS "lip_strength"
#define S_EYES "eye_strength"
#define S_CHIN "chin_strength"

/* Detection grid: every 4th luma pixel in both axes. */
#define DET_STEP 4
/* Frames a face/eye estimate survives without re-confirmation. */
#define FACE_HOLD 20
#define EYE_HOLD 12

/* Scratch buffer roles. */
enum { SCRATCH_WARP, SCRATCH_BLUR_H, SCRATCH_BLUR_V, SCRATCH_COUNT };

struct plane_view {
	uint8_t *y;
	uint32_t ys, yp; /* stride, per-pixel byte step */
	uint8_t *cb, *cr;
	uint32_t cbs, crs, cp;
	uint32_t w, h;   /* luma dims */
	uint32_t cw, ch; /* chroma dims */
	uint32_t csx, csy; /* luma coord >> cs* = chroma coord */
};

struct beauty_data {
	obs_source_t *context;
	pthread_mutex_t mutex;

	/* settings (UI thread writes under mutex, video thread copies) */
	double smooth, rosy, lips, eyes, chin;

	/* detection scratch (video thread only) */
	uint8_t *mask;
	uint8_t *visited;
	uint32_t *queue;
	size_t det_cap;

	/* effect scratch (video thread only) */
	uint8_t *scratch[SCRATCH_COUNT];
	size_t scratch_cap[SCRATCH_COUNT];

	/* face state (video thread only), luma pixel coords */
	bool face_present;
	float fx, fy, fw, fh; /* smoothed bbox (raw skin blob) */
	float fh_eff;         /* height capped vs width: excludes the neck */
	int face_hold;
	float eye_lx, eye_ly, eye_rx, eye_ry;
	bool eyes_valid;
	int eye_hold;
	float chin_x, chin_y;
	int skin_cr_mean;
	int skin_y_mean;
	uint32_t stable_frames; /* consecutive accepted detections */
	float ramp;             /* 0..1 warp strength fade */
	bool frontal;           /* eye geometry consistent with a frontal face */

	bool warned_format;
};

static const char *beauty_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BeautyFilter");
}

static void beauty_update(void *data, obs_data_t *settings)
{
	struct beauty_data *f = data;

	pthread_mutex_lock(&f->mutex);
	f->smooth = obs_data_get_double(settings, S_SMOOTH);
	f->rosy = obs_data_get_double(settings, S_ROSY);
	f->lips = obs_data_get_double(settings, S_LIPS);
	f->eyes = obs_data_get_double(settings, S_EYES);
	f->chin = obs_data_get_double(settings, S_CHIN);
	pthread_mutex_unlock(&f->mutex);
}

static void *beauty_create(obs_data_t *settings, obs_source_t *context)
{
	struct beauty_data *f = bzalloc(sizeof(*f));

	f->context = context;
	pthread_mutex_init(&f->mutex, NULL);
	beauty_update(f, settings);

	return f;
}

static void beauty_destroy(void *data)
{
	struct beauty_data *f = data;

	bfree(f->mask);
	bfree(f->visited);
	bfree(f->queue);
	for (int i = 0; i < SCRATCH_COUNT; i++)
		bfree(f->scratch[i]);
	pthread_mutex_destroy(&f->mutex);
	bfree(f);
}

static obs_properties_t *beauty_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_float_slider(props, S_SMOOTH, obs_module_text("Beauty.Smooth"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(props, S_ROSY, obs_module_text("Beauty.Rosy"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(props, S_LIPS, obs_module_text("Beauty.Lips"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(props, S_EYES, obs_module_text("Beauty.Eyes"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(props, S_CHIN, obs_module_text("Beauty.Chin"), 0.0, 1.0, 0.05);

	UNUSED_PARAMETER(data);
	return props;
}

static void beauty_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, S_SMOOTH, 0.35);
	obs_data_set_default_double(settings, S_ROSY, 0.5);
	obs_data_set_default_double(settings, S_LIPS, 0.5);
	obs_data_set_default_double(settings, S_EYES, 0.4);
	obs_data_set_default_double(settings, S_CHIN, 0.4);
}

static bool make_view(struct obs_source_frame *frame, struct plane_view *v)
{
	const uint32_t w = frame->width, h = frame->height;

	v->w = w;
	v->h = h;

	switch (frame->format) {
	case VIDEO_FORMAT_I420:
		v->y = frame->data[0];
		v->ys = frame->linesize[0];
		v->yp = 1;
		v->cb = frame->data[1];
		v->cbs = frame->linesize[1];
		v->cr = frame->data[2];
		v->crs = frame->linesize[2];
		v->cp = 1;
		v->cw = (w + 1) / 2;
		v->ch = (h + 1) / 2;
		v->csx = 1;
		v->csy = 1;
		return true;
	case VIDEO_FORMAT_NV12:
		v->y = frame->data[0];
		v->ys = frame->linesize[0];
		v->yp = 1;
		v->cb = frame->data[1];
		v->cr = frame->data[1] + 1;
		v->cbs = v->crs = frame->linesize[1];
		v->cp = 2;
		v->cw = (w + 1) / 2;
		v->ch = (h + 1) / 2;
		v->csx = 1;
		v->csy = 1;
		return true;
	case VIDEO_FORMAT_YUY2: /* Y0 U Y1 V */
	case VIDEO_FORMAT_YVYU: /* Y0 V Y1 U */
		v->y = frame->data[0];
		v->ys = frame->linesize[0];
		v->yp = 2;
		v->cb = frame->data[0] + (frame->format == VIDEO_FORMAT_YUY2 ? 1 : 3);
		v->cr = frame->data[0] + (frame->format == VIDEO_FORMAT_YUY2 ? 3 : 1);
		v->cbs = v->crs = frame->linesize[0];
		v->cp = 4;
		v->cw = w / 2;
		v->ch = h;
		v->csx = 1;
		v->csy = 0;
		return true;
	case VIDEO_FORMAT_UYVY: /* U Y0 V Y1 */
		v->y = frame->data[0] + 1;
		v->ys = frame->linesize[0];
		v->yp = 2;
		v->cb = frame->data[0];
		v->cr = frame->data[0] + 2;
		v->cbs = v->crs = frame->linesize[0];
		v->cp = 4;
		v->cw = w / 2;
		v->ch = h;
		v->csx = 1;
		v->csy = 0;
		return true;
	default:
		return false;
	}
}

static inline uint8_t view_y(const struct plane_view *v, uint32_t x, uint32_t y)
{
	return v->y[(size_t)y * v->ys + (size_t)x * v->yp];
}

static inline uint8_t view_cb(const struct plane_view *v, uint32_t cx, uint32_t cy)
{
	return v->cb[(size_t)cy * v->cbs + (size_t)cx * v->cp];
}

static inline uint8_t view_cr(const struct plane_view *v, uint32_t cx, uint32_t cy)
{
	return v->cr[(size_t)cy * v->crs + (size_t)cx * v->cp];
}

/* Skin classifier: classic YCbCr box rule, widened a little so both
 * full- and partial-range camera feeds land inside it. */
static inline bool is_skin(int y, int cb, int cr)
{
	return y > 30 && y < 250 && cb >= 74 && cb <= 130 && cr >= 132 && cr <= 178;
}

static inline float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static inline int clampi(int x, int lo, int hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

/* Dead-band + slow EMA: changes below `dead` freeze the anchor
 * entirely (no per-frame re-warping from grid jitter); larger moves
 * follow slowly. The dead-band is subtracted from the delta so the
 * anchor still converges instead of lagging a constant offset. */
static inline float stabilize(float cur, float target, float dead, float alpha)
{
	const float d = target - cur;
	if (fabsf(d) <= dead)
		return cur;
	return cur + alpha * (d - (d > 0 ? dead : -dead));
}

/* ---------------------------------------------------------------------
 * Detection
 */

static void detect_face(struct beauty_data *f, const struct plane_view *v)
{
	const uint32_t dw = v->w / DET_STEP;
	const uint32_t dh = v->h / DET_STEP;
	const size_t det_size = (size_t)dw * dh;

	if (!dw || !dh)
		return;

	if (f->det_cap < det_size) {
		bfree(f->mask);
		bfree(f->visited);
		bfree(f->queue);
		f->mask = bmalloc(det_size);
		f->visited = bmalloc(det_size);
		f->queue = bmalloc(det_size * sizeof(uint32_t));
		f->det_cap = det_size;
	}

	/* skin mask on the detection grid */
	for (uint32_t j = 0; j < dh; j++) {
		const uint32_t ly = j * DET_STEP;
		const uint32_t cy = ly >> v->csy;
		for (uint32_t i = 0; i < dw; i++) {
			const uint32_t lx = i * DET_STEP;
			const uint32_t cx = lx >> v->csx;
			f->mask[(size_t)j * dw + i] =
				is_skin(view_y(v, lx, ly), view_cb(v, cx, cy), view_cr(v, cx, cy));
		}
	}

	/* largest 4-connected skin component */
	memset(f->visited, 0, det_size);
	uint32_t best_count = 0, best_minx = 0, best_miny = 0, best_maxx = 0, best_maxy = 0;

	for (uint32_t start = 0; start < det_size; start++) {
		if (!f->mask[start] || f->visited[start])
			continue;

		uint32_t head = 0, tail = 0;
		uint32_t count = 0, minx = dw, miny = dh, maxx = 0, maxy = 0;
		f->visited[start] = 1;
		f->queue[tail++] = start;

		while (head < tail) {
			const uint32_t idx = f->queue[head++];
			const uint32_t x = idx % dw, y = idx / dw;
			count++;
			if (x < minx)
				minx = x;
			if (x > maxx)
				maxx = x;
			if (y < miny)
				miny = y;
			if (y > maxy)
				maxy = y;

			const uint32_t n[4] = {x ? idx - 1 : idx, x + 1 < dw ? idx + 1 : idx,
					       y ? idx - dw : idx, y + 1 < dh ? idx + dw : idx};
			for (int k = 0; k < 4; k++) {
				if (n[k] != idx && f->mask[n[k]] && !f->visited[n[k]]) {
					f->visited[n[k]] = 1;
					f->queue[tail++] = n[k];
				}
			}
		}

		if (count > best_count) {
			best_count = count;
			best_minx = minx;
			best_miny = miny;
			best_maxx = maxx;
			best_maxy = maxy;
		}
	}

	const uint32_t bw = best_maxx - best_minx + 1;
	const uint32_t bh = best_maxy - best_miny + 1;

	/* A face too small to see clearly isn't worth beautifying - and
	 * its landmark estimates ride on too few detection-grid sites to
	 * be stable anyway. Hysteresis: a new face must reach 12% of the
	 * frame's short side, a tracked one is kept down to 10%, so a
	 * borderline face doesn't flap the effect on and off. */
	const float short_side = (float)(v->w < v->h ? v->w : v->h);
	const float min_face_w = f->face_present ? fmaxf(0.10f * short_side, 60.0f)
						 : fmaxf(0.12f * short_side, 72.0f);

	const bool plausible = best_count >= det_size / 200 && best_count >= 80 && bw >= 8 && bh >= 8 &&
			       (float)best_count / ((float)bw * bh) >= 0.30f && (float)bh / bw >= 0.7f &&
			       (float)bh / bw <= 2.5f && (float)(bw * DET_STEP) >= min_face_w;

	if (!plausible) {
		if (f->face_present && ++f->face_hold > FACE_HOLD) {
			f->face_present = false;
			blog(LOG_INFO, "[beauty: '%s'] face lost", obs_source_get_name(f->context));
		}
		return;
	}

	const float rx = (float)(best_minx * DET_STEP);
	const float ry = (float)(best_miny * DET_STEP);
	const float rw = (float)(bw * DET_STEP);
	const float rh = (float)(bh * DET_STEP);

	if (!f->face_present) {
		f->fx = rx;
		f->fy = ry;
		f->fw = rw;
		f->fh = rh;
		f->face_present = true;
		f->eyes_valid = false;
		f->eye_hold = EYE_HOLD + 1;
		f->chin_y = 0; /* snap fresh below instead of blending from a previous face */
		f->stable_frames = 0;
		blog(LOG_INFO, "[beauty: '%s'] face found at %.0f,%.0f %.0fx%.0f",
		     obs_source_get_name(f->context), rx, ry, rw, rh);
	} else {
		f->fx = stabilize(f->fx, rx, 3.0f, 0.25f);
		f->fy = stabilize(f->fy, ry, 3.0f, 0.25f);
		f->fw = stabilize(f->fw, rw, 4.0f, 0.25f);
		f->fh = stabilize(f->fh, rh, 4.0f, 0.25f);
	}
	/* The skin blob usually runs past the jaw into the neck/chest;
	 * anthropometric face height tops out around 1.45x its width, so
	 * cap the height used for all effects at that. */
	f->fh_eff = f->fh < 1.45f * f->fw ? f->fh : 1.45f * f->fw;
	f->face_hold = 0;
	f->stable_frames++;

	/* means over skin sites inside the face box (for rosy/lips gating
	 * and the eye darkness threshold) */
	{
		uint32_t sum_y = 0, sum_cr = 0, n = 0;
		const uint32_t j0 = best_miny, j1 = best_maxy, i0 = best_minx, i1 = best_maxx;
		for (uint32_t j = j0; j <= j1; j++) {
			for (uint32_t i = i0; i <= i1; i++) {
				if (!f->mask[(size_t)j * dw + i])
					continue;
				const uint32_t lx = i * DET_STEP, ly = j * DET_STEP;
				sum_y += view_y(v, lx, ly);
				sum_cr += view_cr(v, lx >> v->csx, ly >> v->csy);
				n++;
			}
		}
		if (n) {
			f->skin_y_mean = (int)(sum_y / n);
			f->skin_cr_mean = (int)(sum_cr / n);
		}
	}

	/* eyes: darkness-weighted centroids of the two dark clusters in
	 * the upper face half, one per side of the face center line. Each
	 * cluster must be compact (a diffuse dark region is hair or
	 * shadow, not an eye), and a validated pair that suddenly jumps
	 * far from the tracked one is rejected as a mis-lock rather than
	 * followed - the track only re-snaps after EYE_HOLD rejections. */
	{
		const float cx = f->fx + f->fw * 0.5f;
		const uint32_t j0 = (uint32_t)clampf((f->fy + 0.20f * f->fh_eff) / DET_STEP, 0, (float)(dh - 1));
		const uint32_t j1 = (uint32_t)clampf((f->fy + 0.50f * f->fh_eff) / DET_STEP, 0, (float)(dh - 1));
		const uint32_t i0 = (uint32_t)clampf((f->fx + 0.10f * f->fw) / DET_STEP, 0, (float)(dw - 1));
		const uint32_t i1 = (uint32_t)clampf((f->fx + 0.90f * f->fw) / DET_STEP, 0, (float)(dw - 1));
		const int dark_thresh = f->skin_y_mean - 22;

		float lwx = 0, lwy = 0, lxx = 0, lyy = 0, lw = 0;
		float rwx = 0, rwy = 0, rxx = 0, ryy = 0, rw_ = 0;
		uint32_t ln = 0, rn = 0;

		for (uint32_t j = j0; j <= j1; j++) {
			for (uint32_t i = i0; i <= i1; i++) {
				const int yv = view_y(v, i * DET_STEP, j * DET_STEP);
				if (yv >= dark_thresh)
					continue;
				const float wgt = (float)(dark_thresh - yv);
				const float px = (float)(i * DET_STEP), py = (float)(j * DET_STEP);
				if (px < cx) {
					lwx += wgt * px;
					lwy += wgt * py;
					lxx += wgt * px * px;
					lyy += wgt * py * py;
					lw += wgt;
					ln++;
				} else {
					rwx += wgt * px;
					rwy += wgt * py;
					rxx += wgt * px * px;
					ryy += wgt * py * py;
					rw_ += wgt;
					rn++;
				}
			}
		}

		bool ok = ln >= 3 && rn >= 3 && lw > 0 && rw_ > 0;
		if (ok) {
			const float elx = lwx / lw, ely = lwy / lw;
			const float erx = rwx / rw_, ery = rwy / rw_;
			const float lrms = sqrtf(fmaxf(0.0f, lxx / lw - elx * elx) +
						 fmaxf(0.0f, lyy / lw - ely * ely));
			const float rrms = sqrtf(fmaxf(0.0f, rxx / rw_ - erx * erx) +
						 fmaxf(0.0f, ryy / rw_ - ery * ery));
			const float sep = erx - elx;

			ok = sep >= 0.22f * f->fw && sep <= 0.75f * f->fw &&
			     fabsf(ery - ely) <= 0.14f * f->fh_eff && lrms <= 0.17f * f->fw &&
			     rrms <= 0.17f * f->fw;

			if (ok && f->eyes_valid) {
				const float jump = fmaxf(fabsf(elx - f->eye_lx) + fabsf(ely - f->eye_ly),
							 fabsf(erx - f->eye_rx) + fabsf(ery - f->eye_ry));
				if (jump > 0.15f * f->fw)
					ok = false; /* mis-lock, don't follow */
			}

			if (ok) {
				if (f->eyes_valid) {
					f->eye_lx = stabilize(f->eye_lx, elx, 1.5f, 0.25f);
					f->eye_ly = stabilize(f->eye_ly, ely, 1.5f, 0.25f);
					f->eye_rx = stabilize(f->eye_rx, erx, 1.5f, 0.25f);
					f->eye_ry = stabilize(f->eye_ry, ery, 1.5f, 0.25f);
				} else {
					f->eye_lx = elx;
					f->eye_ly = ely;
					f->eye_rx = erx;
					f->eye_ry = ery;
					f->eyes_valid = true;
				}
				f->eye_hold = 0;

				/* Frontal check for the warps: a turning head
				 * foreshortens the pupil distance and shifts
				 * the eye midpoint off the face center line;
				 * warping with frontal-face proportions then
				 * distorts, so the warps fade out instead. */
				const float ssep = f->eye_rx - f->eye_lx;
				const float mid_off = fabsf(0.5f * (f->eye_lx + f->eye_rx) - cx);
				f->frontal = ssep >= 0.34f * f->fw && ssep <= 0.60f * f->fw &&
					     mid_off <= 0.10f * f->fw;
			}
		}
		if (!ok && f->eyes_valid && ++f->eye_hold > EYE_HOLD)
			f->eyes_valid = false;
	}

	/* chin tip: derived from the eye line by facial proportion (chin
	 * sits ~1.6x the pupil distance below the eye line, face midline
	 * is the eye midpoint). The skin blob's bottom edge is NOT usable
	 * here - it's usually the neck or chest. Without a valid eye pair
	 * fall back to the capped face box (color effects only; the chin
	 * warp itself requires eyes). */
	if (f->eyes_valid) {
		const float sep = f->eye_rx - f->eye_lx;
		const float eye_cy = 0.5f * (f->eye_ly + f->eye_ry);
		const float target = clampf(eye_cy + 1.6f * sep, f->fy + 0.65f * f->fh_eff,
					    f->fy + 1.05f * f->fh_eff);
		if (f->chin_y <= 0) {
			f->chin_y = target;
			f->chin_x = 0.5f * (f->eye_lx + f->eye_rx);
		} else {
			f->chin_y = stabilize(f->chin_y, target, 2.0f, 0.25f);
			f->chin_x = stabilize(f->chin_x, 0.5f * (f->eye_lx + f->eye_rx), 1.5f, 0.25f);
		}
	} else if (f->chin_y <= 0) {
		f->chin_y = f->fy + 0.95f * f->fh_eff;
		f->chin_x = f->fx + 0.5f * f->fw;
	}
}

/* ---------------------------------------------------------------------
 * Effects
 */

static uint8_t *get_scratch(struct beauty_data *f, int idx, size_t size)
{
	if (f->scratch_cap[idx] < size) {
		bfree(f->scratch[idx]);
		f->scratch[idx] = bmalloc(size);
		f->scratch_cap[idx] = size;
	}
	return f->scratch[idx];
}

static inline uint8_t bsample(const uint8_t *buf, uint32_t stride, uint32_t px, uint32_t w, uint32_t h, float x,
			      float y)
{
	x = clampf(x, 0.0f, (float)w - 1.001f);
	y = clampf(y, 0.0f, (float)h - 1.001f);
	const uint32_t ix = (uint32_t)x, iy = (uint32_t)y;
	const float fx = x - ix, fy = y - iy;
	const uint8_t *r0 = buf + (size_t)iy * stride;
	const uint8_t *r1 = r0 + stride;
	const float p00 = r0[(size_t)ix * px], p01 = r0[(size_t)(ix + 1) * px];
	const float p10 = r1[(size_t)ix * px], p11 = r1[(size_t)(ix + 1) * px];
	const float top = p00 + fx * (p01 - p00);
	const float bot = p10 + fx * (p11 - p10);
	return (uint8_t)(top + fy * (bot - top) + 0.5f);
}

/* Copy a pixel region [x0,y0,w,h] of a (possibly interleaved) channel
 * into contiguous scratch keeping the same per-pixel byte step, so the
 * same sampling arithmetic works on the copy. */
static uint8_t *copy_region(struct beauty_data *f, const uint8_t *base, uint32_t stride, uint32_t px, uint32_t x0,
			    uint32_t y0, uint32_t w, uint32_t h)
{
	const size_t row_bytes = (size_t)w * px;
	uint8_t *dst = get_scratch(f, SCRATCH_WARP, row_bytes * h);
	for (uint32_t y = 0; y < h; y++)
		memcpy(dst + (size_t)y * row_bytes, base + (size_t)(y0 + y) * stride + (size_t)x0 * px, row_bytes);
	return dst;
}

/* Local magnify warp: content inside the (Rx,Ry) ellipse around the
 * center is scaled up by up to `a` at the center, continuous (scale 1)
 * at the ellipse edge. */
static void warp_magnify(struct beauty_data *f, uint8_t *base, uint32_t stride, uint32_t px, uint32_t iw, uint32_t ih,
			 float cx, float cy, float Rx, float Ry, float a)
{
	const int x0 = clampi((int)(cx - Rx) - 1, 0, (int)iw - 1);
	const int y0 = clampi((int)(cy - Ry) - 1, 0, (int)ih - 1);
	const int x1 = clampi((int)(cx + Rx) + 2, 0, (int)iw);
	const int y1 = clampi((int)(cy + Ry) + 2, 0, (int)ih);
	const int rw = x1 - x0, rh = y1 - y0;
	if (rw < 4 || rh < 4)
		return;

	const uint8_t *copy = copy_region(f, base, stride, px, x0, y0, rw, rh);
	const size_t copy_stride = (size_t)rw * px;

	for (int y = y0; y < y1; y++) {
		const float dy = ((float)y - cy) / Ry;
		uint8_t *row = base + (size_t)y * stride;
		for (int x = x0; x < x1; x++) {
			const float dx = ((float)x - cx) / Rx;
			const float r2 = dx * dx + dy * dy;
			if (r2 >= 1.0f)
				continue;
			const float s = 1.0f - a * (1.0f - r2);
			const float sx = cx + ((float)x - cx) * s - x0;
			const float sy = cy + ((float)y - cy) * s - y0;
			row[(size_t)x * px] = bsample(copy, (uint32_t)copy_stride, px, rw, rh, sx, sy);
		}
	}
}

/* Chin "V face" warp: rows approaching the chin tip are squeezed
 * horizontally toward the face center line; feathered to zero at the
 * band's top/bottom and at its left/right edges so the warp blends
 * into the untouched surroundings. */
static void warp_chin(struct beauty_data *f, uint8_t *base, uint32_t stride, uint32_t px, uint32_t iw, uint32_t ih,
		      float cx, float chin_y, float band_top, float band_bot, float xr, float cmax)
{
	const int x0 = clampi((int)(cx - xr) - 1, 0, (int)iw - 1);
	const int y0 = clampi((int)band_top, 0, (int)ih - 1);
	const int x1 = clampi((int)(cx + xr) + 2, 0, (int)iw);
	const int y1 = clampi((int)band_bot + 1, 0, (int)ih);
	const int rw = x1 - x0, rh = y1 - y0;
	if (rw < 4 || rh < 4)
		return;

	const uint8_t *copy = copy_region(f, base, stride, px, x0, y0, rw, rh);
	const size_t copy_stride = (size_t)rw * px;

	for (int y = y0; y < y1; y++) {
		float t;
		if ((float)y <= chin_y) {
			t = ((float)y - band_top) / (chin_y - band_top + 1.0f);
			t = clampf(t, 0.0f, 1.0f);
			t *= t; /* ramp up toward the tip */
		} else {
			t = 1.0f - ((float)y - chin_y) / (band_bot - chin_y + 1.0f);
			t = clampf(t, 0.0f, 1.0f);
		}
		const float c = cmax * t;
		if (c <= 0.001f)
			continue;

		uint8_t *row = base + (size_t)y * stride;
		for (int x = x0; x < x1; x++) {
			const float u = ((float)x - cx) / xr; /* [-1,1] */
			const float feather = 1.0f - u * u;
			if (feather <= 0.0f)
				continue;
			const float sx = cx + ((float)x - cx) * (1.0f + c * feather) - x0;
			row[(size_t)x * px] = bsample(copy, (uint32_t)copy_stride, px, rw, rh, sx, (float)(y - y0));
		}
	}
}

/* Rosy skin + lip tint (chroma domain) and epsilon-filter skin
 * smoothing (luma domain), both feathered by the face ellipse. */
static void apply_color_smooth(struct beauty_data *f, const struct plane_view *v, double rosy, double lips,
			       double smooth)
{
	const float ecx = f->fx + f->fw * 0.5f;
	const float ecy = f->fy + f->fh_eff * 0.5f;
	const float erx = f->fw * 0.52f;
	const float ery = f->fh_eff * 0.55f;

	/* chroma pass: rosy + lips */
	if (rosy > 0.001 || lips > 0.001) {
		const uint32_t cx0 = (uint32_t)clampf((f->fx - 0.05f * f->fw), 0, (float)v->w) >> v->csx;
		const uint32_t cy0 = (uint32_t)clampf((f->fy - 0.05f * f->fh_eff), 0, (float)v->h) >> v->csy;
		const uint32_t cx1 = (uint32_t)clampf((f->fx + 1.05f * f->fw), 0, (float)(v->w - 1)) >> v->csx;
		const uint32_t cy1 = (uint32_t)clampf((f->fy + 1.05f * f->fh_eff), 0, (float)(v->h - 1)) >> v->csy;

		const float lip_top = f->fy + 0.60f * f->fh_eff;
		const float lip_half_w = 0.28f * f->fw;
		const int lip_cr_gate = f->skin_cr_mean + 5;

		for (uint32_t cy = cy0; cy <= cy1 && cy < v->ch; cy++) {
			const float ly = (float)(cy << v->csy);
			const float ny = (ly - ecy) / ery;
			for (uint32_t cx = cx0; cx <= cx1 && cx < v->cw; cx++) {
				const float lx = (float)(cx << v->csx);
				const float nx = (lx - ecx) / erx;
				const float e = nx * nx + ny * ny;
				const float ew = clampf((1.1f - e) / 0.35f, 0.0f, 1.0f);
				if (ew <= 0.0f)
					continue;

				uint8_t *pcb = v->cb + (size_t)cy * v->cbs + (size_t)cx * v->cp;
				uint8_t *pcr = v->cr + (size_t)cy * v->crs + (size_t)cx * v->cp;
				const int cbv = *pcb, crv = *pcr;
				const int yv = view_y(v, cx << v->csx, cy << v->csy);
				if (!is_skin(yv, cbv, crv))
					continue;

				float dcr = (float)rosy * 12.0f * ew;

				if (lips > 0.001 && ly >= lip_top && ly <= f->chin_y &&
				    fabsf(lx - ecx) <= lip_half_w && crv >= lip_cr_gate) {
					const float lf = clampf((float)(crv - lip_cr_gate) / 10.0f, 0.0f, 1.0f);
					dcr += (float)lips * 18.0f * lf;
				}

				*pcr = (uint8_t)clampi(crv + (int)(dcr + 0.5f), 16, 240);
				*pcb = (uint8_t)clampi(cbv - (int)(dcr * 0.4f + 0.5f), 16, 240);
			}
		}
	}

	/* luma pass: epsilon smoothing over the face box */
	if (smooth > 0.001) {
		const uint32_t x0 = (uint32_t)clampf(f->fx, 0, (float)(v->w - 1));
		const uint32_t y0 = (uint32_t)clampf(f->fy, 0, (float)(v->h - 1));
		const uint32_t x1 = (uint32_t)clampf(f->fx + f->fw, 0, (float)v->w);
		const uint32_t y1 = (uint32_t)clampf(f->fy + f->fh_eff, 0, (float)v->h);
		const uint32_t rw = x1 - x0, rh = y1 - y0;
		if (rw < 8 || rh < 8)
			return;

		const int r = clampi((int)(f->fw / 64.0f), 2, 6);
		const int win = 2 * r + 1;

		uint16_t *hmean = (uint16_t *)get_scratch(f, SCRATCH_BLUR_H, (size_t)rw * rh * sizeof(uint16_t));
		uint16_t *vmean = (uint16_t *)get_scratch(f, SCRATCH_BLUR_V, (size_t)rw * rh * sizeof(uint16_t));

		for (uint32_t y = 0; y < rh; y++) {
			const uint8_t *row = v->y + (size_t)(y0 + y) * v->ys + (size_t)x0 * v->yp;
			uint16_t *out = hmean + (size_t)y * rw;
			int sum = 0;
			for (int x = -r; x <= r; x++)
				sum += row[(size_t)clampi(x, 0, (int)rw - 1) * v->yp];
			for (uint32_t x = 0; x < rw; x++) {
				out[x] = (uint16_t)(sum / win);
				const int add = clampi((int)x + r + 1, 0, (int)rw - 1);
				const int sub = clampi((int)x - r, 0, (int)rw - 1);
				sum += row[(size_t)add * v->yp] - row[(size_t)sub * v->yp];
			}
		}
		for (uint32_t x = 0; x < rw; x++) {
			int sum = 0;
			for (int y = -r; y <= r; y++)
				sum += hmean[(size_t)clampi(y, 0, (int)rh - 1) * rw + x];
			for (uint32_t y = 0; y < rh; y++) {
				vmean[(size_t)y * rw + x] = (uint16_t)(sum / win);
				const int add = clampi((int)y + r + 1, 0, (int)rh - 1);
				const int sub = clampi((int)y - r, 0, (int)rh - 1);
				sum += hmean[(size_t)add * rw + x] - hmean[(size_t)sub * rw + x];
			}
		}

		const int T = 13; /* max luma flattening per pixel */
		for (uint32_t y = 0; y < rh; y++) {
			const float ny = ((float)(y0 + y) - ecy) / ery;
			uint8_t *row = v->y + (size_t)(y0 + y) * v->ys;
			const uint16_t *mrow = vmean + (size_t)y * rw;
			for (uint32_t x = 0; x < rw; x++) {
				const float nx = ((float)(x0 + x) - ecx) / erx;
				const float e = nx * nx + ny * ny;
				const float ew = clampf((1.1f - e) / 0.35f, 0.0f, 1.0f);
				if (ew <= 0.0f)
					continue;

				const uint32_t lx = x0 + x;
				const uint32_t ly = y0 + y;
				const int cbv = view_cb(v, lx >> v->csx, ly >> v->csy);
				const int crv = view_cr(v, lx >> v->csx, ly >> v->csy);
				uint8_t *py = row + (size_t)lx * v->yp;
				if (!is_skin(*py, cbv, crv))
					continue;

				const int d = clampi((int)mrow[x] - (int)*py, -T, T);
				*py = (uint8_t)clampi((int)*py + (int)(d * (float)smooth * ew), 0, 255);
			}
		}
	}
}

static void apply_warps(struct beauty_data *f, const struct plane_view *v, double eyes, double chin)
{
	/* Both warps are anchored to the eye pair; without a stable one
	 * (ramp fades to 0 on loss) geometry stays untouched. All sizes
	 * derive from the pupil distance, which is far more stable than
	 * the skin blob's dimensions. */
	if (!f->eyes_valid || f->ramp <= 0.02f)
		return;

	const float sep = f->eye_rx - f->eye_lx;
	const float sx = (float)(1 << v->csx), sy = (float)(1 << v->csy);

	if (eyes > 0.001) {
		float R = 0.30f * sep;
		if (R > sep * 0.42f)
			R = sep * 0.42f;
		const float a = 0.35f * (float)eyes * f->ramp;

		if (R >= 5.0f) {
			for (int e = 0; e < 2; e++) {
				const float ex = e ? f->eye_rx : f->eye_lx;
				const float ey = e ? f->eye_ry : f->eye_ly;
				warp_magnify(f, v->y, v->ys, v->yp, v->w, v->h, ex, ey, R, R, a);
				warp_magnify(f, v->cb, v->cbs, v->cp, v->cw, v->ch, ex / sx, ey / sy, R / sx, R / sy,
					     a);
				warp_magnify(f, v->cr, v->crs, v->cp, v->cw, v->ch, ex / sx, ey / sy, R / sx, R / sy,
					     a);
			}
		}
	}

	if (chin > 0.001 && f->chin_y > 0) {
		const float band_top = f->chin_y - 0.55f * sep;
		const float band_bot = f->chin_y + 0.15f * sep;
		const float xr = 1.15f * sep;
		const float cmax = 0.20f * (float)chin * f->ramp;

		if (xr >= 8.0f) {
			warp_chin(f, v->y, v->ys, v->yp, v->w, v->h, f->chin_x, f->chin_y, band_top, band_bot, xr,
				  cmax);
			warp_chin(f, v->cb, v->cbs, v->cp, v->cw, v->ch, f->chin_x / sx, f->chin_y / sy, band_top / sy,
				  band_bot / sy, xr / sx, cmax);
			warp_chin(f, v->cr, v->crs, v->cp, v->cw, v->ch, f->chin_x / sx, f->chin_y / sy, band_top / sy,
				  band_bot / sy, xr / sx, cmax);
		}
	}
}

static struct obs_source_frame *beauty_video(void *data, struct obs_source_frame *frame)
{
	struct beauty_data *f = data;
	struct plane_view v;

	if (!make_view(frame, &v)) {
		if (!f->warned_format) {
			blog(LOG_WARNING, "[beauty: '%s'] unsupported format %d, passing through",
			     obs_source_get_name(f->context), (int)frame->format);
			f->warned_format = true;
		}
		return frame;
	}

	pthread_mutex_lock(&f->mutex);
	const double smooth = f->smooth, rosy = f->rosy, lips = f->lips, eyes = f->eyes, chin = f->chin;
	pthread_mutex_unlock(&f->mutex);

	detect_face(f, &v);

	/* Warp strength fades in only once the eye track has been
	 * continuously stable for a while, and fades back out (rather
	 * than popping off) when it degrades. Color effects don't warp
	 * geometry, so they don't need the ramp. */
	const bool warps_ok =
		f->face_present && f->eyes_valid && f->frontal && f->stable_frames >= 8 && f->eye_hold <= 3;
	f->ramp = clampf(f->ramp + (warps_ok ? 0.08f : -0.12f), 0.0f, 1.0f);

	if (!f->face_present)
		return frame;
	if (smooth <= 0.001 && rosy <= 0.001 && lips <= 0.001 && eyes <= 0.001 && chin <= 0.001)
		return frame;

	apply_color_smooth(f, &v, rosy, lips, smooth);
	apply_warps(f, &v, eyes, chin);

	return frame;
}

struct obs_source_info beauty_filter = {
	.id = "beauty_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC,
	.get_name = beauty_name,
	.create = beauty_create,
	.destroy = beauty_destroy,
	.update = beauty_update,
	.get_defaults = beauty_defaults,
	.get_properties = beauty_properties,
	.filter_video = beauty_video,
};
