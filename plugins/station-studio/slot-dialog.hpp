#pragma once

#include "core/station-schedule.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimeEdit;

namespace station {

class SlotDialog : public QDialog {
	Q_OBJECT

public:
	SlotDialog(const Slot &slot, QWidget *parent);
	Slot Result() const { return slot_; }

	static QString KindName(SlotKind kind);
	static QString DayName(int weekday);

private:
	void UpdateKindUi();
	void Browse();
	void Accept();

	Slot slot_;
	QLineEdit *title_;
	QComboBox *kind_;
	QComboBox *target_;
	QPushButton *browse_;
	QLabel *hint_;
	QTimeEdit *start_;
	QSpinBox *duration_;
	QCheckBox *days_[7];
	QCheckBox *oneOff_;
	QDateEdit *date_;
	QCheckBox *loop_;
	QCheckBox *enabled_;
};

} // namespace station
