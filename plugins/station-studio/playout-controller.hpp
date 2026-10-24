#pragma once

#include "core/station-schedule.hpp"

#include <obs.hpp>
#include <QString>

#include <string>

namespace station {

/* Puts content on air through the normal OBS program output, so the main
   stream, recording, virtual camera and audio monitoring all follow it.

   Scene/Source slots reuse what the operator already configured in OBS
   (cameras, HDMI capture cards, layouts). File/stream/web slots are built
   inside two alternating managed scenes ("Station Playout A/B"), so the
   operator's configured OBS transition runs between old and new content. */
class PlayoutController {
public:
	enum class Health { Ok, Ended, Error };

	bool Play(const Slot &slot, QString &error);
	bool PlayScene(const std::string &sceneName, QString &error);

	void SetRadioMonitoring(bool enabled) { radioMonitoring_ = enabled; }

	/* State of the managed media source currently on air (if any). */
	Health CurrentHealth() const;

	/* Drop references before a scene collection is unloaded / on exit. */
	void Release();

	static bool IsManagedScene(const char *name);

private:
	OBSSceneAutoRelease PrepareInactiveScene();
	OBSSourceAutoRelease CreateManagedSource(const Slot &slot, QString &error);
	void FitToCanvas(obs_sceneitem_t *item);

	int nextIndex_ = 0;
	bool radioMonitoring_ = true;
	OBSWeakSourceAutoRelease onAirMedia_;
	bool onAirLoops_ = true;
};

} // namespace station
