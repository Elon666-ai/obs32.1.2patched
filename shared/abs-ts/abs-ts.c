#include "obs.h"
#include "abs-ts.h"

#include "caption/mpeg.h"
#include "obs-av1.h"
#include "util/ntp-clock.h"
#include "util/darray.h"

#include <string.h>

/* 16-byte "UUID" identifying this SEI/OBU payload, spelled out in ASCII
 * so it's easy to recognize in a hex dump. See
 * docs/obs-abs-timestamp-protocol.md for the full wire format. */
#define SEI_UUID_SIZE 16
static const uint8_t abs_ts_uuid[SEI_UUID_SIZE] = {'O', 'B', 'S', '-', 'A', 'B', 'S', 'T',
						   'S', '-', 'S', 'E', 'I', '-', 'v', '1'};

static const uint8_t nal_start[4] = {0, 0, 0, 1};

static size_t extract_buffer_from_sei(sei_t *sei, uint8_t **data_out)
{
	if (!sei || !sei->head)
		return 0;

	/* Only one message was ever appended, so no need to iterate. */
	sei_message_t *msg = sei_message_head(sei);
	int payload_size = (int)sei_message_size(msg);
	uint8_t *payload_data = sei_message_data(msg);
	*data_out = bmalloc(payload_size);
	memcpy(*data_out, payload_data, payload_size);
	return payload_size;
}

/* Returns the offset (from the start of the buffer, i.e. including its
 * leading annex-B start code) of the first VCL NAL (slice, type 1/5) in an
 * AVC access unit, or SIZE_MAX if none is found. AVC has no "suffix SEI"
 * concept (unlike HEVC) - a SEI NAL must precede the VCL NALs it applies
 * to within the access unit (after AUD/SPS/PPS is fine, but never after
 * the slice data). Appending it at the end instead produces a bitstream
 * that decoders reject or refuse to associate the SEI with the frame
 * ("Late SEI is not implemented" in ffmpeg) - this broke Tencent Cloud's
 * H264 decoding of streams forwarded there by mmx, even though the
 * SEI-carrying bitstream played fine locally (browsers/players that
 * re-parse NALs per frame rather than requiring strict SEI-before-VCL
 * ordering tolerate it).
 */
static size_t avc_find_first_vcl_offset(const uint8_t *data, size_t size)
{
	size_t i = 0;

	while (i + 4 <= size) {
		bool has4 = memcmp(data + i, nal_start, 4) == 0;
		bool has3 = !has4 && memcmp(data + i, nal_start + 1, 3) == 0;
		if (!has4 && !has3) {
			i++;
			continue;
		}

		size_t hdr_len = has4 ? 4 : 3;
		size_t nal_type_pos = i + hdr_len;
		if (nal_type_pos >= size)
			break;

		uint8_t nal_type = data[nal_type_pos] & 0x1F;
		if (nal_type == 1 || nal_type == 5)
			return i;

		i = nal_type_pos;
	}

	return SIZE_MAX;
}

void abs_ts_sei_inject(obs_output_t *output, struct encoder_packet *pkt, struct encoder_packet_time *pkt_time,
		       void *param)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(pkt_time);

	if (!output || !pkt || pkt->type != OBS_ENCODER_VIDEO || !pkt->encoder)
		return;

	const char *codec = obs_encoder_get_codec(pkt->encoder);
	bool avc = strcmp(codec, "h264") == 0;
	bool av1 = strcmp(codec, "av1") == 0;
	bool hevc = false;
#ifdef ENABLE_HEVC
	hevc = strcmp(codec, "hevc") == 0;
