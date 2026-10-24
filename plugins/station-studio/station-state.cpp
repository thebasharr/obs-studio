#include "station-state.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <util/platform.h>

namespace station {

static std::string ConfigFile()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("station.json");
	std::string out = path ? path : "";
	bfree(path);
	return out;
}

static std::string GetStr(obs_data_t *d, const char *key, const std::string &def)
{
	return obs_data_has_user_value(d, key) ? obs_data_get_string(d, key) : def;
}

static int GetInt(obs_data_t *d, const char *key, int def)
{
	return obs_data_has_user_value(d, key) ? (int)obs_data_get_int(d, key) : def;
}

static bool GetBool(obs_data_t *d, const char *key, bool def)
{
	return obs_data_has_user_value(d, key) ? obs_data_get_bool(d, key) : def;
}

bool LoadState(Schedule &schedule, StationSettings &s)
{
	const std::string path = ConfigFile();
	if (path.empty())
		return false;

	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!root)
		return false;

	schedule.slots.clear();
	OBSDataArrayAutoRelease slots = obs_data_get_array(root, "slots");
	const size_t count = obs_data_array_count(slots);
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(slots, i);
		Slot slot;
		slot.id = obs_data_get_string(item, "id");
		slot.title = obs_data_get_string(item, "title");
		slot.startMinute = (int)obs_data_get_int(item, "start_minute");
		slot.durationMinutes = (int)obs_data_get_int(item, "duration_minutes");
		slot.weekdays = (uint8_t)(obs_data_get_int(item, "weekdays") & kEveryDay);
		const char *date = obs_data_get_string(item, "date");
		if (date && *date)
			slot.date = ParseDate(date);
		const int kind = (int)obs_data_get_int(item, "kind");
		slot.kind = (kind >= 0 && kind < kSlotKindCount) ? (SlotKind)kind : SlotKind::Scene;
		slot.target = obs_data_get_string(item, "target");
		slot.loop = GetBool(item, "loop", true);
		slot.enabled = GetBool(item, "enabled", true);
		if (slot.id.empty())
			slot.id = schedule.GenerateId();
		schedule.slots.push_back(std::move(slot));
	}

	const StationSettings def;
	s.automationEnabled = GetBool(root, "automation_enabled", def.automationEnabled);
	s.fallbackScene = GetStr(root, "fallback_scene", def.fallbackScene);
	s.releaseManualAtNextSlot = GetBool(root, "release_manual_at_next_slot", def.releaseManualAtNextSlot);
	s.radioMonitoring = GetBool(root, "radio_monitoring", def.radioMonitoring);
	s.videoKbps = GetInt(root, "video_kbps", def.videoKbps);
	s.audioKbps = GetInt(root, "audio_kbps", def.audioKbps);
	s.receiverUrl = GetStr(root, "receiver_url", def.receiverUrl);
	s.hlsFolder = GetStr(root, "hls_folder", def.hlsFolder);
	s.hlsPort = GetInt(root, "hls_port", def.hlsPort);
	s.youtubeServer = GetStr(root, "youtube_server", def.youtubeServer);
	s.youtubeKey = GetStr(root, "youtube_key", def.youtubeKey);
	s.facebookServer = GetStr(root, "facebook_server", def.facebookServer);
	s.facebookKey = GetStr(root, "facebook_key", def.facebookKey);
	return true;
}

bool SaveState(const Schedule &schedule, const StationSettings &s)
{
	const std::string path = ConfigFile();
	if (path.empty())
		return false;

	OBSDataAutoRelease root = obs_data_create();
	OBSDataArrayAutoRelease slots = obs_data_array_create();
	for (const Slot &slot : schedule.slots) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "id", slot.id.c_str());
		obs_data_set_string(item, "title", slot.title.c_str());
		obs_data_set_int(item, "start_minute", slot.startMinute);
		obs_data_set_int(item, "duration_minutes", slot.durationMinutes);
		obs_data_set_int(item, "weekdays", slot.weekdays);
		obs_data_set_string(item, "date", slot.date ? FormatDate(*slot.date).c_str() : "");
		obs_data_set_int(item, "kind", (int)slot.kind);
		obs_data_set_string(item, "target", slot.target.c_str());
		obs_data_set_bool(item, "loop", slot.loop);
		obs_data_set_bool(item, "enabled", slot.enabled);
		obs_data_array_push_back(slots, item);
	}
	obs_data_set_array(root, "slots", slots);

	obs_data_set_bool(root, "automation_enabled", s.automationEnabled);
	obs_data_set_string(root, "fallback_scene", s.fallbackScene.c_str());
	obs_data_set_bool(root, "release_manual_at_next_slot", s.releaseManualAtNextSlot);
	obs_data_set_bool(root, "radio_monitoring", s.radioMonitoring);
	obs_data_set_int(root, "video_kbps", s.videoKbps);
	obs_data_set_int(root, "audio_kbps", s.audioKbps);
	obs_data_set_string(root, "receiver_url", s.receiverUrl.c_str());
	obs_data_set_string(root, "hls_folder", s.hlsFolder.c_str());
	obs_data_set_int(root, "hls_port", s.hlsPort);
	obs_data_set_string(root, "youtube_server", s.youtubeServer.c_str());
	obs_data_set_string(root, "youtube_key", s.youtubeKey.c_str());
	obs_data_set_string(root, "facebook_server", s.facebookServer.c_str());
	obs_data_set_string(root, "facebook_key", s.facebookKey.c_str());

	return obs_data_save_json_safe(root, path.c_str(), "tmp", "bak");
}

} // namespace station
