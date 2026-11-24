#pragma once

#include "station-state.hpp"

#include <obs.hpp>
#include <QString>

#include <memory>

namespace station {

class HlsServer;

/* Extra outputs that run alongside (and independently from) the normal OBS
   Stream/Record buttons. All of them carry the program output.

   Every extra output shares one video and one audio encoder, so running all
   four costs a single extra encode instead of one per output. The video
   encoder uses the graphics card when one is available. */
class OutputsManager {
public:
	enum class Target { Receiver, Web, YouTube, Facebook };

	OutputsManager();
	~OutputsManager();

	bool Start(Target target, const StationSettings &settings, QString &error);
	void Stop(Target target);
	bool Active(Target target) const;
	QString LastError(Target target) const;
	void StopAll();

	/* Display name of the video encoder in use, empty when none is running. */
	QString VideoEncoderName() const;

	/* Local network URLs for the web player while the web output is running. */
	QString WebPlayerUrl(const StationSettings &settings) const;
	QString WebPlaylistUrl(const StationSettings &settings) const;

private:
	bool StartOutput(Target target, const StationSettings &settings, QString &error);
	bool CreateOutput(Target target, const StationSettings &settings, OBSOutputAutoRelease &output, QString &error);
	bool EnsureSharedEncoders(const StationSettings &settings, bool allowHardware, QString &error);
	bool AnyActive() const;
	OBSOutput &Slot(Target target);
	const OBSOutput &Slot(Target target) const;

	OBSOutput receiver_;
	OBSOutput web_;
	OBSOutput youtube_;
	OBSOutput facebook_;
	OBSEncoder videoEncoder_;
	OBSEncoder audioEncoder_;
	bool videoEncoderIsHardware_ = false;
	std::unique_ptr<HlsServer> hlsServer_;
};

} // namespace station
