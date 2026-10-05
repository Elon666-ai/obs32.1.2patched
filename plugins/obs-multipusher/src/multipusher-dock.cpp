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
#include <map>

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
        updatePubButton();
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

    // Use activated() — fires only on user selection, never programmatically
    connect(siteNameCombo_, QOverload<int>::of(&QComboBox::activated),
            this, &MultipusherDock::onSiteNameChanged);
    connect(streamNameCombo_, QOverload<int>::of(&QComboBox::activated),
            this, &MultipusherDock::onStreamNameChanged);
    connect(pubButton_, &QPushButton::clicked,
            this, &MultipusherDock::onPubButtonClicked);

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
    if (!ctx()) return;

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
}

void MultipusherDock::updatePubButton() {
    if (!pubButton_ || !ctx()) return;

    bool pub = ctx()->publishing;
    if (pub && pubButton_->text() != QStringLiteral("Stop Pub")) {
        pubButton_->setText(QStringLiteral("Stop Pub"));
        pubButton_->setStyleSheet(
            "QPushButton { font-weight:bold; padding:4px 12px; background:#c44; color:white; }"
            "QPushButton:hover { background:#e55; }");
        // Disable bitrate+ while publishing
        if (qualitySpin_) qualitySpin_->setEnabled(false);
    } else if (!pub && pubButton_->text() != QStringLiteral("Start Pub")) {
        pubButton_->setText(QStringLiteral("Start Pub"));
        pubButton_->setStyleSheet(
            "QPushButton { font-weight:bold; padding:4px 12px; }"
            "QPushButton:hover { background:#3a8; color:white; }");
        // Re-enable bitrate+ when stopped
        if (qualitySpin_) qualitySpin_->setEnabled(true);
    }
}
