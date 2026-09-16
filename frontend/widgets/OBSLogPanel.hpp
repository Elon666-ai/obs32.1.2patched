#pragma once

#include <QWidget>
#include <QPlainTextEdit>

class OBSLogPanel : public QWidget {
	Q_OBJECT

	QPlainTextEdit *textEdit;

public:
	OBSLogPanel(QWidget *parent = nullptr);

public slots:
	void AddLine(int logLevel, const QString &msg);
};
