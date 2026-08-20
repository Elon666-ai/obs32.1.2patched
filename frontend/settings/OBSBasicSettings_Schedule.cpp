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
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimeEdit>
#include <QVBoxLayout>

namespace {

/* Qt::DayOfWeek is 1 (Monday) .. 7 (Sunday); index here is that minus 1.
 * Config key prefixes are stored in English so profile files stay portable
 * across UI languages. */
constexpr const char *kDayConfigKeys[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
constexpr const char *kDayLocaleKeys[7] = {
	"Basic.Settings.Schedule.Monday", "Basic.Settings.Schedule.Tuesday",  "Basic.Settings.Schedule.Wednesday",
	"Basic.Settings.Schedule.Thursday", "Basic.Settings.Schedule.Friday", "Basic.Settings.Schedule.Saturday",
	"Basic.Settings.Schedule.Sunday",
};

// A slot's [start, end) window, expanded onto the specific weekday(s) it
// actually falls on. Slots with end <= start wrap past midnight (e.g.
// 22:00-06:00) - the portion up to midnight lands on the checked day
// itself, while the portion from midnight to End lands on the *next*
// calendar day, same as OBSBasic::CheckSchedule() evaluates it. A slot
// with start == end wraps to the full 24h day (no gap at all), which falls
// out of this naturally rather than needing special-casing.
struct DayRange {
	int day; // 0=Monday..6=Sunday
	int start;
	int end; // > start, same day (never wraps)
};

std::vector<DayRange> ExpandSlotRanges(int startMinutes, int endMinutes, const std::array<bool, 7> &days)
{
	std::vector<DayRange> ranges;
	bool wraps = endMinutes <= startMinutes;
	for (int d = 0; d < 7; d++) {
		if (!days[d])
			continue;
		if (!wraps) {
			ranges.push_back({d, startMinutes, endMinutes});
		} else {
			ranges.push_back({d, startMinutes, 24 * 60});
			ranges.push_back({(d + 1) % 7, 0, endMinutes});
		}
	}
	return ranges;
}

} // namespace

void OBSBasicSettings::InitSchedulePage()
{
	auto *hintLabel = new QLabel(QTStr("Basic.Settings.Schedule.Hint"));
	hintLabel->setWordWrap(true);

	scheduleOverlapWarning = new QLabel();
	scheduleOverlapWarning->setProperty("class", "text-danger");
	scheduleOverlapWarning->setWordWrap(true);
	scheduleOverlapWarning->setVisible(false);

	auto *addButton = new QPushButton(QTStr("Basic.Settings.Schedule.AddSlot"));
	addButton->setProperty("class", "icon-plus");
	connect(addButton, &QPushButton::clicked, this,
		[this]() { AddScheduleSlotRow(9 * 60, 18 * 60, {true, true, true, true, true, false, false}); });

	auto *groupBoxLayout = qobject_cast<QVBoxLayout *>(ui->scheduleGroupBox->layout());
	groupBoxLayout->addWidget(hintLabel);
	groupBoxLayout->addWidget(scheduleOverlapWarning);
	groupBoxLayout->addWidget(addButton);

	// scheduleGroupBox is checkable (see the .ui file) and acts as the
	// master switch for the whole feature - unchecking it disables all
	// child widgets automatically (native QGroupBox behavior) and, once
	// saved, also disables the control panel's "Start/Stop Scheduled
	// Streaming" button (see OBSBasicControls::UpdateScheduleButtonEnabled)
	// and stops any schedule currently running (see
	// OBSBasic::RefreshScheduleFeatureState).
	HookWidget(ui->scheduleGroupBox, &QGroupBox::toggled, &OBSBasicSettings::ScheduleChanged);
}

// Builds one row: start/end QTimeEdit, seven weekday QCheckBoxes, a remove
// button. Rows are stored/added in scheduleSlots in display order; row
// index is recomputed on demand (via a lambda capturing the row widget)
// rather than cached, since RemoveScheduleSlotRow() shifts later rows down.
void OBSBasicSettings::AddScheduleSlotRow(int startMinutes, int endMinutes, const std::array<bool, 7> &days)
{
	auto *rowWidget = new QWidget(ui->scheduleGroupBox);
	auto *rowLayout = new QHBoxLayout(rowWidget);
	rowLayout->setContentsMargins(0, 0, 0, 0);

	auto *start = new QTimeEdit(rowWidget);
	start->setDisplayFormat("HH:mm");
	start->setTime(QTime(0, 0).addSecs(startMinutes * 60));
	rowLayout->addWidget(start);

	rowLayout->addWidget(new QLabel(QTStr("Basic.Settings.Schedule.To"), rowWidget));

	auto *end = new QTimeEdit(rowWidget);
	end->setDisplayFormat("HH:mm");
	end->setTime(QTime(0, 0).addSecs(endMinutes * 60));
	rowLayout->addWidget(end);

	std::array<QCheckBox *, 7> dayChecks{};
	for (size_t i = 0; i < dayChecks.size(); i++) {
		auto *check = new QCheckBox(QTStr(kDayLocaleKeys[i]), rowWidget);
		check->setChecked(days[i]);
		rowLayout->addWidget(check);
		dayChecks[i] = check;

		HookWidget(check, &QCheckBox::toggled, &OBSBasicSettings::ScheduleChanged);
		connect(check, &QCheckBox::toggled, this, [this]() { ValidateScheduleSlots(); });
	}

	auto *removeButton = new QPushButton(rowWidget);
	removeButton->setProperty("class", "icon-trash");
	removeButton->setToolTip(QTStr("Remove"));
	rowLayout->addWidget(removeButton);

	rowLayout->addStretch(1);

	// Inserted right before the "Add Slot" button, which is always the
	// last item in the groupbox layout.
	auto *groupBoxLayout = qobject_cast<QVBoxLayout *>(ui->scheduleGroupBox->layout());
	groupBoxLayout->insertWidget(groupBoxLayout->count() - 1, rowWidget);

	scheduleSlots.push_back({rowWidget, start, end, dayChecks, removeButton});

	HookWidget(start, &QTimeEdit::userTimeChanged, &OBSBasicSettings::ScheduleChanged);
	HookWidget(end, &QTimeEdit::userTimeChanged, &OBSBasicSettings::ScheduleChanged);
	connect(start, &QTimeEdit::userTimeChanged, this, [this]() { ValidateScheduleSlots(); });
	connect(end, &QTimeEdit::userTimeChanged, this, [this]() { ValidateScheduleSlots(); });

	connect(removeButton, &QPushButton::clicked, this, [this, rowWidget]() {
		for (size_t i = 0; i < scheduleSlots.size(); i++) {
			if (scheduleSlots[i].rowWidget == rowWidget) {
				RemoveScheduleSlotRow(i);
				break;
			}
		}
	});

	if (!loading) {
		scheduleChanged = true;
		EnableApplyButton(true);
	}

	ValidateScheduleSlots();
}

void OBSBasicSettings::RemoveScheduleSlotRow(size_t idx)
{
	if (idx >= scheduleSlots.size())
		return;

	// deleteLater(), not delete: this runs from the row's own remove
	// button's clicked() handler, so the button (and its enclosing
	// rowWidget) is still on the call stack - destroying it synchronously
	// here would be a use-after-free once that handler returns.
	scheduleSlots[idx].rowWidget->deleteLater();
	scheduleSlots.erase(scheduleSlots.begin() + (ptrdiff_t)idx);

	if (!loading) {
		scheduleChanged = true;
		EnableApplyButton(true);
	}

	ValidateScheduleSlots();
}

// Highlights (via the "text-danger" class, same convention as
// advOutRecWarning/simpleOutRecWarning elsewhere in Settings) any pair of
// slots that end up covering the same moment on the same calendar day once
// expanded via ExpandSlotRanges() (which accounts for slots that wrap past
// midnight landing part of their window on the *next* day), and shows/hides
// scheduleOverlapWarning accordingly. Returns false if any overlap was
// found - QueryAllowedToClose() uses this to block Apply/OK, the same way
// it already blocks on invalid encoder selections.
bool OBSBasicSettings::ValidateScheduleSlots()
{
	bool anyOverlap = false;

	for (auto &slot : scheduleSlots)
		slot.rowWidget->setProperty("class", "");

	std::vector<std::vector<DayRange>> allRanges(scheduleSlots.size());
	for (size_t i = 0; i < scheduleSlots.size(); i++) {
		std::array<bool, 7> days{};
		for (size_t d = 0; d < 7; d++)
			days[d] = scheduleSlots[i].days[d]->isChecked();

		int startI = QTime(0, 0).secsTo(scheduleSlots[i].start->time()) / 60;
		int endI = QTime(0, 0).secsTo(scheduleSlots[i].end->time()) / 60;
		allRanges[i] = ExpandSlotRanges(startI, endI, days);
	}

	for (size_t i = 0; i < scheduleSlots.size(); i++) {
		for (size_t j = i + 1; j < scheduleSlots.size(); j++) {
			bool overlap = false;
			for (const auto &ri : allRanges[i]) {
				for (const auto &rj : allRanges[j]) {
					if (ri.day == rj.day && ri.start < rj.end && rj.start < ri.end) {
						overlap = true;
						break;
					}
				}
				if (overlap)
					break;
			}
			if (overlap) {
				anyOverlap = true;
				scheduleSlots[i].rowWidget->setProperty("class", "text-danger");
				scheduleSlots[j].rowWidget->setProperty("class", "text-danger");
			}
		}
	}

	for (auto &slot : scheduleSlots) {
		slot.rowWidget->style()->unpolish(slot.rowWidget);
		slot.rowWidget->style()->polish(slot.rowWidget);
	}

	scheduleOverlapWarning->setText(anyOverlap ? QTStr("Basic.Settings.Schedule.OverlapWarning") : QString());
	scheduleOverlapWarning->setVisible(anyOverlap);

	return !anyOverlap;
}

void OBSBasicSettings::LoadScheduleSettings()
{
	loading = true;

	config_t *config = main->Config();

	ui->scheduleGroupBox->setChecked(config_get_bool(config, "Schedule", "FeatureEnabled"));

	for (auto &slot : scheduleSlots)
		delete slot.rowWidget;
	scheduleSlots.clear();

	int slotCount = (int)config_get_int(config, "Schedule", "Slot.Count");
	for (int i = 0; i < slotCount; i++) {
		std::string prefix = "Slot" + std::to_string(i) + ".";

		int startMinutes = (int)config_get_int(config, "Schedule", (prefix + "Start").c_str());
		int endMinutes = (int)config_get_int(config, "Schedule", (prefix + "End").c_str());

		std::array<bool, 7> days{};
		for (size_t d = 0; d < days.size(); d++) {
			std::string dayKey = prefix + "Day." + kDayConfigKeys[d];
			days[d] = config_get_bool(config, "Schedule", dayKey.c_str());
		}

		AddScheduleSlotRow(startMinutes, endMinutes, days);
	}

	ValidateScheduleSlots();

	loading = false;
}

void OBSBasicSettings::SaveScheduleSettings()
{
	config_t *config = main->Config();

	config_set_bool(config, "Schedule", "FeatureEnabled", ui->scheduleGroupBox->isChecked());

	// Clear all previously-saved slots first (old slot count may be
	// larger than the current one) so a shrink doesn't leave stale
	// Slot<N>.* keys around for CheckSchedule() to still read.
	int oldSlotCount = (int)config_get_int(config, "Schedule", "Slot.Count");
	for (int i = 0; i < oldSlotCount; i++) {
		std::string prefix = "Slot" + std::to_string(i) + ".";
		config_remove_value(config, "Schedule", (prefix + "Start").c_str());
		config_remove_value(config, "Schedule", (prefix + "End").c_str());
		for (size_t d = 0; d < 7; d++)
			config_remove_value(config, "Schedule", (prefix + "Day." + kDayConfigKeys[d]).c_str());
	}

	config_set_int(config, "Schedule", "Slot.Count", (int)scheduleSlots.size());

	for (size_t i = 0; i < scheduleSlots.size(); i++) {
		const auto &slot = scheduleSlots[i];
		std::string prefix = "Slot" + std::to_string(i) + ".";

		int startMinutes = QTime(0, 0).secsTo(slot.start->time()) / 60;
		int endMinutes = QTime(0, 0).secsTo(slot.end->time()) / 60;

		// end <= start means the slot wraps past midnight into the next
		// day (e.g. 22:00-06:00) - see ExpandSlotRanges() above and
		// OBSBasic::CheckSchedule(), both of which handle that case
		// directly rather than needing it normalized away here.
		config_set_int(config, "Schedule", (prefix + "Start").c_str(), startMinutes);
		config_set_int(config, "Schedule", (prefix + "End").c_str(), endMinutes);
		for (size_t d = 0; d < 7; d++)
			config_set_bool(config, "Schedule", (prefix + "Day." + kDayConfigKeys[d]).c_str(),
					slot.days[d]->isChecked());
	}

	/* Whether a schedule is currently *running* ("Schedule"/"Enabled") is
	 * controlled from the control panel's "Start/Stop Scheduled
	 * Streaming" button (see OBSBasic::ScheduleButtonClicked()), not from
	 * here - this page only controls whether the feature can be used at
	 * all ("Schedule"/"FeatureEnabled", set above) and the configured
	 * time slots. Both slot changes and a feature-switch flip made here
	 * take effect immediately (including stopping an already-running
	 * schedule if the feature switch was just turned off), via the
	 * profileSettingChanged emit below - see
	 * OBSBasic::RefreshScheduleFeatureState(). */
	emit main->profileSettingChanged("Schedule", "SettingsChanged");
}

void OBSBasicSettings::ScheduleChanged()
{
	if (!loading) {
		scheduleChanged = true;
		if (sender())
			sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}
