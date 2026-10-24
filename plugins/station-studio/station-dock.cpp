#include "station-dock.hpp"
#include "outputs-dialog.hpp"
#include "slot-dialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace station {

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static const int kDisplayOrder[7] = {6, 0, 1, 2, 3, 4, 5};

static QString Hm(int minuteOfDay)
{
	minuteOfDay = ((minuteOfDay % kMinutesPerDay) + kMinutesPerDay) % kMinutesPerDay;
	return QString("%1:%2").arg(minuteOfDay / 60, 2, 10, QChar('0')).arg(minuteOfDay % 60, 2, 10, QChar('0'));
}

static QString HmFromSeconds(int64_t localSeconds)
{
	return Hm((int)((localSeconds % kSecondsPerDay) / 60));
}

static QString Describe(const Slot &s)
{
	QString when = Hm(s.startMinute) + QString::fromUtf8(" \u2013 ") + Hm(s.startMinute + s.durationMinutes);
	QString days;
	if (s.date) {
		days = QString::fromStdString(FormatDate(*s.date));
	} else if (s.weekdays == kEveryDay) {
		days = T("Days.Every");
	} else {
		QStringList names;
		for (int wd : kDisplayOrder)
			if (s.weekdays & (1u << wd))
				names << SlotDialog::DayName(wd);
		days = names.join(QString::fromUtf8("\u060C "));
	}
	QString text = QString("%1   %2   [%3 \u2022 %4]")
			       .arg(when, QString::fromStdString(s.title), SlotDialog::KindName(s.kind), days);
	if (!s.enabled)
		text += "   " + T("Slot.Disabled");
	return text;
}

StationDock::StationDock(QWidget *parent) : QWidget(parent)
{
	LoadState(schedule_, settings_);
	engine_.SetEnabled(settings_.automationEnabled);
	engine_.SetReleaseManualAtNextSlot(settings_.releaseManualAtNextSlot);
	playout_.SetRadioMonitoring(settings_.radioMonitoring);

	BuildUi();
	RefreshScheduleViews();

	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, &StationDock::Tick);
	timer_->start(1000);
}

StationDock::~StationDock() = default;

int64_t StationDock::Now() const
{
	const QDateTime now = QDateTime::currentDateTime();
	const QDate d = now.date();
	return ToLocalSeconds(CivilDate{d.year(), d.month(), d.day()}, now.time().msecsSinceStartOfDay() / 1000);
}

