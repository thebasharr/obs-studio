#include "playout-controller.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QFileInfo>

#include <chrono>
#include <cstring>
#include <vector>

namespace station {

static const char *kPlayoutScenes[2] = {"Station Playout A", "Station Playout B"};
static const char *kManagedPrefix = "station-playout::";

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

bool PlayoutController::IsManagedScene(const char *name)
{
	return name && (strcmp(name, kPlayoutScenes[0]) == 0 || strcmp(name, kPlayoutScenes[1]) == 0);
}

static OBSSceneAutoRelease GetOrCreateScene(const char *name)
{
	OBSSourceAutoRelease source = obs_get_source_by_name(name);
	if (source) {
		obs_scene_t *scene = obs_scene_from_source(source);
		return scene ? obs_scene_get_ref(scene) : nullptr;
	}
	return obs_scene_create(name);
}

static void ClearScene(obs_scene_t *scene)
{
	std::vector<OBSSceneItem> items;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<std::vector<OBSSceneItem> *>(param)->emplace_back(item);
			return true;
		},
		&items);

	for (obs_sceneitem_t *item : items) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		const char *name = obs_source_get_name(source);
		const bool owned = name && strncmp(name, kManagedPrefix, strlen(kManagedPrefix)) == 0;
		OBSSource keep = source;
		obs_sceneitem_remove(item);
		if (owned)
			obs_source_remove(keep);
	}
}

OBSSceneAutoRelease PlayoutController::PrepareInactiveScene()
{
	OBSSourceAutoRelease program = obs_frontend_get_current_scene();
	const char *programName = program ? obs_source_get_name(program) : nullptr;

	int index = nextIndex_;
	/* Never rebuild the scene that is currently on air. */
	if (programName && strcmp(programName, kPlayoutScenes[index]) == 0)
		index = 1 - index;

	OBSSceneAutoRelease scene = GetOrCreateScene(kPlayoutScenes[index]);
	if (scene)
		ClearScene(scene);
	nextIndex_ = 1 - index;
	return scene;
}

void PlayoutController::FitToCanvas(obs_sceneitem_t *item)
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;

	vec2 pos{};
	vec2 bounds;
	bounds.x = (float)ovi.base_width;
	bounds.y = (float)ovi.base_height;
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
}

OBSSourceAutoRelease PlayoutController::CreateManagedSource(const Slot &slot, QString &error)
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::string name = std::string(kManagedPrefix) + slot.title + "::" + std::to_string(stamp);

	OBSDataAutoRelease settings = obs_data_create();
	const char *id = nullptr;

	switch (slot.kind) {
	case SlotKind::MediaFile:
		if (!QFileInfo::exists(QString::fromStdString(slot.target))) {
			error = T("Error.FileMissing").arg(QString::fromStdString(slot.target));
			return nullptr;
		}
		id = "ffmpeg_source";
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_string(settings, "local_file", slot.target.c_str());
		obs_data_set_bool(settings, "looping", slot.loop);
		obs_data_set_bool(settings, "restart_on_activate", true);
		obs_data_set_bool(settings, "close_when_inactive", true);
		obs_data_set_bool(settings, "clear_on_media_end", false);
		obs_data_set_bool(settings, "hw_decode", true);
		break;
	case SlotKind::NetworkStream:
		id = "ffmpeg_source";
		obs_data_set_bool(settings, "is_local_file", false);
		obs_data_set_string(settings, "input", slot.target.c_str());
		obs_data_set_int(settings, "reconnect_delay_sec", 5);
		obs_data_set_bool(settings, "restart_on_activate", true);
		obs_data_set_bool(settings, "close_when_inactive", true);
		obs_data_set_bool(settings, "clear_on_media_end", false);
		break;
	case SlotKind::WebPage: {
		if (!obs_source_get_display_name("browser_source")) {
			error = T("Error.NoBrowser");
			return nullptr;
		}
		obs_video_info ovi;
		obs_get_video_info(&ovi);
		id = "browser_source";
		obs_data_set_string(settings, "url", slot.target.c_str());
		obs_data_set_int(settings, "width", ovi.base_width);
		obs_data_set_int(settings, "height", ovi.base_height);
		obs_data_set_bool(settings, "reroute_audio", true);
		obs_data_set_bool(settings, "restart_when_active", true);
		break;
	}
	default:
		return nullptr;
	}

	OBSSourceAutoRelease source = obs_source_create(id, name.c_str(), settings, nullptr);
	if (!source) {
		error = T("Error.CreateSource");
		return nullptr;
	}
	if (radioMonitoring_)
		obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
	return source;
}

bool PlayoutController::PlayScene(const std::string &sceneName, QString &error)
{
	OBSSourceAutoRelease scene = obs_get_source_by_name(sceneName.c_str());
	if (!scene || !obs_scene_from_source(scene)) {
		error = T("Error.SceneMissing").arg(QString::fromStdString(sceneName));
		return false;
	}
	onAirMedia_ = nullptr;
	obs_frontend_set_current_scene(scene);
	return true;
}

bool PlayoutController::Play(const Slot &slot, QString &error)
{
	if (slot.kind == SlotKind::Scene)
		return PlayScene(slot.target, error);

	OBSSourceAutoRelease content;
	bool owned = false;

	if (slot.kind == SlotKind::Source) {
		content = obs_get_source_by_name(slot.target.c_str());
		if (!content) {
			error = T("Error.SourceMissing").arg(QString::fromStdString(slot.target));
			return false;
		}
	} else {
		content = CreateManagedSource(slot, error);
		if (!content)
			return false;
		owned = true;
	}

	OBSSceneAutoRelease scene = PrepareInactiveScene();
	if (!scene) {
		if (owned)
			obs_source_remove(content);
		error = T("Error.CreateSource");
		return false;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, content);
	if (item)
		FitToCanvas(item);

	onAirMedia_ = (owned && slot.kind != SlotKind::WebPage) ? obs_source_get_weak_source(content) : nullptr;
	onAirLoops_ = slot.kind == SlotKind::NetworkStream || slot.loop;

	obs_frontend_set_current_scene(obs_scene_get_source(scene));
	return true;
}

PlayoutController::Health PlayoutController::CurrentHealth() const
{
	if (!onAirMedia_)
		return Health::Ok;
	OBSSourceAutoRelease source = obs_weak_source_get_source(onAirMedia_);
	if (!source)
		return Health::Ok;

	switch (obs_source_media_get_state(source)) {
	case OBS_MEDIA_STATE_ERROR:
		return Health::Error;
	case OBS_MEDIA_STATE_ENDED:
		return onAirLoops_ ? Health::Ok : Health::Ended;
	default:
		return Health::Ok;
	}
}

void PlayoutController::Release()
{
	onAirMedia_ = nullptr;
}

} // namespace station
