#include "outputs-manager.hpp"
#include "hls-server.hpp"

#include <obs-module.h>

#include <QDir>
#include <QHostAddress>
#include <QNetworkInterface>

namespace station {

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

OutputsManager::OutputsManager() = default;

OutputsManager::~OutputsManager()
{
	StopAll();
}

OBSOutput &OutputsManager::Slot(Target target)
{
	switch (target) {
	case Target::Receiver:
		return receiver_;
	case Target::Web:
		return web_;
	case Target::YouTube:
		return youtube_;
	default:
		return facebook_;
	}
}

const OBSOutput &OutputsManager::Slot(Target target) const
{
	return const_cast<OutputsManager *>(this)->Slot(target);
}

bool OutputsManager::Active(Target target) const
{
	const OBSOutput &out = Slot(target);
	return out && obs_output_active(out);
}

QString OutputsManager::LastError(Target target) const
{
	const OBSOutput &out = Slot(target);
	const char *err = out ? obs_output_get_last_error(out) : nullptr;
	return err ? QString::fromUtf8(err) : QString();
}

static int GopFrames()
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || ovi.fps_den == 0)
		return 60;
	return (int)(2 * ovi.fps_num / ovi.fps_den); /* 2 second keyframes */
}

bool OutputsManager::StartFfmpeg(Target target, const StationSettings &s, QString &error)
{
	OBSDataAutoRelease settings = obs_data_create();
	std::string url;

	if (target == Target::Receiver) {
		url = s.receiverUrl;
		obs_data_set_string(settings, "format_name", "mpegts");
		obs_data_set_string(settings, "muxer_settings", "mpegts_flags=resend_headers");
	} else {
		if (s.hlsFolder.empty()) {
			error = T("Error.NoHlsFolder");
			return false;
		}
		QDir dir(QString::fromStdString(s.hlsFolder));
		if (!dir.mkpath(".")) {
			error = T("Error.NoHlsFolder");
			return false;
		}
		/* Stale segments from an earlier run would confuse players. */
		for (const QString &f : dir.entryList({"live*.ts", "live.m3u8"}, QDir::Files))
			dir.remove(f);
		url = dir.filePath("live.m3u8").toStdString();
		obs_data_set_string(settings, "format_name", "hls");
		obs_data_set_string(settings, "muxer_settings",
				    "hls_time=2 hls_list_size=6 hls_flags=delete_segments+independent_segments");
	}

	if (url.empty()) {
		error = T("Error.NoUrl");
		return false;
	}

	obs_data_set_string(settings, "url", url.c_str());
	obs_data_set_int(settings, "gop_size", GopFrames());
	obs_data_set_int(settings, "video_bitrate", s.videoKbps);
	obs_data_set_string(settings, "video_encoder", "libx264");
	obs_data_set_string(settings, "video_settings", "preset=veryfast tune=zerolatency profile=high");
	obs_data_set_int(settings, "audio_bitrate", s.audioKbps);
	obs_data_set_string(settings, "audio_encoder", "aac");

	const char *name = target == Target::Receiver ? "station-receiver" : "station-web";
	OBSOutputAutoRelease output = obs_output_create("ffmpeg_output", name, settings, nullptr);
	if (!output) {
		error = T("Error.CreateOutput");
		return false;
	}
	obs_output_set_mixers(output, 1);

	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		error = err ? QString::fromUtf8(err) : T("Error.StartOutput");
		return false;
	}

	Slot(target) = output.Get();

	if (target == Target::Web) {
		if (!hlsServer_)
			hlsServer_ = std::make_unique<HlsServer>();
		if (!hlsServer_->Start(QString::fromStdString(s.hlsFolder), (quint16)s.hlsPort, error)) {
			obs_output_stop(output);
			Slot(target) = nullptr;
			return false;
		}
	}
	return true;
}

