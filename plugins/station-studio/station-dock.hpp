#pragma once

#include "core/scheduler-engine.hpp"
#include "outputs-manager.hpp"
#include "playout-controller.hpp"
#include "station-state.hpp"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;
class QTimer;

namespace station {

class StationDock : public QWidget {
	Q_OBJECT

public:
	explicit StationDock(QWidget *parent = nullptr);
	~StationDock() override;

	void OnFinishedLoading();
	void OnCollectionChanging();
	void OnCollectionChanged();
	void OnExit();

	void OpenOutputsDialog();

private:
	void BuildUi();
	void Tick();
	void UpdateStatus();
	void RefreshScheduleViews();
	void RefreshDayView();
	void RefreshTakeList();
	void Persist();

	void AddSlot(int weekday, int hour, bool oneOffOnSelectedDay);
	void EditSlot(const std::string &id);
	void DeleteSlot(const std::string &id);
	void TakeManual();
	void ResumeAutomatic();
	void SetAutomation(bool on);

	int64_t Now() const;

	Schedule schedule_;
	StationSettings settings_;
	SchedulerEngine engine_;
	PlayoutController playout_;
	OutputsManager outputs_;
	QString onAirTitle_;
	QString lastError_;
	bool loaded_ = false;
	bool suspended_ = false;

	QTimer *timer_;
	QLabel *onAir_;
	QLabel *next_;
	QLabel *mode_;
	QLabel *warning_;
	QPushButton *automation_;
	QComboBox *takeList_;
	QPushButton *take_;
	QPushButton *resume_;
	QTableWidget *week_;
	QListWidget *slotList_;
	QDateEdit *day_;
	QListWidget *dayList_;
	QComboBox *fallback_;
	QCheckBox *releaseManual_;
	QCheckBox *radioMonitoring_;
};

} // namespace station
