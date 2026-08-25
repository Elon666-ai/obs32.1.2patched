#include "quality-score.h"

#include <util/base.h>
#include <util/platform.h>
#include <util/threading.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

/* Ring/queue depths. 180 reference frames is ~3s at 60fps (~23MB at the
 * 480x270 detection size for a 1080p stream); decode latency behind the
 * matching raw frame is normally 1-3 frames, so this is generous. A
 * packet queue deeper than 120 means the decoder has fallen multiple
 * seconds behind realtime - at that point the whole queue is dropped
 * and decoding resyncs at the next keyframe rather than lagging
 * forever. */
static constexpr size_t MAX_REFS = 180;
static constexpr size_t MAX_QUEUE = 120;
static constexpr int64_t SAMPLE_INTERVAL_US = 500000; /* 2 scored frames/s */
static constexpr int64_t REPORT_INTERVAL_US = 10000000; /* one log line / 10s */
/* Persistent avcodec errors (corrupt feed, unsupported profile) turn
 * the scorer off instead of spamming the log every packet. */
static constexpr uint32_t MAX_CONSECUTIVE_ERRORS = 30;

static AVCodecID codec_id_from_name(const char *codec)
{
	if (!codec)
		return AV_CODEC_ID_NONE;
	if (strcmp(codec, "h264") == 0)
		return AV_CODEC_ID_H264;
	if (strcmp(codec, "hevc") == 0)
		return AV_CODEC_ID_HEVC;
	if (strcmp(codec, "av1") == 0)
		return AV_CODEC_ID_AV1;
	return AV_CODEC_ID_NONE;
}

/* Mean SSIM over non-overlapping 8x8 luma blocks (x264-style), standard
 * K1/K2 constants. Identical planes give exactly 1.0. */
static double compute_ssim(const uint8_t *a, const uint8_t *b, uint32_t w, uint32_t h)
{
	constexpr double C1 = 6.5025; /* (0.01 * 255)^2 */
	constexpr double C2 = 58.5225; /* (0.03 * 255)^2 */

	double total = 0.0;
	size_t blocks = 0;

	for (uint32_t by = 0; by + 8 <= h; by += 8) {
		for (uint32_t bx = 0; bx + 8 <= w; bx += 8) {
			int32_t sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
			for (uint32_t y = 0; y < 8; y++) {
				const uint8_t *ra = a + (size_t)(by + y) * w + bx;
				const uint8_t *rb = b + (size_t)(by + y) * w + bx;
				for (uint32_t x = 0; x < 8; x++) {
					const int32_t pa = ra[x];
					const int32_t pb = rb[x];
					sa += pa;
					sb += pb;
					saa += pa * pa;
					sbb += pb * pb;
					sab += pa * pb;
				}
			}

			constexpr double n = 64.0;
			const double ma = sa / n;
			const double mb = sb / n;
			const double va = saa / n - ma * ma;
			const double vb = sbb / n - mb * mb;
			const double cov = sab / n - ma * mb;

			total += ((2.0 * ma * mb + C1) * (2.0 * cov + C2)) /
				 ((ma * ma + mb * mb + C1) * (va + vb + C2));
			blocks++;
		}
	}

	return blocks ? total / blocks : 1.0;
}

/* Luma PSNR in dB, capped at 99 for (near-)identical planes. */
static double compute_psnr(const uint8_t *a, const uint8_t *b, uint32_t w, uint32_t h)
{
	uint64_t se = 0;
	const size_t n = (size_t)w * h;
	for (size_t i = 0; i < n; i++) {
		const int32_t d = (int32_t)a[i] - (int32_t)b[i];
		se += (uint64_t)(d * d);
	}
	if (!se)
		return 99.0;
	const double mse = (double)se / (double)n;
	return std::min(99.0, 10.0 * log10(255.0 * 255.0 / mse));
}

QualityScorer::~QualityScorer()
{
	Stop();
}

