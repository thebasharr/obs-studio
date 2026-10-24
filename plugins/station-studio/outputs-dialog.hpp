#pragma once

#include "outputs-manager.hpp"
#include "station-state.hpp"

#include <QDialog>

#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace station {

class OutputsDialog : public QDialog {
	Q_OBJECT

public:
	OutputsDialog(StationSettings &settings, OutputsManager &outputs, std::function<void()> persist,
		      QWidget *parent);

private:
	struct Row {
		OutputsManager::Target target;
		QLabel *status;
		QPushButton *toggle;
	};

	void ApplyFields();
	void Toggle(Row &row);
	void Refresh();

	StationSettings &settings_;
	OutputsManager &outputs_;
	std::function<void()> persist_;

	QSpinBox *videoKbps_;
	QSpinBox *audioKbps_;
	QLineEdit *receiverUrl_;
	QLineEdit *hlsFolder_;
	QSpinBox *hlsPort_;
	QLineEdit *playerUrl_;
	QLineEdit *ytServer_;
	QLineEdit *ytKey_;
	QLineEdit *fbServer_;
	QLineEdit *fbKey_;
	Row rows_[4];
};

} // namespace station
