#pragma once

#include "station-state.hpp"

#include <obs.hpp>
#include <QString>

#include <memory>

namespace station {

class HlsServer;

/* Extra outputs that run alongside (and independently from) the normal OBS
   Stream/Record buttons. All of them carry the program output. */
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

	/* Local network URLs for the web player while the web output is running. */
	QString WebPlayerUrl(const StationSettings &settings) const;
	QString WebPlaylistUrl(const StationSettings &settings) const;

private:
	bool StartFfmpeg(Target target, const StationSettings &settings, QString &error);
	bool StartRtmp(Target target, const StationSettings &settings, QString &error);
	bool EnsureSharedEncoders(const StationSettings &settings, QString &error);
	OBSOutput &Slot(Target target);
	const OBSOutput &Slot(Target target) const;

	OBSOutput receiver_;
	OBSOutput web_;
	OBSOutput youtube_;
	OBSOutput facebook_;
	OBSEncoder videoEncoder_;
	OBSEncoder audioEncoder_;
	std::unique_ptr<HlsServer> hlsServer_;
};

} // namespace station
