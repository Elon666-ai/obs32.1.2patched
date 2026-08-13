/******************************************************************************
    Copyright (C) 2026 by OBS Contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <QDateTime>
#include <QTimer>

namespace {

/* Qt::DayOfWeek is 1 (Monday) .. 7 (Sunday); index here is that minus 1,
 * matching the order OBSBasicSettings_Schedule.cpp writes per-slot
 * "Slot<N>.Day.<key>" config keys in. */
constexpr const char *kScheduleDayKeys[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

/* How often the schedule is checked against the wall clock. */
constexpr int kScheduleCheckIntervalMs = 5000;

} // namespace

void OBSBasic::InitSchedule()
{
	scheduleTimer = new QTimer(this);
	connect(scheduleTimer, &QTimer::timeout, this, &OBSBasic::CheckSchedule);
	scheduleTimer->start(kScheduleCheckIntervalMs);

	connect(this, &OBSBasic::profileSettingChanged, this,
		[this](const std::string &category, const std::string &) {
			if (category == "Schedule")
				ReloadSchedule();
		});

	emit ScheduleEnabledChanged(ScheduleEnabled());
	emit ScheduleFeatureEnabledChanged(ScheduleFeatureEnabled());

	CheckSchedule();
}

bool OBSBasic::ScheduleEnabled() const
{
	if (!activeConfiguration)
		return false;
	return config_get_bool(activeConfiguration, "Schedule", "Enabled");
}

// "Schedule"/"FeatureEnabled" - see the comment on the declaration in
// OBSBasic.hpp. Written from Settings > Stream > Scheduled Streaming
// Configuration (OBSBasicSettings::SaveScheduleSettings).
bool OBSBasic::ScheduleFeatureEnabled() const
{
	if (!activeConfiguration)
		return false;
	return config_get_bool(activeConfiguration, "Schedule", "FeatureEnabled");
}

// Called after the feature switch may have changed (from
// OBSBasicSettings::SaveScheduleSettings, via the profileSettingChanged ->
// ReloadSchedule path). If the feature was just turned off while a
// schedule was actively running, stop it the same way the control panel
// button's "stop" click would - don't leave a schedule silently running
// with no way to see/control it once its enabling switch is gone.
void OBSBasic::RefreshScheduleFeatureState()
{
	bool featureEnabled = ScheduleFeatureEnabled();
	emit ScheduleFeatureEnabledChanged(featureEnabled);

	if (!featureEnabled && ScheduleEnabled()) {
		config_set_bool(activeConfiguration, "Schedule", "Enabled", false);
		StopScheduledStream();
		emit ScheduleEnabledChanged(false);
		activeConfiguration.SaveSafe("tmp");
	}
}

void OBSBasic::ReloadSchedule()
{
	// RefreshScheduleFeatureState() may itself force Schedule/Enabled off
	// (and emit ScheduleEnabledChanged) if the feature switch was just
	// disabled - do that first so the plain ScheduleEnabledChanged emit
	// below reflects the final state, not a stale one.
	RefreshScheduleFeatureState();
	emit ScheduleEnabledChanged(ScheduleEnabled());
	CheckSchedule();
}