void QualityScorer::Start(obs_output_t *output)
{
	if (started)
		return;

	obs_encoder_t *encoder = obs_output_get_video_encoder2(output, 0);
	if (!encoder) {
		blog(LOG_WARNING, "[obs-webrtc] quality scorer: no video encoder on layer 0, not starting");
		return;
	}

	const char *codec = obs_encoder_get_codec(encoder);
	const AVCodecID codec_id = codec_id_from_name(codec);
	const AVCodec *avdec = codec_id != AV_CODEC_ID_NONE ? avcodec_find_decoder(codec_id) : nullptr;
	if (!avdec) {
		blog(LOG_WARNING, "[obs-webrtc] quality scorer: no decoder available for codec '%s', not starting",
		     codec ? codec : "(null)");
		return;
	}

	const uint32_t enc_width = obs_encoder_get_width(encoder);
	const uint32_t enc_height = obs_encoder_get_height(encoder);
	if (!enc_width || !enc_height) {
		blog(LOG_WARNING, "[obs-webrtc] quality scorer: encoder has no dimensions yet, not starting");
		return;
	}

	decoder = avcodec_alloc_context3(avdec);
	if (!decoder)
		return;
	decoder->thread_count = 2;
	if (avcodec_open2(decoder, avdec, nullptr) < 0) {
		blog(LOG_WARNING, "[obs-webrtc] quality scorer: failed to open %s decoder, not starting", codec);
		avcodec_free_context(&decoder);
		return;
	}

	/* SSIM-standard downsample factor round(min(w,h)/256): 1080p -> 4
	 * (480x270), 720p -> 3, <=540p -> compared at full size. */
	const uint32_t f = std::max(1u, (std::min(enc_width, enc_height) + 128) / 256);
	det_width = std::max(16u, (enc_width / f) & ~1u);
	det_height = std::max(16u, (enc_height / f) & ~1u);

	video_t *video = obs_output_video(output);
	const struct video_output_info *voi = video ? video_output_get_info(video) : nullptr;
	if (voi && voi->fps_num)
		frame_dur_us = (int64_t)voi->fps_den * 1000000 / voi->fps_num;

	dec_frame = av_frame_alloc();
	det_y.assign((size_t)det_width * det_height, 0);
	det_u.assign((size_t)(det_width / 2) * (det_height / 2), 0);
	det_v.assign((size_t)(det_width / 2) * (det_height / 2), 0);

	target_encoder = encoder;
	score_output = output;
	dead = false;
	clock_offset_us = INT64_MAX;
	wait_keyframe = true;
	next_sample_us = 0;
	next_report_us = 0;
	win_samples = 0;
	win_ssim_sum = 0.0;
	win_ssim_min = 1.0;
	win_psnr_sum = 0.0;
	win_misses = 0;
	win_decode_errors = 0;
	resyncs = 0;
	reported_resyncs = 0;
	consecutive_errors = 0;
	refs.clear();

	running = true;
	decode_thread = std::thread(&QualityScorer::DecodeLoop, this);
	started = true;

	/* Register the reference tap last so RawVideo's started check holds.
	 * Every frame (divisor 1) is needed: which instants get scored is
	 * only known decode-side, and matching is exact per frame. */
	struct video_scale_info conversion = {};
	conversion.format = VIDEO_FORMAT_I420;
	conversion.width = det_width;
	conversion.height = det_height;
	conversion.range = VIDEO_RANGE_DEFAULT;
	conversion.colorspace = VIDEO_CS_DEFAULT;
	obs_add_raw_video_callback2(&conversion, 1, RawVideo, this);

	blog(LOG_INFO,
	     "[obs-webrtc] quality scorer started: decoding %s layer 0 (%ux%u), comparing at %ux%u, "
	     "%lld samples/s (program feed = 100)",
	     codec, enc_width, enc_height, det_width, det_height, (long long)(1000000 / SAMPLE_INTERVAL_US));
}

void QualityScorer::Stop()
{
	if (!started)
		return;
	started = false;

	/* Removal waits for in-flight callbacks; RawVideo takes ref_mutex,
	 * so don't hold it here. */
	obs_remove_raw_video_callback(RawVideo, this);

	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		running = false;
		for (AVPacket *pkt : queue)
			av_packet_free(&pkt);
		queue.clear();
	}
	queue_cv.notify_all();
	if (decode_thread.joinable())
		decode_thread.join();

	if (sws) {
		sws_freeContext(sws);
		sws = nullptr;
	}
	if (dec_frame)
		av_frame_free(&dec_frame);
	if (decoder)
		avcodec_free_context(&decoder);

	{
		std::lock_guard<std::mutex> lock(ref_mutex);
		refs.clear();
	}

	target_encoder = nullptr;
	score_output = nullptr;
	blog(LOG_INFO, "[obs-webrtc] quality scorer stopped");
}

