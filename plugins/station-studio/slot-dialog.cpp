#include "slot-dialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimeEdit>

namespace station {

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Saturday-first week, as used by most Arabic-speaking broadcasters. */
static const int kDisplayOrder[7] = {6, 0, 1, 2, 3, 4, 5};

QString SlotDialog::KindName(SlotKind kind)
{
	switch (kind) {
	case SlotKind::Scene:
		return T("Kind.Scene");
	case SlotKind::Source:
		return T("Kind.Source");
	case SlotKind::MediaFile:
		return T("Kind.MediaFile");
	case SlotKind::NetworkStream:
		return T("Kind.NetworkStream");
	case SlotKind::WebPage:
		return T("Kind.WebPage");
	}
	return QString();
}

QString SlotDialog::DayName(int weekday)
{
	const std::string key = "Day." + std::to_string(weekday);
	return T(key.c_str());
}

SlotDialog::SlotDialog(const Slot &slot, QWidget *parent) : QDialog(parent), slot_(slot)
{
	setWindowTitle(slot.title.empty() ? T("Slot.NewTitle") : T("Slot.EditTitle"));
	setMinimumWidth(480);

	auto *form = new QFormLayout;

	title_ = new QLineEdit(QString::fromStdString(slot.title));
	title_->setPlaceholderText(T("Slot.TitlePlaceholder"));
	form->addRow(T("Slot.Title"), title_);

	kind_ = new QComboBox;
	for (int i = 0; i < kSlotKindCount; i++)
		kind_->addItem(KindName((SlotKind)i), i);
	kind_->setCurrentIndex((int)slot.kind);
	form->addRow(T("Slot.Kind"), kind_);

	target_ = new QComboBox;
	target_->setEditable(true);
	target_->setMinimumContentsLength(30);
	/* Paths and URLs are always LTR, even inside an RTL dialog. */
	target_->setLayoutDirection(Qt::LeftToRight);
	browse_ = new QPushButton(T("Slot.Browse"));
	auto *targetRow = new QHBoxLayout;
	targetRow->addWidget(target_, 1);
	targetRow->addWidget(browse_);
	form->addRow(T("Slot.Target"), targetRow);

	hint_ = new QLabel;
	hint_->setWordWrap(true);
	hint_->setStyleSheet("color: palette(mid);");
	form->addRow(QString(), hint_);

	start_ = new QTimeEdit(QTime(slot.startMinute / 60, slot.startMinute % 60));
	start_->setDisplayFormat("HH:mm");
	start_->setLayoutDirection(Qt::LeftToRight);
	form->addRow(T("Slot.Start"), start_);

	duration_ = new QSpinBox;
	duration_->setRange(1, kMinutesPerDay);
	duration_->setValue(slot.durationMinutes);
	duration_->setSuffix(" " + T("Slot.Minutes"));
	form->addRow(T("Slot.Duration"), duration_);

	auto *daysRow = new QHBoxLayout;
	for (int i = 0; i < 7; i++) {
		const int wd = kDisplayOrder[i];
		days_[wd] = new QCheckBox(DayName(wd));
		days_[wd]->setChecked(slot.weekdays & (1u << wd));
		daysRow->addWidget(days_[wd]);
	}
	daysRow->addStretch();
	form->addRow(T("Slot.Days"), daysRow);

	oneOff_ = new QCheckBox(T("Slot.OneOff"));
	oneOff_->setChecked(slot.date.has_value());
	date_ = new QDateEdit;
	date_->setCalendarPopup(true);
	date_->setDisplayFormat("yyyy-MM-dd");
	date_->setLayoutDirection(Qt::LeftToRight);
	date_->setDate(slot.date ? QDate(slot.date->year, slot.date->month, slot.date->day) : QDate::currentDate());
	auto *dateRow = new QHBoxLayout;
	dateRow->addWidget(oneOff_);
	dateRow->addWidget(date_);
	dateRow->addStretch();
	form->addRow(QString(), dateRow);

	loop_ = new QCheckBox(T("Slot.Loop"));
	loop_->setChecked(slot.loop);
	form->addRow(QString(), loop_);

	enabled_ = new QCheckBox(T("Slot.Enabled"));
	enabled_->setChecked(slot.enabled);
	form->addRow(QString(), enabled_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Ok)->setText(T("Save"));
	buttons->button(QDialogButtonBox::Cancel)->setText(T("Cancel"));

	auto *root = new QVBoxLayout(this);
	root->addLayout(form);
	root->addWidget(buttons);

	connect(kind_, &QComboBox::currentIndexChanged, this, [this]() { UpdateKindUi(); });
	connect(browse_, &QPushButton::clicked, this, &SlotDialog::Browse);
	connect(oneOff_, &QCheckBox::toggled, this, [this](bool on) {
		date_->setEnabled(on);
		for (QCheckBox *d : days_)
			d->setEnabled(!on);
	});
	connect(buttons, &QDialogButtonBox::accepted, this, &SlotDialog::Accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	UpdateKindUi();
	target_->setCurrentText(QString::fromStdString(slot.target));
	date_->setEnabled(oneOff_->isChecked());
	for (QCheckBox *d : days_)
		d->setEnabled(!oneOff_->isChecked());
}

void SlotDialog::UpdateKindUi()
{
	const SlotKind kind = (SlotKind)kind_->currentData().toInt();
	const QString current = target_->currentText();
	target_->clear();

	if (kind == SlotKind::Scene) {
		char **names = obs_frontend_get_scene_names();
		for (char **n = names; n && *n; n++)
			target_->addItem(QString::fromUtf8(*n));
		bfree(names);
	} else if (kind == SlotKind::Source) {
		obs_enum_sources(
			[](void *param, obs_source_t *source) {
				const char *name = obs_source_get_name(source);
				if (name && strncmp(name, "station-playout::", 17) != 0)
					static_cast<QComboBox *>(param)->addItem(QString::fromUtf8(name));
				return true;
			},
			target_);
	}
	target_->setCurrentText(current);

	browse_->setVisible(kind == SlotKind::MediaFile);
	loop_->setVisible(kind == SlotKind::MediaFile);

	const char *hints[] = {"Hint.Scene", "Hint.Source", "Hint.MediaFile", "Hint.NetworkStream", "Hint.WebPage"};
	hint_->setText(T(hints[(int)kind]));
}

void SlotDialog::Browse()
{
	const QString file = QFileDialog::getOpenFileName(
		this, T("Slot.Browse"), target_->currentText(),
		"Media (*.mp4 *.mkv *.mov *.ts *.m4v *.webm *.mp3 *.aac *.wav *.flac *.ogg *.m4a);;* (*)");
	if (!file.isEmpty())
		target_->setCurrentText(file);
}

void SlotDialog::Accept()
{
	Slot s = slot_;
	s.title = title_->text().trimmed().toStdString();
	s.kind = (SlotKind)kind_->currentData().toInt();
	s.target = target_->currentText().trimmed().toStdString();
	s.startMinute = start_->time().hour() * 60 + start_->time().minute();
	s.durationMinutes = duration_->value();
	s.weekdays = 0;
	for (int wd = 0; wd < 7; wd++) {
		if (days_[wd]->isChecked())
			s.weekdays |= (uint8_t)(1u << wd);
	}
	if (oneOff_->isChecked()) {
		const QDate d = date_->date();
		s.date = CivilDate{d.year(), d.month(), d.day()};
	} else {
		s.date.reset();
	}
	s.loop = loop_->isChecked();
	s.enabled = enabled_->isChecked();
	if (s.title.empty())
		s.title = s.target;

	const char *errorKey = nullptr;
	switch (ValidateSlot(s)) {
	case SlotError::None:
		break;
	case SlotError::MissingTarget:
		errorKey = "Error.MissingTarget";
		break;
	case SlotError::NoWeekdays:
		errorKey = "Error.NoWeekdays";
		break;
	default:
		errorKey = "Error.BadTime";
		break;
	}
	if (errorKey) {
		QMessageBox::warning(this, windowTitle(), T(errorKey));
		return;
	}

	slot_ = s;
	accept();
}

} // namespace station
