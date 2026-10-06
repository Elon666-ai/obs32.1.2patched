#include "multipusher-dock.hpp"
#include "plugin-main.hpp"
#include "srt-auth.hpp"
#include "utils.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMainWindow>
#include <QTimer>
#include <QCheckBox>
#include <QTimeEdit>
#include <QDateTime>
#include <QLayout>
#include <array>
#include <cstddef>
#include <map>
#include <vector>

#define QSTRING(s) QString::fromStdString(s)
#define TO_STD(q)   (q).toStdString()

MultipusherDock* MultipusherDock::instance_ = nullptr;

inline MultipusherContext* ctx() { return g_ctx.get(); }

// ── OBS Frontend Dock Registration ────────────────────────────────

void MultipusherDock::Register() {
    // Defer to OBS_FRONTEND_EVENT_FINISHED_LOADING so main window is fully built.
    obs_frontend_add_event_callback(
        [](enum obs_frontend_event event, void*) {
            if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING) return;
            QMainWindow* mw = static_cast<QMainWindow*>(obs_frontend_get_main_window());
            if (!mw) return;
            auto* d = new QDockWidget(QString::fromUtf8("Multipusher v1.1.4 Amor@2026"), mw);
            d->setObjectName("obsMultipusherDock");
            d->setWidget(new MultipusherDock(d));
            mw->addDockWidget(Qt::BottomDockWidgetArea, d);
            MP_LOG(LOG_INFO, "[obs-multipusher] dock panel registered");

            // Sync OBS streaming service URL (frontend is fully ready at this point)
            if (g_ctx) g_ctx->SyncOBSServiceURL();
        }, nullptr);
    MP_LOG(LOG_INFO, "[obs-multipusher] dock registration deferred to FINISHED_LOADING");
}

MultipusherDock* MultipusherDock::Instance() {
    return instance_;
}

// ── Constructor / Destructor ─────────────────────────────────────

MultipusherDock::MultipusherDock(QWidget* parent)
    : QWidget(parent)
{
    instance_ = this;
    setMinimumSize(750, 200);
    buildUI();
    loadConfig();
    applyConfigToUI();
    refreshStreamTable();

    // Periodically refresh the table so live stream statuses (Running / Retrying
    // / Stopped, pushed in from the output threads via ladder.SetStatus) actually
    // show up. Runs on the UI thread, which is required for QTableWidget updates.
    // Also keeps the start/stop button text in sync with the actual publishing state.
    auto* statusTimer = new QTimer(this);
    connect(statusTimer, &QTimer::timeout, this, [this]() {
        refreshStreamTable();
        if (intervalMode_) updateIntervalControl();
        updateButtons();
    });
    statusTimer->start(500);

    // Auto-publish if config says so
    if (ctx() && ctx()->config.Get().publish.onReady) {
        ctx()->StartPublishing();
        appendLog("Auto-published (onReady)");
    }
}

MultipusherDock::~MultipusherDock() {
    instance_ = nullptr;
}

// ── UI Construction ──────────────────────────────────────────────

