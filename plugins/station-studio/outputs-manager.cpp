#include "outputs-manager.hpp"
#include "ui-common.hpp"
#include "hls-server.hpp"

#include <obs-module.h>

#include <QDir>
#include <QHostAddress>
#include <QNetworkInterface>

#include <cstring>

namespace station {

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

bool OutputsManager::AnyActive() const
{
	return Active(Target::Receiver) || Active(Target::Web) || Active(Target::YouTube) || Active(Target::Facebook);
}

/* H.264 encoders that run on the graphics card, best first. OBS registers
   these only when the matching hardware and drivers are present. */
static const char *kHardwareEncoders[] = {
	"obs_nvenc_h264_tex",                          /* NVIDIA */
	"h264_texture_amf",                            /* AMD */
	"obs_qsv11_v2",                                /* Intel Quick Sync */
	"com.apple.videotoolbox.videoencoder.ave.avc", /* Apple */
	"ffmpeg_vaapi_tex",                            /* Linux VA-API */
};

static bool EncoderUsable(const char *id)
{
	const char *codec = obs_get_encoder_codec(id);
	if (!codec || strcmp(codec, "h264") != 0) {
		return false;
	}
	return (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) == 0;
}

bool OutputsManager::EnsureSharedEncoders(const StationSettings &s, bool allowHardware, QString &error)
{
	/* Encoders already feeding a running output are reused as they are:
	   new outputs join them at the next keyframe. */
	if (videoEncoder_ && audioEncoder_ && obs_encoder_active(videoEncoder_)) {
		return true;
	}

	/* Nothing is running: rebuild so changed settings take effect. */
	videoEncoder_ = nullptr;
	audioEncoder_ = nullptr;
	videoEncoderIsHardware_ = false;

	OBSDataAutoRelease vs = obs_data_create();
	obs_data_set_string(vs, "rate_control", "CBR");
	obs_data_set_int(vs, "bitrate", s.videoKbps);
	obs_data_set_int(vs, "keyint_sec", 2); /* HLS segments and fast joins need regular keyframes */
	/* Stream headers in every keyframe, so HLS segments are self-contained
	   even when the web output joins an encoder that is already running. */
	obs_data_set_bool(vs, "repeat_headers", true);

	OBSEncoderAutoRelease venc;
	if (allowHardware && s.hardwareEncoder) {
		for (const char *id : kHardwareEncoders) {
			if (!EncoderUsable(id)) {
				continue;
			}
			venc = obs_video_encoder_create(id, "station-video", vs, nullptr);
			if (venc) {
				videoEncoderIsHardware_ = true;
				break;
			}
		}
	}
	if (!venc) {
		obs_data_set_string(vs, "preset", "veryfast");
		obs_data_set_string(vs, "profile", "high");
		venc = obs_video_encoder_create("obs_x264", "station-video", vs, nullptr);
	}

	OBSDataAutoRelease as = obs_data_create();
	obs_data_set_int(as, "bitrate", s.audioKbps);
	OBSEncoderAutoRelease aenc = obs_audio_encoder_create("ffmpeg_aac", "station-audio", as, 0, nullptr);
	if (!venc || !aenc) {
		error = T("Error.CreateEncoder");
		return false;
	}
	obs_encoder_set_video(venc, obs_get_video());
	obs_encoder_set_audio(aenc, obs_get_audio());
	videoEncoder_ = venc.Get();
	audioEncoder_ = aenc.Get();

	blog(LOG_INFO, "[station-studio] Extra outputs encode with '%s'", obs_encoder_get_id(videoEncoder_));
	return true;
}

bool OutputsManager::CreateOutput(Target target, const StationSettings &s, OBSOutputAutoRelease &output, QString &error)
{
	switch (target) {
	case Target::Receiver: {
		if (s.receiverUrl.empty()) {
			error = T("Error.NoUrl");
			return false;
		}
		/* SRT encrypts with a 10-79 character passphrase; other protocols
		   here (UDP, RIST) are not encrypted by this output. */
		const bool srt = QString::fromStdString(s.receiverUrl).startsWith("srt://", Qt::CaseInsensitive);
		if (!s.receiverPassphrase.empty()) {
			if (!srt) {
				error = T("Error.PassphraseNeedsSrt");
				return false;
			}
			if (s.receiverPassphrase.size() < 10 || s.receiverPassphrase.size() > 79) {
				error = T("Error.PassphraseLength");
				return false;
			}
		}

		/* Same SRT/RIST output OBS uses for its own stream button. The
		   custom service hands "password" to SRT as its passphrase. */
		OBSDataAutoRelease ss = obs_data_create();
		obs_data_set_string(ss, "server", s.receiverUrl.c_str());
		if (srt && !s.receiverPassphrase.empty()) {
			obs_data_set_bool(ss, "use_auth", true);
			obs_data_set_string(ss, "password", s.receiverPassphrase.c_str());
		}
		OBSServiceAutoRelease service = obs_service_create("rtmp_custom", "station-receiver", ss, nullptr);
		output = obs_output_create("ffmpeg_mpegts_muxer", "station-receiver", nullptr, nullptr);
		if (!service || !output) {
			error = T("Error.CreateOutput");
			return false;
		}
		obs_output_set_service(output, service);
		obs_output_set_reconnect_settings(output, 20, 5);
		return true;
	}
	case Target::Web: {
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
		for (const QString &f : dir.entryList({"live*.ts", "live.m3u8"}, QDir::Files)) {
			dir.remove(f);
		}
		/* OBS's recording muxer writes HLS when the file ends in .m3u8.
		   delete_segments keeps a 24/7 stream from filling the disk. */
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "path", dir.filePath("live.m3u8").toUtf8().constData());
		obs_data_set_string(settings, "muxer_settings",
				    "hls_time=2 hls_list_size=6 hls_flags=delete_segments+independent_segments");
		output = obs_output_create("ffmpeg_muxer", "station-web", settings, nullptr);
		if (!output) {
			error = T("Error.CreateOutput");
			return false;
		}
		return true;
	}
	case Target::YouTube:
	case Target::Facebook: {
		const bool yt = target == Target::YouTube;
		const std::string &server = yt ? s.youtubeServer : s.facebookServer;
		const std::string &key = yt ? s.youtubeKey : s.facebookKey;
		if (server.empty() || key.empty()) {
			error = T("Error.NoKey");
			return false;
		}
		OBSDataAutoRelease ss = obs_data_create();
		obs_data_set_string(ss, "server", server.c_str());
		obs_data_set_string(ss, "key", key.c_str());
		const char *name = yt ? "station-youtube" : "station-facebook";
		OBSServiceAutoRelease service = obs_service_create("rtmp_custom", name, ss, nullptr);
		output = obs_output_create("rtmp_output", name, nullptr, nullptr);
		if (!service || !output) {
			error = T("Error.CreateOutput");
			return false;
		}
		obs_output_set_service(output, service);
		obs_output_set_reconnect_settings(output, 20, 5);
		return true;
	}
	}
	return false;
}