void QualityScorer::OnPacket(struct encoder_packet *packet)
{
	if (!started || dead)
		return;
	if (packet->type != OBS_ENCODER_VIDEO || packet->encoder != target_encoder)
		return;

	/* dts_usec is in the output's own interleaving-relative clock, not
	 * the reference tap's absolute clock (see header comment on
	 * clock_offset_us) - pts/dts delta correction still applies within
	 * that clock; with bf=0 pts==dts and the correction is zero, but
	 * compute it anyway in case an encoder reorders after all. */
	int64_t pts_us = packet->dts_usec;
	if (packet->pts != packet->dts && packet->timebase_den)
		pts_us += (packet->pts - packet->dts) * 1000000 * packet->timebase_num / packet->timebase_den;

	{
		const int64_t now_us = (int64_t)(os_gettime_ns() / 1000);
		const int64_t implied_offset = now_us - pts_us;
		int64_t prev = clock_offset_us.load(std::memory_order_relaxed);
		while (implied_offset < prev &&
		       !clock_offset_us.compare_exchange_weak(prev, implied_offset, std::memory_order_relaxed))
			;
	}

	std::lock_guard<std::mutex> lock(queue_mutex);
	if (!running)
		return;

	if (wait_keyframe) {
		if (!packet->keyframe)
			return;
		wait_keyframe = false;
	}

	if (queue.size() >= MAX_QUEUE) {
		/* Decoder fell way behind realtime: drop everything and pick
		 * the stream back up at the next keyframe. */
		for (AVPacket *pkt : queue)
			av_packet_free(&pkt);
		queue.clear();
		wait_keyframe = true;
		resyncs.fetch_add(1);
		return;
	}

	AVPacket *pkt = av_packet_alloc();
	if (!pkt)
		return;
	if (av_new_packet(pkt, (int)packet->size) < 0) {
		av_packet_free(&pkt);
		return;
	}
	memcpy(pkt->data, packet->data, packet->size);
	pkt->pts = pts_us;
	pkt->dts = pts_us;
	if (packet->keyframe)
		pkt->flags |= AV_PKT_FLAG_KEY;

	queue.push_back(pkt);
	queue_cv.notify_one();
}

void QualityScorer::RawVideo(void *param, struct video_data *frame)
{
	QualityScorer *self = static_cast<QualityScorer *>(param);

	std::lock_guard<std::mutex> lock(self->ref_mutex);
	if (!self->started || !frame->data[0])
		return;

	RefFrame ref;
	ref.ts_us = (int64_t)(frame->timestamp / 1000);
	ref.luma.resize((size_t)self->det_width * self->det_height);
	for (uint32_t y = 0; y < self->det_height; y++)
		memcpy(ref.luma.data() + (size_t)y * self->det_width, frame->data[0] + (size_t)y * frame->linesize[0],
		       self->det_width);

	self->refs.push_back(std::move(ref));
	while (self->refs.size() > MAX_REFS)
		self->refs.pop_front();
}

void QualityScorer::DecodeLoop()
{
	os_set_thread_name("obs-webrtc: quality scorer");

	while (true) {
		AVPacket *pkt = nullptr;
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			queue_cv.wait(lock, [this] { return !running || !queue.empty(); });
			if (!running)
				return;
			pkt = queue.front();
			queue.pop_front();
		}

		int ret = avcodec_send_packet(decoder, pkt);
		if (ret == AVERROR(EAGAIN)) {
			DrainDecoder();
			ret = avcodec_send_packet(decoder, pkt);
		}
		av_packet_free(&pkt);

		if (ret < 0 && ret != AVERROR_EOF) {
			win_decode_errors++;
			if (++consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
				blog(LOG_ERROR,
				     "[obs-webrtc] quality scorer: %u consecutive decode errors, giving up "
				     "(stream itself is unaffected)",
				     consecutive_errors);
				dead = true;
				std::lock_guard<std::mutex> lock(queue_mutex);
				for (AVPacket *p : queue)
					av_packet_free(&p);
				queue.clear();
			}
			continue;
		}
		consecutive_errors = 0;

		DrainDecoder();
	}
}

void QualityScorer::DrainDecoder()
{
	while (avcodec_receive_frame(decoder, dec_frame) == 0) {
		CompareFrame(dec_frame);
		av_frame_unref(dec_frame);
	}
}