void MultipusherDock::buildUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(4);
    mainLayout->setContentsMargins(6, 6, 6, 6);

    // Site / Stream / bitrate+ / Start Pub row
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel("Site:"));
    siteNameCombo_ = new QComboBox();
    siteNameCombo_->addItems({"studio_3drush", "studio_gsp2w"});
    siteNameCombo_->setMinimumWidth(140);
    row->addWidget(siteNameCombo_);

    row->addSpacing(12);

    row->addWidget(new QLabel("Stream:"));
    streamNameCombo_ = new QComboBox();
    streamNameCombo_->setMinimumWidth(140);
    streamNameCombo_->addItems({"3drush-fwh", "3drush-fwv"});
    row->addWidget(streamNameCombo_);

    row->addSpacing(12);

    // bitrate+ bitrate boost percentage (default 20%)
    row->addWidget(new QLabel("bitrate+:"));
    qualitySpin_ = new QSpinBox();
    qualitySpin_->setRange(0, 100);
    qualitySpin_->setValue(20);
    qualitySpin_->setSuffix("%");
    qualitySpin_->setSingleStep(1);
    qualitySpin_->setMinimumWidth(150); // +50% over the default ~100px control length
    qualitySpin_->setToolTip("Boost all encoder bitrates by this percentage");
    row->addWidget(qualitySpin_);

    row->addStretch();

    // Start/Stop toggle button
    pubButton_ = new QPushButton("Start Pub");
    pubButton_->setMinimumWidth(100);
    pubButton_->setStyleSheet(
        "QPushButton { font-weight:bold; padding:4px 12px; }"
        "QPushButton:hover { background:#3a8; color:white; }");
    row->addWidget(pubButton_);

    mainLayout->addLayout(row);

    // ── Second row: interval (scheduled) publishing config ────────
    auto* intervalHeader = new QHBoxLayout();
    intervalHeader->addWidget(new QLabel("Interval:"));
    addIntervalBtn_ = new QPushButton("Add Slot");
    addIntervalBtn_->setMinimumWidth(80);
    intervalHeader->addWidget(addIntervalBtn_);
    intervalHeader->addStretch();

    intervalButton_ = new QPushButton("Start Interval Pub");
    intervalButton_->setMinimumWidth(150);
    intervalButton_->setStyleSheet(
        "QPushButton { font-weight:bold; padding:4px 12px; }"
        "QPushButton:hover { background:#38a; color:white; }");
    intervalHeader->addWidget(intervalButton_);
    mainLayout->addLayout(intervalHeader);

    intervalSlotsWidget_ = new QWidget();
    intervalSlotsLayout_ = new QVBoxLayout(intervalSlotsWidget_);
    intervalSlotsLayout_->setContentsMargins(0, 0, 0, 0);
    intervalSlotsLayout_->setSpacing(2);
    mainLayout->addWidget(intervalSlotsWidget_);

    connect(addIntervalBtn_, &QPushButton::clicked, this, [this]() {
        IntervalSlot s;
        s.startMinutes = 9 * 60;
        s.endMinutes   = 18 * 60;
        for (int d = 0; d < 5; ++d) s.days[d] = true; // Mon-Fri default
        if (ctx()) {
            ctx()->config.GetMutable().intervals.push_back(s);
            ctx()->config.Save(ctx()->config.Path());
        }
        rebuildIntervalRows();
    });

    // Use activated() — fires only on user selection, never programmatically
    connect(siteNameCombo_, QOverload<int>::of(&QComboBox::activated),
            this, &MultipusherDock::onSiteNameChanged);
    connect(streamNameCombo_, QOverload<int>::of(&QComboBox::activated),
            this, &MultipusherDock::onStreamNameChanged);
    connect(pubButton_, &QPushButton::clicked,
            this, &MultipusherDock::onPubButtonClicked);
    connect(intervalButton_, &QPushButton::clicked,
            this, &MultipusherDock::onIntervalButtonClicked);

    // Stream table
    buildStreamTable();
    mainLayout->addWidget(streamTable_, 1);
}

void MultipusherDock::buildStreamTable() {
    streamTable_ = new QTableWidget(0, 4, this);
    streamTable_->setHorizontalHeaderLabels({"Status", "Stream", "Encoding parameter", "Level"});
    streamTable_->horizontalHeader()->setStretchLastSection(true);
    streamTable_->setColumnWidth(0, 110);
    streamTable_->setColumnWidth(1, 240);
    streamTable_->setColumnWidth(2, 400);
    streamTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    streamTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    streamTable_->verticalHeader()->setVisible(false);
}

// ── Slots ────────────────────────────────────────────────────────

void MultipusherDock::onSiteNameChanged(int index) {
    QString siteName = siteNameCombo_->itemText(index);
    MP_LOG(LOG_INFO, "[obs-multipusher] site changed to: %s", siteName.toUtf8().constData());

    // Repopulate stream dropdown based on site
    streamNameCombo_->clear();
    std::string site = TO_STD(siteName);
    if (site == "studio_3drush") {
        streamNameCombo_->addItems({"3drush-fwh", "3drush-fwv"});
    } else if (site == "studio_gsp2w") {
        streamNameCombo_->addItems({"gsp2w-fwv", "gsp2w-fwh"});
    } else {
        streamNameCombo_->addItem(siteName);
    }
    streamNameCombo_->setCurrentIndex(0);

    if (ctx()) {
        ctx()->config.GetMutable().siteName = TO_STD(streamNameCombo_->currentText());
        ctx()->ladder.LoadFromConfig(ctx()->config.Get());
        ctx()->config.Save(ctx()->config.Path());
    }
    refreshStreamTable();
}

