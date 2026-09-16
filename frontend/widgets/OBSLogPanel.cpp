#include "OBSLogPanel.hpp"

#include <OBSApp.hpp>

#include <QVBoxLayout>
#include <QScrollBar>

#include "moc_OBSLogPanel.cpp"

OBSLogPanel::OBSLogPanel(QWidget *parent) : QWidget(parent)
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	textEdit = new QPlainTextEdit(this);
	textEdit->setReadOnly(true);
	textEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
	textEdit->setMaximumBlockCount(2000);
	textEdit->setStyleSheet(
		"QPlainTextEdit { background-color: #1e1e1e; color: #d4d4d4; font-family: Consolas, "
		"'Courier New', monospace; font-size: 11px; }");

	QFont monoFont("Consolas");
	monoFont.setStyleHint(QFont::Monospace);
	monoFont.setPointSize(9);
	textEdit->setFont(monoFont);

	layout->addWidget(textEdit);

	connect(App(), &OBSApp::logLineAdded, this, &OBSLogPanel::AddLine);
}

void OBSLogPanel::AddLine(int logLevel, const QString &msg)
{
	QColor color;
	switch (logLevel) {
	case LOG_ERROR:
		color = QColor("#f44747");
		break;
	case LOG_WARNING:
		color = QColor("#dcdcaa");
		break;
	case LOG_INFO:
		color = QColor("#d4d4d4");
		break;
	default:
		color = QColor("#808080");
		break;
	}

	QTextCursor cursor = textEdit->textCursor();
	cursor.movePosition(QTextCursor::End);

	QTextCharFormat fmt;
	fmt.setForeground(color);
	cursor.insertText(msg + "\n", fmt);

	textEdit->setTextCursor(cursor);

	QScrollBar *scroll = textEdit->verticalScrollBar();
	scroll->setValue(scroll->maximum());
}
