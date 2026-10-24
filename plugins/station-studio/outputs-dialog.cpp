#include "outputs-dialog.hpp"

#include <obs-module.h>

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace station {

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static QLineEdit *LtrEdit(const std::string &value, bool secret = false)
{
	auto *edit = new QLineEdit(QString::fromStdString(value));
	edit->setLayoutDirection(Qt::LeftToRight);
	if (secret)
		edit->setEchoMode(QLineEdit::Password);
	return edit;
}

OutputsDialog::OutputsDialog(StationSettings &settings, OutputsManager &outputs, std::function<void()> persist,
			     QWidget *parent)
	: QDialog(parent),
	  settings_(settings),
	  outputs_(outputs),
	  persist_(std::move(persist))
{
	setWindowTitle(T("Outputs.Title"));
	setMinimumWidth(560);
	auto *root = new QVBoxLayout(this);

	auto *intro = new QLabel(T("Outputs.Intro"));
	intro->setWordWrap(true);
	root->addWidget(intro);

	auto *quality = new QGroupBox(T("Outputs.Quality"));
	auto *qForm = new QFormLayout(quality);
	videoKbps_ = new QSpinBox;
	videoKbps_->setRange(300, 50000);
	videoKbps_->setSingleStep(500);
	videoKbps_->setSuffix(" kbps");
	videoKbps_->setValue(settings.videoKbps);
	audioKbps_ = new QSpinBox;
	audioKbps_->setRange(64, 320);
	audioKbps_->setSingleStep(32);
	audioKbps_->setSuffix(" kbps");
	audioKbps_->setValue(settings.audioKbps);
	qForm->addRow(T("Outputs.VideoBitrate"), videoKbps_);
	qForm->addRow(T("Outputs.AudioBitrate"), audioKbps_);
	root->addWidget(quality);

	auto makeRow = [this](int i, OutputsManager::Target target, QFormLayout *form) {
		rows_[i].target = target;
		rows_[i].status = new QLabel;
		rows_[i].status->setWordWrap(true);
		rows_[i].toggle = new QPushButton;
		auto *line = new QHBoxLayout;
		line->addWidget(rows_[i].status, 1);
		line->addWidget(rows_[i].toggle);
		form->addRow(T("Outputs.Status"), line);
		connect(rows_[i].toggle, &QPushButton::clicked, this, [this, i]() { Toggle(rows_[i]); });
	};

	auto *receiver = new QGroupBox(T("Outputs.Receiver"));
	auto *rForm = new QFormLayout(receiver);
	receiverUrl_ = LtrEdit(settings.receiverUrl);
	rForm->addRow(T("Outputs.Url"), receiverUrl_);
	auto *rHint = new QLabel(T("Outputs.ReceiverHint"));
	rHint->setWordWrap(true);
	rForm->addRow(QString(), rHint);
	makeRow(0, OutputsManager::Target::Receiver, rForm);
	root->addWidget(receiver);

	auto *web = new QGroupBox(T("Outputs.Web"));
	auto *wForm = new QFormLayout(web);
	hlsFolder_ = LtrEdit(settings.hlsFolder);
	auto *browse = new QPushButton(T("Slot.Browse"));
	auto *folderRow = new QHBoxLayout;
	folderRow->addWidget(hlsFolder_, 1);
	folderRow->addWidget(browse);
	wForm->addRow(T("Outputs.Folder"), folderRow);
	hlsPort_ = new QSpinBox;
	hlsPort_->setRange(1024, 65535);
	hlsPort_->setValue(settings.hlsPort);
	wForm->addRow(T("Outputs.Port"), hlsPort_);
	playerUrl_ = LtrEdit("");
	playerUrl_->setReadOnly(true);
	wForm->addRow(T("Outputs.PlayerUrl"), playerUrl_);
	auto *wHint = new QLabel(T("Outputs.WebHint"));
	wHint->setWordWrap(true);
	wForm->addRow(QString(), wHint);
	makeRow(1, OutputsManager::Target::Web, wForm);
	root->addWidget(web);

	auto *yt = new QGroupBox(T("Outputs.YouTube"));
	auto *yForm = new QFormLayout(yt);
	ytServer_ = LtrEdit(settings.youtubeServer);
	ytKey_ = LtrEdit(settings.youtubeKey, true);
	yForm->addRow(T("Outputs.Server"), ytServer_);
	yForm->addRow(T("Outputs.Key"), ytKey_);
	makeRow(2, OutputsManager::Target::YouTube, yForm);
	root->addWidget(yt);

	auto *fb = new QGroupBox(T("Outputs.Facebook"));
	auto *fForm = new QFormLayout(fb);
	fbServer_ = LtrEdit(settings.facebookServer);
	fbKey_ = LtrEdit(settings.facebookKey, true);
	fForm->addRow(T("Outputs.Server"), fbServer_);
	fForm->addRow(T("Outputs.Key"), fbKey_);
	makeRow(3, OutputsManager::Target::Facebook, fForm);
	root->addWidget(fb);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
	buttons->button(QDialogButtonBox::Close)->setText(T("Close"));
	root->addWidget(buttons);

	connect(browse, &QPushButton::clicked, this, [this]() {
		const QString dir = QFileDialog::getExistingDirectory(this, T("Outputs.Folder"), hlsFolder_->text());
		if (!dir.isEmpty())
			hlsFolder_->setText(dir);
	});
	connect(buttons, &QDialogButtonBox::rejected, this, [this]() {
		ApplyFields();
		reject();
	});

	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, &OutputsDialog::Refresh);
	timer->start(1000);
	Refresh();
}

void OutputsDialog::ApplyFields()
{
	settings_.videoKbps = videoKbps_->value();
	settings_.audioKbps = audioKbps_->value();
	settings_.receiverUrl = receiverUrl_->text().trimmed().toStdString();
	settings_.hlsFolder = hlsFolder_->text().trimmed().toStdString();
	settings_.hlsPort = hlsPort_->value();
	settings_.youtubeServer = ytServer_->text().trimmed().toStdString();
	settings_.youtubeKey = ytKey_->text().trimmed().toStdString();
	settings_.facebookServer = fbServer_->text().trimmed().toStdString();
	settings_.facebookKey = fbKey_->text().trimmed().toStdString();
	persist_();
}

void OutputsDialog::Toggle(Row &row)
{
	if (outputs_.Active(row.target)) {
		outputs_.Stop(row.target);
	} else {
		ApplyFields();
		QString error;
		if (!outputs_.Start(row.target, settings_, error))
			QMessageBox::warning(this, windowTitle(), T("Error.StartOutputDetail").arg(error));
	}
	Refresh();
}

void OutputsDialog::Refresh()
{
	for (Row &row : rows_) {
		const bool active = outputs_.Active(row.target);
		const QString err = outputs_.LastError(row.target);
		row.status->setText(active ? T("Outputs.Running") : (err.isEmpty() ? T("Outputs.Stopped") : err));
		row.status->setStyleSheet(active ? "color: #1b8a3a; font-weight: bold;" : QString());
		row.toggle->setText(active ? T("Outputs.Stop") : T("Outputs.Start"));
	}
	const bool webActive = outputs_.Active(OutputsManager::Target::Web);
	playerUrl_->setText(webActive ? outputs_.WebPlayerUrl(settings_) : QString());
}

} // namespace station