void MultipusherDock::onStreamNameChanged(int index) {
    QString streamName = streamNameCombo_->itemText(index);
    MP_LOG(LOG_INFO, "[obs-multipusher] stream changed to: %s", streamName.toUtf8().constData());
    if (ctx()) {
        auto& cfg = ctx()->config.GetMutable();
        cfg.siteName = TO_STD(streamName);
        // Auto-detect video layout from stream name suffix: fwv→portrait, fwh→landscape
        std::string name = TO_STD(streamName);
        std::string lower = Utils::ToLower(name);
        cfg.input.videoLayout = (lower.find("fwv") != std::string::npos) ? "portrait" : "landscape";
        ctx()->ladder.LoadFromConfig(cfg);
        ctx()->config.Save(ctx()->config.Path());
    }
    refreshStreamTable();
}

// ── Config ───────────────────────────────────────────────────────

void MultipusherDock::loadConfig() {
    if (!ctx()) return;
    applyConfigToUI();
}

void MultipusherDock::applyConfigToUI() {
    if (!ctx()) return;
    auto& cfg = ctx()->config.GetMutable();

    std::string siteName = StreamLadder::SiteNameForStream(cfg.siteName);

    siteNameCombo_->blockSignals(true);
    int siteIdx = siteNameCombo_->findText(QSTRING(siteName));
    if (siteIdx >= 0) siteNameCombo_->setCurrentIndex(siteIdx);
    else siteNameCombo_->setCurrentText(QSTRING(siteName));
    siteNameCombo_->blockSignals(false);

    // Repopulate stream combo to match the resolved site before setting index.
    streamNameCombo_->blockSignals(true);
    streamNameCombo_->clear();
    if (siteName == "studio_3drush")
        streamNameCombo_->addItems({"3drush-fwh", "3drush-fwv"});
    else if (siteName == "studio_gsp2w")
        streamNameCombo_->addItems({"gsp2w-fwv", "gsp2w-fwh"});
    else
        streamNameCombo_->addItem(QSTRING(cfg.siteName));
    int streamIdx = streamNameCombo_->findText(QSTRING(cfg.siteName));
    if (streamIdx >= 0) streamNameCombo_->setCurrentIndex(streamIdx);
    else streamNameCombo_->setCurrentText(QSTRING(cfg.siteName));
    streamNameCombo_->blockSignals(false);

    rebuildIntervalRows();
}

void MultipusherDock::readUItoConfig() {
    if (!ctx()) return;
    auto& cfg = ctx()->config.GetMutable();
    cfg.siteName = TO_STD(streamNameCombo_->currentText());
    // Only update the ladder for table display, don't reconfigure outputs
    ctx()->ladder.LoadFromConfig(cfg);
}

// ── Stream table ─────────────────────────────────────────────────

// Parse a bitrate string like "400k" → int kbps; apply boost percentage.
static QString formatBoostedBitrate(const std::string& bitrate, int boostPercent) {
    if (bitrate.empty()) return "?";
    std::string s = bitrate;
    bool hasK = (s.back() == 'k' || s.back() == 'K');
    if (hasK) s.pop_back();
    int kbps = 0;
    try { kbps = std::stoi(s); } catch (...) { return QString::fromStdString(bitrate); }
    kbps = static_cast<int>(kbps * (100 + boostPercent) / 100);
    return QString::number(kbps) + (hasK ? "k" : "");
}