void StationDock::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);

	onAir_ = new QLabel;
	onAir_->setStyleSheet("font-size: 15px; font-weight: bold;");
	onAir_->setWordWrap(true);
	next_ = new QLabel;
	next_->setWordWrap(true);
	mode_ = new QLabel;
	warning_ = new QLabel;
	warning_->setWordWrap(true);
	warning_->setStyleSheet("color: #c0392b;");
	warning_->hide();
	root->addWidget(onAir_);
	root->addWidget(next_);
	root->addWidget(mode_);
	root->addWidget(warning_);

	automation_ = new QPushButton;
	automation_->setCheckable(true);
	automation_->setChecked(settings_.automationEnabled);
	root->addWidget(automation_);
	connect(automation_, &QPushButton::toggled, this, &StationDock::SetAutomation);

	auto *takeRow = new QHBoxLayout;
	takeList_ = new QComboBox;
	take_ = new QPushButton(T("Dock.Take"));
	takeRow->addWidget(takeList_, 1);
	takeRow->addWidget(take_);
	root->addLayout(takeRow);
	resume_ = new QPushButton(T("Dock.Resume"));
	root->addWidget(resume_);
	connect(take_, &QPushButton::clicked, this, &StationDock::TakeManual);
	connect(resume_, &QPushButton::clicked, this, &StationDock::ResumeAutomatic);

	auto *tabs = new QTabWidget;
	root->addWidget(tabs, 1);

	/* Week grid + full list */
	auto *weekPage = new QWidget;
	auto *weekLayout = new QVBoxLayout(weekPage);
	week_ = new QTableWidget(24, 7);
	week_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	week_->setSelectionMode(QAbstractItemView::SingleSelection);
	week_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	week_->verticalHeader()->setDefaultSectionSize(22);
	QStringList dayHeaders, hourHeaders;
	for (int wd : kDisplayOrder)
		dayHeaders << SlotDialog::DayName(wd);
	for (int h = 0; h < 24; h++)
		hourHeaders << Hm(h * 60);
	week_->setHorizontalHeaderLabels(dayHeaders);
	week_->setVerticalHeaderLabels(hourHeaders);
	week_->setToolTip(T("Dock.WeekTip"));
	weekLayout->addWidget(week_, 3);

	slotList_ = new QListWidget;
	weekLayout->addWidget(slotList_, 2);
	auto *listButtons = new QHBoxLayout;
	auto *add = new QPushButton(T("Dock.Add"));
	auto *edit = new QPushButton(T("Dock.Edit"));
	auto *del = new QPushButton(T("Dock.Delete"));
	listButtons->addWidget(add);
	listButtons->addWidget(edit);
	listButtons->addWidget(del);
	weekLayout->addLayout(listButtons);
	tabs->addTab(weekPage, T("Tab.Week"));

	connect(week_, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
		const QStringList ids = week_->item(row, col) ? week_->item(row, col)->data(Qt::UserRole).toStringList()
							      : QStringList();
		if (ids.isEmpty()) {
			AddSlot(kDisplayOrder[col], row, false);
			return;
		}
		if (ids.size() == 1) {
			EditSlot(ids.first().toStdString());
			return;
		}
		QMenu menu(this);
		for (const QString &id : ids) {
			const Slot *s = schedule_.Find(id.toStdString());
			if (s)
				menu.addAction(QString::fromStdString(s->title), this,
					       [this, id]() { EditSlot(id.toStdString()); });
		}
		menu.exec(QCursor::pos());
	});
	auto selectedId = [this]() {
		QListWidgetItem *item = slotList_->currentItem();
		return item ? item->data(Qt::UserRole).toString().toStdString() : std::string();
	};
	connect(add, &QPushButton::clicked, this, [this]() { AddSlot(-1, QTime::currentTime().hour(), false); });
	connect(edit, &QPushButton::clicked, this, [this, selectedId]() {
		const std::string id = selectedId();
		if (!id.empty())
			EditSlot(id);
	});
	connect(del, &QPushButton::clicked, this, [this, selectedId]() {
		const std::string id = selectedId();
		if (!id.empty())
			DeleteSlot(id);
	});
	connect(slotList_, &QListWidget::itemDoubleClicked, this,
		[this](QListWidgetItem *item) { EditSlot(item->data(Qt::UserRole).toString().toStdString()); });

	/* Single day (shows recurring + one-off airings) */
	auto *dayPage = new QWidget;
	auto *dayLayout = new QVBoxLayout(dayPage);
	day_ = new QDateEdit(QDate::currentDate());
	day_->setCalendarPopup(true);
	day_->setDisplayFormat("dddd yyyy-MM-dd");
	dayLayout->addWidget(day_);
	dayList_ = new QListWidget;
	dayLayout->addWidget(dayList_, 1);
	auto *addOneOff = new QPushButton(T("Dock.AddOneOff"));
	dayLayout->addWidget(addOneOff);
	tabs->addTab(dayPage, T("Tab.Day"));
	connect(day_, &QDateEdit::dateChanged, this, &StationDock::RefreshDayView);
	connect(addOneOff, &QPushButton::clicked, this,
		[this]() { AddSlot(-1, QTime::currentTime().hour(), true); });
	connect(dayList_, &QListWidget::itemDoubleClicked, this,
		[this](QListWidgetItem *item) { EditSlot(item->data(Qt::UserRole).toString().toStdString()); });

	/* Settings */
	auto *settingsPage = new QWidget;
	auto *settingsLayout = new QVBoxLayout(settingsPage);
	settingsLayout->addWidget(new QLabel(T("Settings.Fallback")));
	fallback_ = new QComboBox;
	settingsLayout->addWidget(fallback_);
	auto *fallbackHint = new QLabel(T("Settings.FallbackHint"));
	fallbackHint->setWordWrap(true);
	settingsLayout->addWidget(fallbackHint);
	releaseManual_ = new QCheckBox(T("Settings.ReleaseManual"));
	releaseManual_->setChecked(settings_.releaseManualAtNextSlot);
	settingsLayout->addWidget(releaseManual_);
	radioMonitoring_ = new QCheckBox(T("Settings.RadioMonitoring"));
	radioMonitoring_->setChecked(settings_.radioMonitoring);
	settingsLayout->addWidget(radioMonitoring_);
	auto *outputsButton = new QPushButton(T("Menu.Outputs"));
	settingsLayout->addWidget(outputsButton);
	settingsLayout->addStretch();
	tabs->addTab(settingsPage, T("Tab.Settings"));

	connect(fallback_, &QComboBox::activated, this, [this]() {
		settings_.fallbackScene = fallback_->currentData().toString().toStdString();
		Persist();
	});
	connect(releaseManual_, &QCheckBox::toggled, this, [this](bool on) {
		settings_.releaseManualAtNextSlot = on;
		engine_.SetReleaseManualAtNextSlot(on);
		Persist();
	});
	connect(radioMonitoring_, &QCheckBox::toggled, this, [this](bool on) {
		settings_.radioMonitoring = on;
		playout_.SetRadioMonitoring(on);
		Persist();
	});
	connect(outputsButton, &QPushButton::clicked, this, &StationDock::OpenOutputsDialog);

	UpdateStatus();
}