bool OutputsManager::EnsureSharedEncoders(const StationSettings &s, QString &error)
{
	/* YouTube and Facebook share one encoder pair: one encode, two uploads. */
	if (videoEncoder_ && audioEncoder_) {
		if (obs_encoder_active(videoEncoder_))
			return true;
		OBSDataAutoRelease vs = obs_encoder_get_settings(videoEncoder_);
		obs_data_set_int(vs, "bitrate", s.videoKbps);
		obs_encoder_update(videoEncoder_, vs);
		OBSDataAutoRelease as = obs_encoder_get_settings(audioEncoder_);
		obs_data_set_int(as, "bitrate", s.audioKbps);
		obs_encoder_update(audioEncoder_, as);
		return true;
	}

	OBSDataAutoRelease vs = obs_data_create();
	obs_data_set_string(vs, "rate_control", "CBR");
	obs_data_set_int(vs, "bitrate", s.videoKbps);
	obs_data_set_int(vs, "keyint_sec", 2);
	obs_data_set_string(vs, "preset", "veryfast");
	obs_data_set_string(vs, "profile", "high");

	OBSEncoderAutoRelease venc = obs_video_encoder_create("obs_x264", "station-multistream-video", vs, nullptr);
	OBSDataAutoRelease as = obs_data_create();
	obs_data_set_int(as, "bitrate", s.audioKbps);
	OBSEncoderAutoRelease aenc = obs_audio_encoder_create("ffmpeg_aac", "station-multistream-audio", as, 0, nullptr);
	if (!venc || !aenc) {
		error = T("Error.CreateEncoder");
		return false;
	}
	obs_encoder_set_video(venc, obs_get_video());
	obs_encoder_set_audio(aenc, obs_get_audio());
	videoEncoder_ = venc.Get();
	audioEncoder_ = aenc.Get();
	return true;
}

bool OutputsManager::StartRtmp(Target target, const StationSettings &s, QString &error)
{
	const bool yt = target == Target::YouTube;
	const std::string &server = yt ? s.youtubeServer : s.facebookServer;
	const std::string &key = yt ? s.youtubeKey : s.facebookKey;
	if (server.empty() || key.empty()) {
		error = T("Error.NoKey");
		return false;
	}
	if (!EnsureSharedEncoders(s, error))
		return false;

	OBSDataAutoRelease ss = obs_data_create();
	obs_data_set_string(ss, "server", server.c_str());
	obs_data_set_string(ss, "key", key.c_str());
	OBSServiceAutoRelease service =
		obs_service_create("rtmp_custom", yt ? "station-youtube" : "station-facebook", ss, nullptr);

	OBSOutputAutoRelease output =
		obs_output_create("rtmp_output", yt ? "station-youtube" : "station-facebook", nullptr, nullptr);
	if (!service || !output) {
		error = T("Error.CreateOutput");
		return false;
	}
	obs_output_set_service(output, service);
	obs_output_set_video_encoder(output, videoEncoder_);
	obs_output_set_audio_encoder(output, audioEncoder_, 0);
	obs_output_set_reconnect_settings(output, 20, 5);

	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		error = err ? QString::fromUtf8(err) : T("Error.StartOutput");
		return false;
	}
	Slot(target) = output.Get();
	return true;
}

bool OutputsManager::Start(Target target, const StationSettings &settings, QString &error)
{
	if (Active(target))
		return true;
	if (target == Target::Receiver || target == Target::Web)
		return StartFfmpeg(target, settings, error);
	return StartRtmp(target, settings, error);
}

void OutputsManager::Stop(Target target)
{
	OBSOutput &out = Slot(target);
	if (out) {
		obs_output_stop(out);
		out = nullptr;
	}
	if (target == Target::Web && hlsServer_)
		hlsServer_->Stop();
}

void OutputsManager::StopAll()
{
	Stop(Target::Receiver);
	Stop(Target::Web);
	Stop(Target::YouTube);
	Stop(Target::Facebook);
	videoEncoder_ = nullptr;
	audioEncoder_ = nullptr;
}

static QString LanAddress()
{
	for (const QHostAddress &addr : QNetworkInterface::allAddresses()) {
		if (addr.protocol() == QAbstractSocket::IPv4Protocol && !addr.isLoopback())
			return addr.toString();
	}
	return "127.0.0.1";
}

QString OutputsManager::WebPlayerUrl(const StationSettings &s) const
{
	return QString("http://%1:%2/").arg(LanAddress()).arg(s.hlsPort);
}

QString OutputsManager::WebPlaylistUrl(const StationSettings &s) const
{
	return QString("http://%1:%2/live.m3u8").arg(LanAddress()).arg(s.hlsPort);
}

} // namespace station