void MultipusherDock::refreshStreamTable() {
    if (!ctx()) return;
    auto statuses = ctx()->ladder.GetStatuses();
    auto& cfg = ctx()->config.Get();

    // Build a level->StreamConfig lookup for maxrate
    std::map<std::string, StreamConfig> levelMap;
    for (auto& s : cfg.streams)
        levelMap[s.level] = s;

    int boostPercent = qualitySpin_ ? qualitySpin_->value() : 0;

    streamTable_->setRowCount(static_cast<int>(statuses.size()));
    for (int i = 0; i < static_cast<int>(statuses.size()); ++i) {
        auto& os = statuses[i];
        auto* levelItem  = new QTableWidgetItem(QSTRING(os.level));
        auto* nameItem   = new QTableWidgetItem(QSTRING(os.streamName));

        QString encParam;
        if (os.audioOnly) {
            encParam = QString("%1 %2")
                       .arg(QSTRING(os.codec), QSTRING(os.bitrate));
        } else {
            auto it = levelMap.find(os.level);
            QString maxrate = "?";
            if (it != levelMap.end() && !it->second.videoMaxrate.empty())
                maxrate = formatBoostedBitrate(it->second.videoMaxrate, boostPercent);

            encParam = QString("%1 %2 (max %3), %4")
                       .arg(QSTRING(os.codec),
                            formatBoostedBitrate(os.bitrate, boostPercent),
                            maxrate, QSTRING(os.resolution));
        }
        auto* encItem    = new QTableWidgetItem(encParam);
        auto* statusItem = new QTableWidgetItem(QSTRING(os.status));

        if (os.status == "Running")
            statusItem->setForeground(QColor(30, 145, 75));
        else if (os.status == "Starting" || os.status == "Retrying")
            statusItem->setForeground(QColor(190, 135, 25));
        else if (os.status == "Stopped")
            statusItem->setForeground(QColor(195, 65, 65));
        else if (os.status == "Exception")
            statusItem->setForeground(QColor(220, 140, 20));
        else
            statusItem->setForeground(QColor(120, 120, 120));

        streamTable_->setItem(i, 0, statusItem);
        streamTable_->setItem(i, 1, nameItem);
        streamTable_->setItem(i, 2, encItem);
        streamTable_->setItem(i, 3, levelItem);
    }
}

void MultipusherDock::appendLog(const QString& text) {
    MP_LOG(LOG_INFO, "[obs-multipusher] %s", text.toUtf8().constData());
}

// ── Start / Stop toggle ──────────────────────────────────────────

void MultipusherDock::onPubButtonClicked() {
    if (!ctx() || intervalMode_) return;

    if (ctx()->publishing) {
        MP_LOG(LOG_INFO, "[obs-multipusher] user clicked Stop");
        ctx()->StopPublishing();
    } else {
        // Apply bitrate+ percentage before starting
        ctx()->qualityBoostPercent = qualitySpin_ ? qualitySpin_->value() : 0;
        MP_LOG(LOG_INFO, "[obs-multipusher] user clicked Start (bitrate+:%d%%)",
               ctx()->qualityBoostPercent);
        ctx()->StartPublishing();
    }
    updateButtons();
}

// ── Interval (scheduled) publishing ──────────────────────────────