void StationDock::Persist()
{
	if (!SaveState(schedule_, settings_))
		blog(LOG_WARNING, "[station-studio] Failed to save schedule");
}

void StationDock::RefreshTakeList()
{
	const QString takeCurrent = takeList_->currentText();
	takeList_->clear();
	fallback_->clear();
	fallback_->addItem(T("Settings.NoFallback"), QString());

	char **names = obs_frontend_get_scene_names();
	for (char **n = names; n && *n; n++) {
		if (PlayoutController::IsManagedScene(*n))
			continue;
		const QString name = QString::fromUtf8(*n);
		takeList_->addItem(name);
		fallback_->addItem(name, name);
	}
	bfree(names);

	takeList_->setCurrentText(takeCurrent);
	const int fb = fallback_->findData(QString::fromStdString(settings_.fallbackScene));
	fallback_->setCurrentIndex(fb >= 0 ? fb : 0);
}

void StationDock::RefreshScheduleViews()
{
	week_->clearContents();
	for (const Slot &s : schedule_.slots) {
		if (s.date)
			continue;
		for (int col = 0; col < 7; col++) {
			const int wd = kDisplayOrder[col];
			if (!(s.weekdays & (1u << wd)))
				continue;
			const int row = s.startMinute / 60;
			QTableWidgetItem *item = week_->item(row, col);
			if (!item) {
				item = new QTableWidgetItem;
				week_->setItem(row, col, item);
			}
			QStringList ids = item->data(Qt::UserRole).toStringList();
			ids << QString::fromStdString(s.id);
			item->setData(Qt::UserRole, ids);
			const QString line = Hm(s.startMinute) + " " + QString::fromStdString(s.title);
			item->setText(item->text().isEmpty() ? line : item->text() + "\n" + line);
			item->setToolTip(item->text());
			item->setBackground(s.enabled ? QColor(52, 120, 200, 70) : QColor(128, 128, 128, 50));
		}
	}

	std::vector<const Slot *> sorted;
	for (const Slot &s : schedule_.slots)
		sorted.push_back(&s);
	std::sort(sorted.begin(), sorted.end(), [](const Slot *a, const Slot *b) {
		if (a->date.has_value() != b->date.has_value())
			return !a->date.has_value();
		return a->startMinute < b->startMinute;
	});
	slotList_->clear();
	for (const Slot *s : sorted) {
		auto *item = new QListWidgetItem(Describe(*s));
		item->setData(Qt::UserRole, QString::fromStdString(s->id));
		slotList_->addItem(item);
	}

	int conflicts = 0;
	for (const Overlap &o : schedule_.FindOverlaps())
		if (!o.nested)
			conflicts++;
	warning_->setVisible(conflicts > 0);
	warning_->setText(T("Dock.OverlapWarning").arg(conflicts));

	RefreshDayView();
}

void StationDock::RefreshDayView()
{
	const QDate d = day_->date();
	dayList_->clear();
	for (const Occurrence &o : schedule_.OccurrencesStartingOn(CivilDate{d.year(), d.month(), d.day()})) {
		auto *item = new QListWidgetItem(Describe(o.slot));
		item->setData(Qt::UserRole, QString::fromStdString(o.slot.id));
		dayList_->addItem(item);
	}
	if (dayList_->count() == 0)
		dayList_->addItem(T("Dock.DayEmpty"));
}

void StationDock::AddSlot(int weekday, int hour, bool oneOffOnSelectedDay)
{
	Slot s;
	s.id = schedule_.GenerateId();
	s.startMinute = std::clamp(hour, 0, 23) * 60;
	if (weekday >= 0)
		s.weekdays = (uint8_t)(1u << weekday);
	if (oneOffOnSelectedDay) {
		const QDate d = day_->date();
		s.date = CivilDate{d.year(), d.month(), d.day()};
	}

	SlotDialog dialog(s, this);
	if (dialog.exec() != QDialog::Accepted)
		return;
	schedule_.slots.push_back(dialog.Result());
	engine_.ForceRefresh();
	Persist();
	RefreshScheduleViews();
}

void StationDock::EditSlot(const std::string &id)
{
	const Slot *existing = schedule_.Find(id);
	if (!existing)
		return;

	SlotDialog dialog(*existing, this);
	if (dialog.exec() != QDialog::Accepted)
		return;
	for (Slot &s : schedule_.slots) {
		if (s.id == id)
			s = dialog.Result();
	}
	engine_.ForceRefresh();
	Persist();
	RefreshScheduleViews();
}

void StationDock::DeleteSlot(const std::string &id)
{
	const Slot *s = schedule_.Find(id);
	if (!s)
		return;
	if (QMessageBox::question(this, T("Dock.Delete"), T("Dock.ConfirmDelete").arg(QString::fromStdString(s->title))) !=
	    QMessageBox::Yes)
		return;
	schedule_.Remove(id);
	engine_.ForceRefresh();
	Persist();
	RefreshScheduleViews();
}

