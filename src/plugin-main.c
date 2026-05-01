/* src/plugin-main.c */
#include <obs-module.h>
#include "plugin-support.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("dynamic-video-cutter", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Automatically jumps through a media source at configurable intervals";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "dynamic-video-cutter";
}

MODULE_EXPORT const char *obs_module_author(void)
{
	return "K_STYER";
}

extern struct obs_source_info video_cutter_filter_info;

bool obs_module_load(void)
{
	obs_register_source(&video_cutter_filter_info);
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "unloaded");
}
