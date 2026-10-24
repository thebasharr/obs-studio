#pragma once

#include "core/station-schedule.hpp"

#include <string>

namespace station {

struct StationSettings {
	bool automationEnabled = false;
	std::string fallbackScene;
	bool releaseManualAtNextSlot = false;
	bool radioMonitoring = true;

	/* Extra outputs. Each re-encodes the program output independently of the
	   main OBS stream/recording, so the operator's normal workflow is untouched. */
	int videoKbps = 4000;
	int audioKbps = 160;

	std::string receiverUrl = "srt://0.0.0.0:9000?mode=listener";

	std::string hlsFolder;
	int hlsPort = 8080;

	std::string youtubeServer = "rtmp://a.rtmp.youtube.com/live2";
	std::string youtubeKey;
	std::string facebookServer = "rtmps://live-api-s.facebook.com:443/rtmp/";
	std::string facebookKey;
};

/* Stored as JSON in the plugin config dir (station-studio/station.json),
   written atomically with a .bak so a crash mid-save cannot lose the schedule. */
bool LoadState(Schedule &schedule, StationSettings &settings);
bool SaveState(const Schedule &schedule, const StationSettings &settings);

} // namespace station
