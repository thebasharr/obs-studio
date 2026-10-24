#include "station-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QMainWindow>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("station-studio", "ar-SA")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Station Studio: 24/7 TV/radio scheduling, playout and extra outputs";
}

static QPointer<station::StationDock> dock;

static void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	if (!dock)
		return;
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		dock->OnFinishedLoading();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		dock->OnCollectionChanging();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		dock->OnCollectionChanged();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		dock->OnExit();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main)
		return false;

	dock = new station::StationDock(main);
	if (!obs_frontend_add_dock_by_id("station-studio", obs_module_text("Dock.Title"), dock)) {
		delete dock;
		return false;
	}

	obs_frontend_add_tools_menu_item(
		obs_module_text("Menu.Outputs"),
		[](void *) {
			if (dock)
				dock->OpenOutputsDialog();
		},
		nullptr);

	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
}