void StationDock::SetAutomation(bool on)
{
	settings_.automationEnabled = on;
	engine_.SetEnabled(on);
	Persist();
	UpdateStatus();
}

void StationDock::TakeManual()
{
	const std::string name = takeList_->currentText().toStdString();
	if (name.empty())
		return;
	QString error;
	if (!playout_.PlayScene(name, error)) {
		QMessageBox::warning(this, T("Dock.Take"), error);
		return;
	}
	engine_.EnterManual(schedule_, Now());
	onAirTitle_ = QString::fromStdString(name);
	UpdateStatus();
}

void StationDock::ResumeAutomatic()
{
	engine_.ResumeAutomatic();
	engine_.ForceRefresh();
	Tick();
}

void StationDock::Tick()
{
	if (!loaded_ || suspended_)
		return;

	const int64_t now = Now();

	auto playFallback = [this]() {
		QString error;
		if (!settings_.fallbackScene.empty() && playout_.PlayScene(settings_.fallbackScene, error)) {
			onAirTitle_ = T("Status.Fallback");
		} else {
			if (!error.isEmpty())
				lastError_ = error;
			onAirTitle_ = T("Status.Gap");
		}
	};

	const SchedulerEngine::Decision decision = engine_.Tick(schedule_, now);
	if (decision.type == SchedulerEngine::Decision::Type::Play && decision.occurrence) {
		const Occurrence &o = *decision.occurrence;
		QString error;
		if (playout_.Play(o.slot, error)) {
			onAirTitle_ = QString::fromStdString(o.slot.title);
			lastError_.clear();
			blog(LOG_INFO, "[station-studio] On air: %s", o.slot.title.c_str());
		} else {
			lastError_ = error;
			blog(LOG_WARNING, "[station-studio] Failed to play '%s': %s", o.slot.title.c_str(),
			     error.toUtf8().constData());
			engine_.ReportFailure(o.Key(), now, true);
			playFallback();
		}
	} else if (decision.type == SchedulerEngine::Decision::Type::Fallback) {
		playFallback();
	}

	if (engine_.Enabled() && !engine_.Manual() && engine_.CurrentKey() && !engine_.CurrentKey()->empty()) {
		const PlayoutController::Health health = playout_.CurrentHealth();
		if (health != PlayoutController::Health::Ok) {
			const bool retry = health == PlayoutController::Health::Error;
			lastError_ = retry ? T("Error.MediaFailed") : T("Status.Ended");
			engine_.ReportFailure(*engine_.CurrentKey(), now, retry);
			playFallback();
		}
	}

	UpdateStatus();
}

void StationDock::UpdateStatus()
{
	automation_->setText(automation_->isChecked() ? T("Dock.AutomationOn") : T("Dock.AutomationOff"));
	automation_->setStyleSheet(automation_->isChecked() ? "background-color: #1b8a3a; color: white;" : QString());

	if (!engine_.Enabled())
		mode_->setText(T("Mode.Off"));
	else if (engine_.Manual())
		mode_->setText(T("Mode.Manual"));
	else
		mode_->setText(T("Mode.Auto"));
	resume_->setEnabled(engine_.Enabled() && engine_.Manual());

	QString onAir = T("Status.OnAir").arg(onAirTitle_.isEmpty() ? QString::fromUtf8("\u2014") : onAirTitle_);
	if (!lastError_.isEmpty())
		onAir += "\n" + T("Status.Error").arg(lastError_);
	onAir_->setText(onAir);

	const auto change = schedule_.NextChange(Now());
	if (!change)
		next_->setText(T("Status.NothingNext"));
	else if (change->next)
		next_->setText(T("Status.Next").arg(HmFromSeconds(change->atSeconds),
						    QString::fromStdString(change->next->slot.title)));
	else
		next_->setText(T("Status.NextGap").arg(HmFromSeconds(change->atSeconds)));
}

void StationDock::OpenOutputsDialog()
{
	OutputsDialog dialog(settings_, outputs_, [this]() { Persist(); }, this);
	dialog.exec();
}

void StationDock::OnFinishedLoading()
{
	loaded_ = true;
	RefreshTakeList();
	engine_.ForceRefresh();
	Tick();
}

void StationDock::OnCollectionChanging()
{
	suspended_ = true;
	playout_.Release();
}

void StationDock::OnCollectionChanged()
{
	suspended_ = false;
	RefreshTakeList();
	engine_.ForceRefresh();
}

void StationDock::OnExit()
{
	timer_->stop();
	suspended_ = true;
	outputs_.StopAll();
	playout_.Release();
	Persist();
}

} // namespace station
