#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QPointer>
#include <QString>

#include "sync-dock.hpp"
#include "calibration-source.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-audio-sync-assistant", "en-US")

static constexpr const char *DOCK_ID = "obs_audio_sync_assistant_dock";
static QPointer<SyncDock> dock;

static void show_dock()
{
    if (!dock)
        dock = new SyncDock();

    if (!obs_frontend_add_dock_by_id(DOCK_ID, "Audio Sync Assistant", dock)) {
        // Dock already exists. OBS will toggle it from the Docks menu.
        return;
    }
}

bool obs_module_load(void)
{
    calibration_source::register_source();
    obs_frontend_add_tools_menu_item("Audio Sync Assistant", [](void *) { show_dock(); }, nullptr);
    show_dock();
    blog(LOG_INFO, "Audio Sync Assistant loaded");
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_dock(DOCK_ID);
    delete dock;
    dock = nullptr;
    blog(LOG_INFO, "Audio Sync Assistant unloaded");
}