void OBSBasic::CheckSchedule()
{
	if (!ScheduleEnabled()) {
		scheduleStreamActive = false;
		return;
	}

	/* Not ready yet (e.g. called from the initial ActivateProfile() during
	 * startup, before ResetOutputs() has run) - CheckSchedule() will run
	 * again once InitSchedule() finishes setting up. */
	if (!outputHandler)
		return;

	/* Scheduled streaming only ever drives OBS's normal streaming
	 * start/stop path; if the user (or something else) already has a
	 * stream going/going down through some other route, don't fight it. */
	if (disableOutputsRef)
		return;

	QDateTime now = QDateTime::currentDateTime();
	int dayIdx = now.date().dayOfWeek() - 1; // Qt: 1=Monday -> 0-based
	if (dayIdx < 0 || dayIdx > 6)
		return;

	int nowMinutes = now.time().hour() * 60 + now.time().minute();
	std::string dayKey = kScheduleDayKeys[dayIdx];

	/* Streaming should be active if *any* slot covers the current weekday
	 * and time - slots are validated not to overlap on the same day at
	 * save time (see OBSBasicSettings::ValidateScheduleSlots), so at most
	 * one can ever match, but this doesn't rely on that. */
	bool shouldBeStreaming = false;
	int slotCount = (int)config_get_int(activeConfiguration, "Schedule", "Slot.Count");
	for (int i = 0; i < slotCount && !shouldBeStreaming; i++) {
		std::string prefix = "Slot" + std::to_string(i) + ".";

		bool dayEnabled = config_get_bool(activeConfiguration, "Schedule", (prefix + "Day." + dayKey).c_str());
		if (!dayEnabled)
			continue;

		int startMinutes = (int)config_get_int(activeConfiguration, "Schedule", (prefix + "Start").c_str());
		int endMinutes = (int)config_get_int(activeConfiguration, "Schedule", (prefix + "End").c_str());

		shouldBeStreaming = nowMinutes >= startMinutes && nowMinutes < endMinutes;
	}

	scheduleStreamActive = shouldBeStreaming;

	bool active = outputHandler && outputHandler->StreamingActive();
	if (shouldBeStreaming && !active && !streamingStarting) {
		blog(LOG_INFO, "Starting stream due to schedule");
		StartStreaming();
	} else if (!shouldBeStreaming && active && !streamingStopping) {
		blog(LOG_INFO, "Stopping stream due to schedule");
		StopStreaming();
	}
}

void OBSBasic::StopScheduledStream()
{
	/* Called right after "Schedule"/"Enabled" has been written to false
	 * on the same config_t that activeConfiguration/Config() point at,
	 * so ScheduleEnabled() already reads false by the time this runs.
	 * Manual control is restored separately by the
	 * ScheduleEnabledChanged(false) emit that the caller sends right
	 * after this (via profileSettingChanged, or directly from
	 * ScheduleButtonClicked()). */
	scheduleStreamActive = false;

	if (outputHandler && outputHandler->StreamingActive() && !streamingStopping) {
		blog(LOG_INFO, "Stopping stream: scheduled streaming was disabled");
		StopStreaming();
	}
}

void OBSBasic::ScheduleButtonClicked()
{
	/* Toggled from the "Start/Stop Scheduled Streaming" control panel
	 * button (see OBSBasicControls). Starting it flips Schedule/Enabled
	 * on and hands control over to CheckSchedule(), which then starts/
	 * stops the stream on its own as the configured time windows (see
	 * Settings > Stream > Scheduled Streaming Configuration) are entered
	 * and left; the manual Start Streaming button gets disabled for as
	 * long as this is on (see SetScheduleForceDisabled). Stopping it
	 * immediately tears down any stream it's currently keeping alive and
	 * hands manual control back, same as disabling it used to do from
	 * Settings before that checkbox was replaced by this button. */
	bool nowEnabled = !ScheduleEnabled();

	// The control panel button is disabled while the feature switch is
	// off (see OBSBasicControls::UpdateScheduleButtonEnabled), so this
	// should be unreachable in practice - kept as a safety net against
	// other trigger paths (e.g. a future hotkey).
	if (nowEnabled && !ScheduleFeatureEnabled())
		return;

	// Scheduled streaming takes over the manual Start/Stop Streaming
	// path (see CheckSchedule()) - if a manually-started stream is
	// already running (or connecting), refuse to hand control over to
	// the schedule rather than silently taking it over or stopping it.
	// The control panel button is disabled while a manual stream is
	// active/connecting (see OBSBasicControls::UpdateScheduleButtonEnabled),
	// so this should be unreachable in practice, same as the feature
	// switch check above.
	if (nowEnabled && (streamingStarting || (outputHandler && outputHandler->StreamingActive())))
		return;

	config_set_bool(activeConfiguration, "Schedule", "Enabled", nowEnabled);

	if (!nowEnabled)
		StopScheduledStream();

	emit ScheduleEnabledChanged(nowEnabled);
	CheckSchedule();

	activeConfiguration.SaveSafe("tmp");
}
