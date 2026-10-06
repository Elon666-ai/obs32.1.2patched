#pragma once

#include "config-manager.hpp"
#include "stream-ladder.hpp"
#include "status-reporter.hpp"
#include "player-link.hpp"

#include <QWidget>
#include <QDockWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QTableWidget>
#include <QLabel>
#include <QPushButton>
#include <QTimeEdit>
#include <QCheckBox>
#include <QVBoxLayout>

#include <array>

#include <obs-frontend-api.h>

/// Panel widget: site dropdown + stream dropdown + start/stop button + stream table.
class MultipusherDock : public QWidget {
    Q_OBJECT

public:
    explicit MultipusherDock(QWidget* parent = nullptr);
    ~MultipusherDock() override;

    static void Register();
    static MultipusherDock* Instance();

private slots:
    void onSiteNameChanged(int index);
    void onStreamNameChanged(int index);
    void onPubButtonClicked();
    void onIntervalButtonClicked();

private:
    void buildUI();
    void buildStreamTable();
    void refreshStreamTable();
    void updateButtons();
    void appendLog(const QString& text);

    void loadConfig();
    void applyConfigToUI();
    void readUItoConfig();

    // ── Interval (scheduled) publishing ──────────────────────────
    void buildIntervalRow(QHBoxLayout* row);
    void loadIntervalToUI();
    void onIntervalEdited();
    bool intervalsActiveNow() const;
    void updateIntervalControl();

    QComboBox*    siteNameCombo_     = nullptr;
    QComboBox*    streamNameCombo_   = nullptr;
    QPushButton*  pubButton_         = nullptr;
    QSpinBox*     qualitySpin_       = nullptr;
    QTableWidget* streamTable_       = nullptr;
    QLabel*       statusLabel_       = nullptr;

    QPushButton*  intervalButton_   = nullptr;
    QTimeEdit*    intervalStart_    = nullptr;
    QTimeEdit*    intervalEnd_      = nullptr;
    std::array<QCheckBox*, 7> intervalDays_{};
    bool          intervalMode_     = false;
    bool          loadingIntervals_ = false;
    int           lastButtonState_  = -1;

    ConfigManager  config_;

    static MultipusherDock* instance_;
};