bool OutputsManager::StartOutput(Target target, const StationSettings &s, QString &error)
{
	OBSOutputAutoRelease output;
	if (!CreateOutput(target, s, output, error)) {
		return false;
	}

	/* A hardware encoder can fail to open (busy GPU, driver limits). If no
	   other output depends on it yet, retry once with the software encoder. */
	for (int attempt = 0; attempt < 2; attempt++) {
		const bool allowHardware = attempt == 0;
		if (!EnsureSharedEncoders(s, allowHardware, error)) {
			return false;
		}
		obs_output_set_video_encoder(output, videoEncoder_);
		obs_output_set_audio_encoder(output, audioEncoder_, 0);

		if (obs_output_start(output)) {
			Slot(target) = output.Get();
			return true;
		}

		const char *err = obs_output_get_last_error(output);
		error = err ? QString::fromUtf8(err) : T("Error.StartOutput");
		if (!videoEncoderIsHardware_ || AnyActive()) {
			return false;
		}
		blog(LOG_WARNING, "[station-studio] Hardware encoder failed (%s), falling back to x264",
		     error.toUtf8().constData());
		videoEncoder_ = nullptr; /* force a rebuild without hardware */
	}
	return false;
}

bool OutputsManager::Start(Target target, const StationSettings &settings, QString &error)
{
	if (Active(target)) {
		return true;
	}
	if (!StartOutput(target, settings, error)) {
		return false;
	}

	if (target == Target::Web) {
		if (!hlsServer_) {
			hlsServer_ = std::make_unique<HlsServer>();
		}
		char *playerDir = obs_module_file("player");
		const QString playerFolder = QString::fromUtf8(playerDir ? playerDir : "");
		bfree(playerDir);
		if (!hlsServer_->Start(QString::fromStdString(settings.hlsFolder), playerFolder,
				       (quint16)settings.hlsPort, settings.hlsLanAccess, error)) {
			Stop(Target::Web);
			return false;
		}
	}
	return true;
}

QString OutputsManager::VideoEncoderName() const
{
	if (!videoEncoder_ || !obs_encoder_active(videoEncoder_)) {
		return QString();
	}
	const char *name = obs_encoder_get_display_name(obs_encoder_get_id(videoEncoder_));
	return QString::fromUtf8(name ? name : obs_encoder_get_id(videoEncoder_));
}

void OutputsManager::Stop(Target target)
{
	OBSOutput &out = Slot(target);
	if (out) {
		obs_output_stop(out);
		out = nullptr;
	}
	if (target == Target::Web && hlsServer_) {
		hlsServer_->Stop();
	}
}

void OutputsManager::StopAll()
{
	Stop(Target::Receiver);
	Stop(Target::Web);
	Stop(Target::YouTube);
	Stop(Target::Facebook);
	videoEncoder_ = nullptr;
	audioEncoder_ = nullptr;
	videoEncoderIsHardware_ = false;
}

static QString LanAddress(const StationSettings &s)
{
	if (!s.hlsLanAccess) {
		return "127.0.0.1";
	}
	for (const QHostAddress &addr : QNetworkInterface::allAddresses()) {
		if (addr.protocol() == QAbstractSocket::IPv4Protocol && !addr.isLoopback()) {
			return addr.toString();
		}
	}
	return "127.0.0.1";
}

QString OutputsManager::WebPlayerUrl(const StationSettings &s) const
{
	return QString("http://%1:%2/").arg(LanAddress(s)).arg(s.hlsPort);
}

QString OutputsManager::WebPlaylistUrl(const StationSettings &s) const
{
	return QString("http://%1:%2/live.m3u8").arg(LanAddress(s)).arg(s.hlsPort);
}

} // namespace station
