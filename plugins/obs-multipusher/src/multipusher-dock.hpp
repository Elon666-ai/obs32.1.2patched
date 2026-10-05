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

private:
    void buildUI();
    void buildStreamTable();
    void refreshStreamTable();
    void updatePubButton();
    void appendLog(const QString& text);

    void loadConfig();
    void applyConfigToUI();
    void readUItoConfig();

    QComboBox*    siteNameCombo_     = nullptr;
    QComboBox*    streamNameCombo_   = nullptr;
    QPushButton*  pubButton_         = nullptr;
    QSpinBox*     qualitySpin_       = nullptr;
    QTableWidget* streamTable_       = nullptr;
    QLabel*       statusLabel_       = nullptr;

    ConfigManager  config_;

    static MultipusherDock* instance_;
};
