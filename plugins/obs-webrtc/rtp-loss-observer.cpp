#include "rtp-loss-observer.h"

void RtpLossObserver::incoming(rtc::message_vector &messages, const rtc::message_callback &)
{
	for (const auto &message : messages) {
		if (!message || message->type != rtc::Message::Control)
			continue;
		if (message->size() < sizeof(rtc::RtcpHeader))
			continue;
		ParseRtcp(reinterpret_cast<const uint8_t *>(message->data()), message->size());
	}
}

void RtpLossObserver::ParseRtcp(const uint8_t *data, size_t size)
{
	std::lock_guard<std::mutex> lock(mtx);

	size_t offset = 0;
	while (offset + sizeof(rtc::RtcpHeader) <= size) {
		const auto *header = reinterpret_cast<const rtc::RtcpHeader *>(data + offset);
		const size_t length = header->lengthInBytes();
		if (length < sizeof(rtc::RtcpHeader) || offset + length > size)
			break;

		switch (header->payloadType()) {
		case 200: { // Sender Report
			const auto *sr = reinterpret_cast<const rtc::RtcpSr *>(header);
			const int count = header->reportCount();
			for (int i = 0; i < count; i++)
				Accumulate(sr->getReportBlock(i));
			break;
		}
		case 201: { // Receiver Report
			const auto *rr = reinterpret_cast<const rtc::RtcpRr *>(header);
			const int count = header->reportCount();
			for (int i = 0; i < count; i++)
				Accumulate(rr->getReportBlock(i));
			break;
		}
		default:
			break;
		}

		offset += length;
	}
}

void RtpLossObserver::Accumulate(const rtc::RtcpReportBlock *block)
{
	if (!block)
		return;

	const uint32_t ssrc = block->getSSRC();
	const uint32_t highest = block->extendedHighestSeqNo();
	const uint32_t lost = block->getPacketsLostCount();

	SsrcState &state = ssrcs[ssrc];

	// First report for this SSRC, or the sender's counters went backwards
	// (SSRC reuse after a renegotiation): re-baseline instead of booking a
	// bogus jump.
	if (!state.initialized || highest < state.last_extended_highest || lost < state.last_lost) {
		state.initialized = true;
		state.last_extended_highest = highest;
		state.last_lost = lost;
		return;
	}

	const uint32_t expected_delta = highest - state.last_extended_highest;
	const uint32_t lost_delta = lost - state.last_lost;
	state.last_extended_highest = highest;
	state.last_lost = lost;

	if (expected_delta == 0)
		return;

	// Guard against an inconsistent report (lost should never exceed the
	// number of new sequence numbers expected in the interval).
	const uint32_t capped_lost = lost_delta > expected_delta ? expected_delta : lost_delta;

	expected_total.fetch_add(expected_delta, std::memory_order_relaxed);
	lost_total.fetch_add(capped_lost, std::memory_order_relaxed);
}