static const char* kIntervalDayNames[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

void MultipusherDock::rebuildIntervalRows() {
    if (!intervalSlotsLayout_ || !ctx()) return;

    loadingIntervals_ = true;

    // Tear down existing rows.
    for (auto& r : intervalRows_)
        if (r.rowWidget) r.rowWidget->deleteLater();
    intervalRows_.clear();
    while (QLayoutItem* item = intervalSlotsLayout_->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    for (auto& slot : ctx()->config.Get().intervals)
        addIntervalRow(slot);

    loadingIntervals_ = false;

    updateButtons();
}

void MultipusherDock::addIntervalRow(const IntervalSlot& slot) {
    auto* rowWidget = new QWidget(intervalSlotsWidget_);
    auto* rowLayout = new QHBoxLayout(rowWidget);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);

    auto* start = new QTimeEdit(rowWidget);
    start->setDisplayFormat("HH:mm");
    start->setTime(QTime(0, 0).addSecs(slot.startMinutes * 60));
    rowLayout->addWidget(start);

    rowLayout->addWidget(new QLabel("to", rowWidget));

    auto* end = new QTimeEdit(rowWidget);
    end->setDisplayFormat("HH:mm");
    end->setTime(QTime(0, 0).addSecs(slot.endMinutes * 60));
    rowLayout->addWidget(end);

    std::array<QCheckBox*, 7> dayChecks{};
    for (int i = 0; i < 7; ++i) {
        auto* check = new QCheckBox(kIntervalDayNames[i], rowWidget);
        check->setChecked(slot.days[i]);
        rowLayout->addWidget(check);
        dayChecks[i] = check;
        connect(check, &QCheckBox::toggled, this, [this]() { onIntervalEdited(); });
    }

    auto* remove = new QPushButton("X", rowWidget);
    remove->setFixedWidth(24);
    remove->setToolTip("Remove");
    rowLayout->addWidget(remove);
    rowLayout->addStretch(1);

    intervalSlotsLayout_->addWidget(rowWidget);
    intervalRows_.push_back({rowWidget, start, end, dayChecks, remove});

    connect(start, &QTimeEdit::userTimeChanged, this, [this]() { onIntervalEdited(); });
    connect(end, &QTimeEdit::userTimeChanged, this, [this]() { onIntervalEdited(); });
    connect(remove, &QPushButton::clicked, this, [this, rowWidget]() {
        for (size_t i = 0; i < intervalRows_.size(); ++i) {
            if (intervalRows_[i].rowWidget == rowWidget) {
                removeIntervalRow(i);
                break;
            }
        }
    });
}

void MultipusherDock::removeIntervalRow(size_t idx) {
    if (idx >= intervalRows_.size()) return;

    intervalRows_[idx].rowWidget->deleteLater();
    intervalRows_.erase(intervalRows_.begin() + static_cast<std::ptrdiff_t>(idx));
    onIntervalEdited();
}

void MultipusherDock::onIntervalEdited() {
    if (loadingIntervals_ || !ctx()) return;

    auto& cfg = ctx()->config.GetMutable();
    cfg.intervals.clear();
    for (auto& r : intervalRows_) {
        IntervalSlot s;
        s.startMinutes = QTime(0, 0).secsTo(r.start->time()) / 60;
        s.endMinutes   = QTime(0, 0).secsTo(r.end->time()) / 60;
        for (int d = 0; d < 7; ++d) s.days[d] = r.days[d]->isChecked();
        cfg.intervals.push_back(s);
    }
    ctx()->config.Save(ctx()->config.Path());
}

// Current wall-clock coverage of any configured interval, mirroring
// OBSBasic::CheckSchedule(): slots with end <= start wrap past midnight.
bool MultipusherDock::intervalsActiveNow() const {
    if (!ctx()) return false;

    const auto& ivSlots = ctx()->config.Get().intervals;
    if (ivSlots.empty()) return false;

    QDateTime now = QDateTime::currentDateTime();
    int dayIdx = now.date().dayOfWeek() - 1; // 0=Monday
    if (dayIdx < 0 || dayIdx > 6) return false;
    int prevDayIdx = (dayIdx + 6) % 7;

    int nowMinutes = now.time().hour() * 60 + now.time().minute();

    for (const auto& s : ivSlots) {
        int start = s.startMinutes;
        int end   = s.endMinutes;

        if (end > start) {
            if (s.days[dayIdx] && nowMinutes >= start && nowMinutes < end)
                return true;
            continue;
        }

        if (s.days[dayIdx] && nowMinutes >= start)
            return true;
        if (s.days[prevDayIdx] && nowMinutes < end)
            return true;
    }
    return false;
}

// Only meaningful while interval mode is armed: keep the plugin's publishing
// state in sync with whether the current time falls inside an interval.
void MultipusherDock::updateIntervalControl() {
    if (!intervalMode_ || !ctx()) return;

    bool active = intervalsActiveNow();
    if (active && !ctx()->publishing) {
        ctx()->qualityBoostPercent = qualitySpin_ ? qualitySpin_->value() : 0;
        MP_LOG(LOG_INFO, "[obs-multipusher] interval active -> starting publish");
        ctx()->StartPublishing();
    } else if (!active && ctx()->publishing) {
        MP_LOG(LOG_INFO, "[obs-multipusher] interval inactive -> stopping publish");
        ctx()->StopPublishing();
    }
}

void MultipusherDock::onIntervalButtonClicked() {
    if (!ctx()) return;

    if (!intervalMode_) {
        // Manual publishing owns the outputs; refuse to take over.
        if (ctx()->publishing) return;

        intervalMode_ = true;
        MP_LOG(LOG_INFO, "[obs-multipusher] interval mode enabled");
        updateIntervalControl();
    } else {
        intervalMode_ = false;
        if (ctx()->publishing)
            ctx()->StopPublishing();
        MP_LOG(LOG_INFO, "[obs-multipusher] interval mode disabled");
    }
    updateButtons();
}

// Keep the two mutually-exclusive publish buttons and the interval editor in
// sync with the current mode/publishing state.
void MultipusherDock::updateButtons() {
    if (!pubButton_ || !intervalButton_ || !ctx()) return;

    const QString kStartPub        = QStringLiteral("Start Pub");
    const QString kStopPub         = QStringLiteral("Stop Pub");
    const QString kStartInterval   = QStringLiteral("Start Interval Pub");
    const QString kStopInterval    = QStringLiteral("Stop Interval Pub");

    const char* kGreenPub =
        "QPushButton { font-weight:bold; padding:4px 12px; }"
        "QPushButton:hover { background:#3a8; color:white; }";
    const char* kRedPub =
        "QPushButton { font-weight:bold; padding:4px 12px; background:#c44; color:white; }"
        "QPushButton:hover { background:#e55; }";
    const char* kBlueInterval =
        "QPushButton { font-weight:bold; padding:4px 12px; }"
        "QPushButton:hover { background:#38a; color:white; }";
    const char* kRedInterval =
        "QPushButton { font-weight:bold; padding:4px 12px; background:#c44; color:white; }"
        "QPushButton:hover { background:#e55; }";

    bool pub = ctx()->publishing;

    // 0 = idle, 1 = manual publishing, 2 = interval mode.
    int state = intervalMode_ ? 2 : (pub ? 1 : 0);
    if (state == lastButtonState_) return;
    lastButtonState_ = state;

    if (intervalMode_) {
        // Interval mode owns publishing: manual Start Pub disabled, keeps
        // its "Start Pub" label (it's not the thing that stops interval mode).
        if (pubButton_->text() != kStartPub) pubButton_->setText(kStartPub);
        pubButton_->setStyleSheet(kGreenPub);
        pubButton_->setEnabled(false);

        if (intervalButton_->text() != kStopInterval) intervalButton_->setText(kStopInterval);
        intervalButton_->setStyleSheet(kRedInterval);
        intervalButton_->setEnabled(true);
    } else if (pub) {
        // Manual publishing active: interval button disabled.
        if (pubButton_->text() != kStopPub) pubButton_->setText(kStopPub);
        pubButton_->setStyleSheet(kRedPub);
        pubButton_->setEnabled(true);

        if (intervalButton_->text() != kStartInterval) intervalButton_->setText(kStartInterval);
        intervalButton_->setStyleSheet(kBlueInterval);
        intervalButton_->setEnabled(false);
    } else {
        // Idle: both available.
        if (pubButton_->text() != kStartPub) pubButton_->setText(kStartPub);
        pubButton_->setStyleSheet(kGreenPub);
        pubButton_->setEnabled(true);

        if (intervalButton_->text() != kStartInterval) intervalButton_->setText(kStartInterval);
        intervalButton_->setStyleSheet(kBlueInterval);
        intervalButton_->setEnabled(true);
    }

    // The interval editor is locked while interval mode is running.
    bool editEnabled = !intervalMode_;
    if (intervalSlotsWidget_) intervalSlotsWidget_->setEnabled(editEnabled);
    if (addIntervalBtn_) addIntervalBtn_->setEnabled(editEnabled);
    if (qualitySpin_) qualitySpin_->setEnabled(!pub && !intervalMode_);
}
