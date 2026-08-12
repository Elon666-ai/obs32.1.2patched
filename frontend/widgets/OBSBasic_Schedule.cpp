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

/* Qt::DayOfWeek is 1 (Monday) .. 7 (Sunday), matching the order the config
 * keys are written in by OBSBasicSettings_Schedule.cpp. */
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

	CheckSchedule();
}

bool OBSBasic::ScheduleEnabled() const
{
	if (!activeConfiguration)
		return false;
	return config_get_bool(activeConfiguration, "Schedule", "Enabled");
}

void OBSBasic::ReloadSchedule()
{
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

	std::string dayKey = kScheduleDayKeys[dayIdx];
	bool dayEnabled = config_get_bool(activeConfiguration, "Schedule", (dayKey + ".Enabled").c_str());

	bool shouldBeStreaming = false;
	if (dayEnabled) {
		int startMinutes = (int)config_get_int(activeConfiguration, "Schedule", (dayKey + ".Start").c_str());
		int endMinutes = (int)config_get_int(activeConfiguration, "Schedule", (dayKey + ".End").c_str());
		int nowMinutes = now.time().hour() * 60 + now.time().minute();

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
	/* Called from OBSBasicSettings::SaveScheduleSettings() right after it
	 * writes "Schedule"/"Enabled"=false to the same config_t that
	 * activeConfiguration/Config() point at, so ScheduleEnabled() already
	 * reads false by the time this runs. Manual control is restored
	 * separately by the ScheduleEnabledChanged(false) emit that
	 * ReloadSchedule() sends right after this (via profileSettingChanged). */
	scheduleStreamActive = false;

	if (outputHandler && outputHandler->StreamingActive() && !streamingStopping) {
		blog(LOG_INFO, "Stopping stream: scheduled streaming was disabled");
		StopStreaming();
	}
}