#endif
	if (!avc && !hevc && !av1)
		return;

	uint8_t payload[SEI_UUID_SIZE + 8];
	memcpy(payload, abs_ts_uuid, SEI_UUID_SIZE);
	uint64_t ts_ms = ntp_clock_now_ms();
	for (int i = 0; i < 8; i++)
		payload[SEI_UUID_SIZE + i] = (uint8_t)(ts_ms >> (8 * (7 - i)));

	uint8_t hevc_nal_header[2] = {0, 0};
	if (hevc) {
		size_t nal_header_index_start;
		if (memcmp(pkt->data, nal_start + 1, 3) == 0)
			nal_header_index_start = 3;
		else if (memcmp(pkt->data, nal_start, 4) == 0)
			nal_header_index_start = 4;
		else
			return; /* not a valid annex-b start code, bail out */

		hevc_nal_header[0] = pkt->data[nal_header_index_start];
		hevc_nal_header[1] = pkt->data[nal_header_index_start + 1];
	}

	size_t avc_vcl_offset = SIZE_MAX;
	if (avc) {
		avc_vcl_offset = avc_find_first_vcl_offset(pkt->data, pkt->size);
		if (avc_vcl_offset == SIZE_MAX)
			return; /* no VCL NAL found, nothing to annotate */
	}

	struct encoder_packet backup = *pkt;
	long ref = 1;
	DARRAY(uint8_t) out_data;
	da_init(out_data);
	da_push_back_array(out_data, (uint8_t *)&ref, sizeof(ref));

	sei_t sei;
	sei_init(&sei, 0.0);
	sei_message_t *msg = sei_message_new(sei_type_user_data_unregistered, payload, sizeof(payload));
	sei_message_append(&sei, msg);

	uint8_t *data = NULL;
	size_t size = 0;

	if (avc || hevc) {
		data = bmalloc(sei_render_size(&sei));
		size = sei_render(&sei, data);
	}

	if (avc) {
		/* Insert before the first VCL NAL (after any AUD/SPS/PPS),
		 * since AVC has no suffix-SEI concept - see
		 * avc_find_first_vcl_offset() for why appending at the end
		 * (as this used to do) breaks strict decoders. */
		da_push_back_array(out_data, pkt->data, avc_vcl_offset);
		da_push_back_array(out_data, nal_start, 4);
		da_push_back_array(out_data, data, size);
		da_push_back_array(out_data, pkt->data + avc_vcl_offset, pkt->size - avc_vcl_offset);
	} else if (hevc) {
		da_push_back_array(out_data, pkt->data, pkt->size);
		/* Suffix SEI (NAL type 40): explicitly meant to follow the
		 * VCL NALs it applies to, so appending here is spec-correct
		 * for HEVC (unlike AVC, which has no suffix/prefix concept). */
		da_push_back_array(out_data, nal_start + 1, 3);

		const uint8_t suffix_sei_nal_type = 40;
		uint8_t first_byte = (suffix_sei_nal_type << 1) | (0x01 & hevc_nal_header[0]);
		hevc_nal_header[0] = first_byte;
		da_push_back_array(out_data, hevc_nal_header, 2);
		da_push_back_array(out_data, &data[1], size - 1);
	} else if (av1) {
		da_push_back_array(out_data, pkt->data, pkt->size);
		uint8_t *sei_payload = NULL;
		size_t sei_payload_size = extract_buffer_from_sei(&sei, &sei_payload);
		uint8_t *obu_buffer = NULL;
		size_t obu_buffer_size = 0;
		metadata_obu(sei_payload, sei_payload_size, &obu_buffer, &obu_buffer_size, METADATA_TYPE_USER_PRIVATE_6);
		if (sei_payload)
			bfree(sei_payload);
		if (obu_buffer) {
			da_push_back_array(out_data, obu_buffer, obu_buffer_size);
			bfree(obu_buffer);
		}
	}

	if (data)
		bfree(data);
	sei_free(&sei);

	obs_encoder_packet_release(pkt);
	*pkt = backup;
	pkt->data = (uint8_t *)out_data.array + sizeof(ref);
	pkt->size = out_data.num - sizeof(ref);
}
