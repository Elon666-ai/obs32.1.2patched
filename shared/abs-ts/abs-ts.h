#ifndef ABS_TS_H
#define ABS_TS_H
#ifdef __cplusplus
extern "C" {
#endif

/* Packet callback that stamps every video packet with an absolute
 * (NTP-corrected) millisecond timestamp, carried as an unregistered
 * user-data SEI message (AVC/HEVC) or a user-private metadata OBU
 * (AV1). Register with obs_output_add_packet_callback() on whichever
 * output(s) should carry it; no corresponding "destroy" call is
 * needed since this callback holds no per-output state.
 *
 * See docs/obs-abs-timestamp-protocol.md for the wire format.
 */
void abs_ts_sei_inject(obs_output_t *output, struct encoder_packet *pkt, struct encoder_packet_time *pkt_time,
		       void *param);

#ifdef __cplusplus
}
#endif
#endif
