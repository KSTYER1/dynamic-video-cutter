/* src/plugin-support.h */
#pragma once

#include <obs-module.h>

#ifndef PLUGIN_NAME
#define PLUGIN_NAME "dynamic-video-cutter"
#endif

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "1.0.1"
#endif

#ifdef __cplusplus
extern "C" {
#endif

void obs_log(int log_level, const char *format, ...);

#ifdef __cplusplus
}
#endif