void QualityScorer::CompareFrame(AVFrame *frame)
{
	if (frame->pts == AV_NOPTS_VALUE)
		return;

	const int64_t offset = clock_offset_us.load(std::memory_order_relaxed);
	if (offset == INT64_MAX)
		return; // OnPacket() hasn't calibrated the clock offset yet

	const int64_t pts = frame->pts + offset;
	if (pts < next_sample_us)
		return;

	/* Pull the matching reference out of the ring. pts only increases,
	 * so everything older than the tolerance window is dead weight.
	 * The window is a full frame duration (not half) since the offset
	 * above is calibrated, not exact - it tracks the minimum observed
	 * encode/delivery latency, so a frame arriving with slightly above
	 * that minimum reads as marginally "later" than its true content
	 * time. */
	RefFrame ref;
	bool found = false;
	{
		std::lock_guard<std::mutex> lock(ref_mutex);
		while (!refs.empty() && refs.front().ts_us < pts - frame_dur_us)
			refs.pop_front();
		if (!refs.empty() && refs.front().ts_us <= pts + frame_dur_us) {
			ref = std::move(refs.front());
			refs.pop_front();
			found = true;
		}
	}

	next_sample_us = pts + SAMPLE_INTERVAL_US;
	if (!next_report_us)
		next_report_us = pts + REPORT_INTERVAL_US;

	if (!found) {
		/* Reference already evicted (decoder lagging several seconds)
		 * or not yet arrived (shouldn't happen - the raw frame always
		 * precedes its packet). Counted, not scored. */
		win_misses++;
	} else {
		/* Same fast-bilinear kernel the libobs raw-video conversion
		 * uses for the reference tap, so a hypothetical lossless
		 * encode compares equal instead of eating a kernel-mismatch
		 * penalty. yuv->yuv conversion applies no range change. */
		sws = sws_getCachedContext(sws, frame->width, frame->height, (AVPixelFormat)frame->format,
					   (int)det_width, (int)det_height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR,
					   nullptr, nullptr, nullptr);
		if (!sws)
			return;

		uint8_t *dst[3] = {det_y.data(), det_u.data(), det_v.data()};
		const int dst_linesize[3] = {(int)det_width, (int)(det_width / 2), (int)(det_width / 2)};
		sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dst_linesize);

		/* SSIM can go negative for structurally unrelated frames (a
		 * mismatch, not a quality level) - floor the score at 0. */
		const double ssim = std::max(0.0, compute_ssim(ref.luma.data(), det_y.data(), det_width, det_height));
		const double psnr = compute_psnr(ref.luma.data(), det_y.data(), det_width, det_height);

		win_samples++;
		win_ssim_sum += ssim;
		win_ssim_min = std::min(win_ssim_min, ssim);
		win_psnr_sum += psnr;

		/* Per-sample score for the frontend's preview overlay. The
		 * output outlives this thread (Stop() joins it before the
		 * output is torn down), so score_output stays valid here. */
		uint8_t stack[128];
		calldata_t data;
		calldata_init_fixed(&data, stack, sizeof(stack));
		calldata_set_ptr(&data, "output", score_output);
		calldata_set_float(&data, "score", ssim * 100.0);
		calldata_set_float(&data, "psnr", psnr);
		signal_handler_signal(obs_output_get_signal_handler(score_output), "quality_score", &data);
	}

	if (pts < next_report_us)
		return;
	next_report_us = pts + REPORT_INTERVAL_US;

	const uint32_t total_resyncs = resyncs.load();
	const uint32_t new_resyncs = total_resyncs - reported_resyncs;
	reported_resyncs = total_resyncs;

	if (win_samples) {
		blog(LOG_INFO,
		     "[obs-webrtc] stream quality score: %.1f avg / %.1f min of 100 "
		     "(Y-SSIM vs program feed, PSNR %.1f dB avg, %u samples)",
		     win_ssim_sum / win_samples * 100.0, win_ssim_min * 100.0, win_psnr_sum / win_samples,
		     win_samples);
	}
	if (win_misses || win_decode_errors || new_resyncs) {
		blog(LOG_WARNING,
		     "[obs-webrtc] quality scorer: %u unmatched samples, %u decode errors, %u queue resyncs "
		     "in the last window",
		     win_misses, win_decode_errors, new_resyncs);
	}

	win_samples = 0;
	win_ssim_sum = 0.0;
	win_ssim_min = 1.0;
	win_psnr_sum = 0.0;
	win_misses = 0;
	win_decode_errors = 0;
}
