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

#include "OBSBasicSettings.hpp"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimeEdit>
#include <QVBoxLayout>

namespace {

/* Order matches Qt::DayOfWeek (Monday = 1 .. Sunday = 7), and is the order
 * rows are laid out (two days per row, left-to-right then top-to-bottom).
 * Config key prefixes are stored in English so profile files stay portable
 * across UI languages. */
struct ScheduleDayInfo {
	const char *configKey;
	const char *localeKey;
};

constexpr ScheduleDayInfo kScheduleDays[7] = {
	{"Mon", "Basic.Settings.Schedule.Monday"},   {"Tue", "Basic.Settings.Schedule.Tuesday"},
	{"Wed", "Basic.Settings.Schedule.Wednesday"}, {"Thu", "Basic.Settings.Schedule.Thursday"},
	{"Fri", "Basic.Settings.Schedule.Friday"},   {"Sat", "Basic.Settings.Schedule.Saturday"},
	{"Sun", "Basic.Settings.Schedule.Sunday"},
};

/* Two days per row (Monday+Tuesday, Wednesday+Thursday, ...); the last row
 * only has Sunday in the left column. */
constexpr int kScheduleColumns = 2;

} // namespace

void OBSBasicSettings::InitSchedulePage()
{
	auto *hintLabel = new QLabel(QTStr("Basic.Settings.Schedule.Hint"));
	hintLabel->setWordWrap(true);

	auto *gridLayout = new QGridLayout();

	for (size_t i = 0; i < scheduleDays.size(); i++) {
		ScheduleDayRow &row = scheduleDays[i];

		row.enabled = new QCheckBox(QTStr(kScheduleDays[i].localeKey));

		row.start = new QTimeEdit();
		row.start->setDisplayFormat("HH:mm");
		row.start->setTime(QTime(9, 0));

		row.end = new QTimeEdit();
		row.end->setDisplayFormat("HH:mm");
		row.end->setTime(QTime(18, 0));

		auto *dayWidget = new QWidget();
		auto *dayLayout = new QHBoxLayout(dayWidget);
		dayLayout->setContentsMargins(0, 0, 0, 0);
		dayLayout->addWidget(row.enabled);
		dayLayout->addWidget(row.start);
		dayLayout->addWidget(new QLabel(QTStr("Basic.Settings.Schedule.To")));
		dayLayout->addWidget(row.end);
		dayLayout->addStretch(1);

		int gridRow = (int)i / kScheduleColumns;
		int gridColumn = (int)i % kScheduleColumns;
		gridLayout->addWidget(dayWidget, gridRow, gridColumn);

		HookWidget(row.enabled.data(), &QCheckBox::toggled, &OBSBasicSettings::ScheduleChanged);
		HookWidget(row.start.data(), &QTimeEdit::userTimeChanged, &OBSBasicSettings::ScheduleChanged);
		HookWidget(row.end.data(), &QTimeEdit::userTimeChanged, &OBSBasicSettings::ScheduleChanged);
	}

	auto *groupBoxLayout = qobject_cast<QVBoxLayout *>(ui->scheduleGroupBox->layout());
	groupBoxLayout->addWidget(hintLabel);
	groupBoxLayout->addLayout(gridLayout);
}

void OBSBasicSettings::LoadScheduleSettings()
{
	loading = true;

	config_t *config = main->Config();

	for (size_t i = 0; i < scheduleDays.size(); i++) {
		const ScheduleDayInfo &info = kScheduleDays[i];
		ScheduleDayRow &row = scheduleDays[i];

		std::string dayEnabledKey = std::string(info.configKey) + ".Enabled";
		std::string dayStartKey = std::string(info.configKey) + ".Start";
		std::string dayEndKey = std::string(info.configKey) + ".End";

		bool dayEnabled = config_get_bool(config, "Schedule", dayEnabledKey.c_str());

		config_set_default_int(config, "Schedule", dayStartKey.c_str(), 9 * 60);
		config_set_default_int(config, "Schedule", dayEndKey.c_str(), 18 * 60);

		int startMinutes = (int)config_get_int(config, "Schedule", dayStartKey.c_str());
		int endMinutes = (int)config_get_int(config, "Schedule", dayEndKey.c_str());

		row.enabled->setChecked(dayEnabled);
		row.start->setTime(QTime(0, 0).addSecs(startMinutes * 60));
		row.end->setTime(QTime(0, 0).addSecs(endMinutes * 60));
	}

	loading = false;
}

void OBSBasicSettings::SaveScheduleSettings()
{
	config_t *config = main->Config();

	for (size_t i = 0; i < scheduleDays.size(); i++) {
		const ScheduleDayInfo &info = kScheduleDays[i];
		ScheduleDayRow &row = scheduleDays[i];

		std::string dayEnabledKey = std::string(info.configKey) + ".Enabled";
		std::string dayStartKey = std::string(info.configKey) + ".Start";
		std::string dayEndKey = std::string(info.configKey) + ".End";

		int startMinutes = QTime(0, 0).secsTo(row.start->time()) / 60;
		int endMinutes = QTime(0, 0).secsTo(row.end->time()) / 60;

		/* End must be strictly after start within the same day; clamp
		 * rather than reject so saving never silently no-ops. */
		if (endMinutes <= startMinutes)
			endMinutes = std::min(startMinutes + 1, 24 * 60 - 1);

		config_set_bool(config, "Schedule", dayEnabledKey.c_str(), row.enabled->isChecked());
		config_set_int(config, "Schedule", dayStartKey.c_str(), startMinutes);
		config_set_int(config, "Schedule", dayEndKey.c_str(), endMinutes);
	}

	/* Whether Scheduled Streaming itself is on/off ("Schedule"/"Enabled")
	 * is no longer controlled from this page - see
	 * OBSBasic::ScheduleButtonClicked() (control panel "Start/Stop
	 * Scheduled Streaming" button). Day/time changes made here take
	 * effect immediately for a schedule that's already running, via the
	 * profileSettingChanged emit below. */
	emit main->profileSettingChanged("Schedule", "SettingsChanged");
}

void OBSBasicSettings::ScheduleChanged()
{
	if (!loading) {
		scheduleChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}
