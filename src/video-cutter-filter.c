/* src/video-cutter-filter.c */
#include <obs-module.h>
#include <util/threading.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "plugin-support.h"

#define MAX_REGIONS 5
#define MANUAL_SEEK_THRESHOLD_MS 1500
#define MANUAL_SEEK_COOLDOWN_SEC 2.0f
#define OWN_SEEK_GRACE_SEC 0.35f

typedef struct {
	int64_t start;
	int64_t end;
} active_region_t;

/* ----------------------------------------------------------------
 * Data struct
 * ---------------------------------------------------------------- */

typedef struct {
	obs_source_t *source;
	pthread_mutex_t config_mutex;

	bool background_pause;
	int cut_interval_sec;
	int cut_distance_sec;
	int random_range_sec;

	bool blend_enabled;
	char *blend_filter_name;
	char *active_blend_filter_name;
	int blend_duration_ms;

	struct {
		bool enabled;
		int64_t start_ms;
		int64_t end_ms;
	} regions[MAX_REGIONS];

	int64_t last_media_time_ms;
	int64_t last_duration_ms;
	bool have_last_media_time;
	float manual_seek_cooldown;
	float own_seek_grace;

	float elapsed;
	float blend_timer;
	bool blend_active;
	bool blend_filter_was_enabled;
} video_cutter_t;

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static void deactivate_blend(video_cutter_t *vc, obs_source_t *parent);

static int64_t get_media_duration(obs_source_t *parent)
{
	int64_t duration = obs_source_media_get_duration(parent);
	if (duration > 0)
		return duration;

	const char *id = obs_source_get_id(parent);
	if (!id || strcmp(id, "vlc_source") != 0)
		return 0;

	obs_data_t *settings = obs_source_get_settings(parent);
	if (!settings)
		return 0;

	obs_data_array_t *playlist =
		obs_data_get_array(settings, "playlist");
	if (playlist && obs_data_array_count(playlist) > 0) {
		obs_data_t *item = obs_data_array_item(playlist, 0);
		if (item) {
			int64_t ms = obs_data_get_int(item, "duration");
			if (ms > 0)
				duration = ms;
			obs_data_release(item);
		}
	}
	if (playlist)
		obs_data_array_release(playlist);
	obs_data_release(settings);

	return duration;
}

/* Build sorted list of regions that fit the current media duration. */
static int build_active_regions(video_cutter_t *vc, active_region_t *out,
				int64_t duration)
{
	int count = 0;
	for (int i = 0; i < MAX_REGIONS; i++) {
		if (!vc->regions[i].enabled)
			continue;

		int64_t start = vc->regions[i].start_ms;
		int64_t end = vc->regions[i].end_ms;

		if (start < 0)
			start = 0;
		if (duration > 0 && end > duration)
			end = duration;

		if (duration > 0 && start >= duration)
			continue;
		if (end <= start)
			continue;

		out[count].start = start;
		out[count].end = end;
		count++;
	}
	for (int i = 0; i < count - 1; i++) {
		for (int j = i + 1; j < count; j++) {
			if (out[j].start < out[i].start) {
				active_region_t tmp = out[i];
				out[i] = out[j];
				out[j] = tmp;
			}
		}
	}
	return count;
}

static bool is_inside_any_region(video_cutter_t *vc, int64_t pos,
				 int64_t duration)
{
	active_region_t active[MAX_REGIONS];
	int count = build_active_regions(vc, active, duration);

	for (int i = 0; i < count; i++) {
		if (pos >= active[i].start && pos < active[i].end)
			return true;
	}
	return false;
}

static void remember_media_position(video_cutter_t *vc, int64_t current)
{
	vc->last_media_time_ms = current;
	vc->have_last_media_time = true;
}

static bool detected_external_seek(video_cutter_t *vc, int64_t current,
				   float seconds)
{
	if (!vc->have_last_media_time)
		return false;

	int64_t expected =
		vc->last_media_time_ms + (int64_t)(seconds * 1000.0f);
	int64_t delta = current > expected ? current - expected :
					     expected - current;
	return delta > MANUAL_SEEK_THRESHOLD_MS;
}

static int64_t compute_next_position(video_cutter_t *vc, int64_t current,
				     int64_t duration)
{
	int64_t jump_ms = (int64_t)vc->cut_distance_sec * 1000;

	if (vc->random_range_sec > 0) {
		int range_ms = vc->random_range_sec * 1000;
		int deviation = (rand() % (2 * range_ms + 1)) - range_ms;
		jump_ms += deviation;
	}

	if (jump_ms < 1000)
		jump_ms = 1000;

	active_region_t active[MAX_REGIONS];
	int count = build_active_regions(vc, active, duration);

	if (count == 0) {
		/* No regions defined: simple modulo overflow */
		int64_t next = current + jump_ms;
		if (duration > 0 && next >= duration)
			next = next % duration;
		return next < 0 ? 0 : next;
	}

	/* Find which region contains current position */
	int current_region = -1;
	for (int i = 0; i < count; i++) {
		if (current >= active[i].start && current < active[i].end) {
			current_region = i;
			break;
		}
	}

	if (current_region == -1) {
		/* Outside all regions: snap to next upcoming region,
		 * else wrap to first */
		for (int i = 0; i < count; i++) {
			if (current < active[i].start)
				return active[i].start;
		}
		return active[0].start;
	}

	/* Inside current region: try to jump within it */
	int64_t next = current + jump_ms;
	if (next < active[current_region].end)
		return next;

	/* Jump exits the region: advance to next region (wrap if last) */
	int next_region = (current_region + 1) % count;
	return active[next_region].start;
}

static void activate_blend(video_cutter_t *vc, obs_source_t *parent)
{
	if (!vc->blend_enabled || !vc->blend_filter_name ||
	    vc->blend_filter_name[0] == '\0')
		return;

	if (vc->blend_active)
		deactivate_blend(vc, parent);

	obs_source_t *filter = obs_source_get_filter_by_name(
		parent, vc->blend_filter_name);
	if (filter) {
		if (filter == vc->source) {
			obs_source_release(filter);
			return;
		}

		vc->blend_filter_was_enabled = obs_source_enabled(filter);
		bfree(vc->active_blend_filter_name);
		vc->active_blend_filter_name = bstrdup(vc->blend_filter_name);
		obs_source_set_enabled(filter, true);
		vc->blend_active = true;
		vc->blend_timer = (float)vc->blend_duration_ms / 1000.0f;
		obs_source_release(filter);
	}
}

static void deactivate_blend(video_cutter_t *vc, obs_source_t *parent)
{
	if (!vc->blend_active)
		return;

	const char *filter_name = vc->active_blend_filter_name ?
		vc->active_blend_filter_name :
		vc->blend_filter_name;
	if (!filter_name || filter_name[0] == '\0') {
		vc->blend_active = false;
		vc->blend_filter_was_enabled = false;
		return;
	}

	obs_source_t *filter = obs_source_get_filter_by_name(
		parent, filter_name);
	if (filter) {
		if (filter != vc->source && !vc->blend_filter_was_enabled)
			obs_source_set_enabled(filter, false);
		obs_source_release(filter);
	}
	vc->blend_active = false;
	vc->blend_filter_was_enabled = false;
	bfree(vc->active_blend_filter_name);
	vc->active_blend_filter_name = NULL;
}

/* ----------------------------------------------------------------
 * obs_source_info callbacks
 * ---------------------------------------------------------------- */

static const char *vc_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("FilterName");
}

static void *vc_create(obs_data_t *settings, obs_source_t *source)
{
	video_cutter_t *vc = bzalloc(sizeof(video_cutter_t));
	vc->source = source;
	pthread_mutex_init(&vc->config_mutex, NULL);

	srand((unsigned)time(NULL));

	vc->blend_filter_name = NULL;
	vc->active_blend_filter_name = NULL;
	vc->last_media_time_ms = 0;
	vc->last_duration_ms = 0;
	vc->have_last_media_time = false;
	vc->manual_seek_cooldown = 0.0f;
	vc->own_seek_grace = 0.0f;
	vc->elapsed = 0.0f;
	vc->blend_timer = 0.0f;
	vc->blend_active = false;
	vc->blend_filter_was_enabled = false;

	obs_source_update(source, settings);
	return vc;
}

static void vc_destroy(void *data)
{
	video_cutter_t *vc = data;
	pthread_mutex_destroy(&vc->config_mutex);
	bfree(vc->blend_filter_name);
	bfree(vc->active_blend_filter_name);
	bfree(vc);
}

static void vc_update(void *data, obs_data_t *settings)
{
	video_cutter_t *vc = data;

	pthread_mutex_lock(&vc->config_mutex);

	vc->background_pause =
		obs_data_get_bool(settings, "background_pause");
	vc->cut_interval_sec =
		(int)obs_data_get_int(settings, "cut_interval");
	vc->cut_distance_sec =
		(int)obs_data_get_int(settings, "cut_distance");
	vc->random_range_sec =
		(int)obs_data_get_int(settings, "random_range");

	vc->blend_enabled = obs_data_get_bool(settings, "blend_enabled");

	bfree(vc->blend_filter_name);
	const char *fname =
		obs_data_get_string(settings, "blend_filter_name");
	vc->blend_filter_name =
		(fname && fname[0]) ? bstrdup(fname) : NULL;

	vc->blend_duration_ms =
		(int)obs_data_get_int(settings, "blend_duration_ms");

	for (int i = 0; i < MAX_REGIONS; i++) {
		char key[32];

		snprintf(key, sizeof(key), "region_%d_enabled", i + 1);
		vc->regions[i].enabled = obs_data_get_bool(settings, key);

		snprintf(key, sizeof(key), "region_%d_start_h", i + 1);
		int sh = (int)obs_data_get_int(settings, key);
		snprintf(key, sizeof(key), "region_%d_start_m", i + 1);
		int sm = (int)obs_data_get_int(settings, key);

		snprintf(key, sizeof(key), "region_%d_end_h", i + 1);
		int eh = (int)obs_data_get_int(settings, key);
		snprintf(key, sizeof(key), "region_%d_end_m", i + 1);
		int em = (int)obs_data_get_int(settings, key);

		vc->regions[i].start_ms =
			((int64_t)sh * 3600 + (int64_t)sm * 60) * 1000;
		vc->regions[i].end_ms =
			((int64_t)eh * 3600 + (int64_t)em * 60) * 1000;
	}

	pthread_mutex_unlock(&vc->config_mutex);
}

static void vc_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "background_pause", true);
	obs_data_set_default_int(settings, "cut_interval", 5);
	obs_data_set_default_int(settings, "cut_distance", 30);
	obs_data_set_default_int(settings, "random_range", 0);
	obs_data_set_default_bool(settings, "blend_enabled", false);
	obs_data_set_default_string(settings, "blend_filter_name", "");
	obs_data_set_default_int(settings, "blend_duration_ms", 500);

	for (int i = 0; i < MAX_REGIONS; i++) {
		char key[32];
		snprintf(key, sizeof(key), "region_%d_enabled", i + 1);
		obs_data_set_default_bool(settings, key, false);
		snprintf(key, sizeof(key), "region_%d_start_h", i + 1);
		obs_data_set_default_int(settings, key, 0);
		snprintf(key, sizeof(key), "region_%d_start_m", i + 1);
		obs_data_set_default_int(settings, key, 0);
		snprintf(key, sizeof(key), "region_%d_end_h", i + 1);
		obs_data_set_default_int(settings, key, 0);
		snprintf(key, sizeof(key), "region_%d_end_m", i + 1);
		obs_data_set_default_int(settings, key, 0);
	}
}

/* Callback: show/hide region fields based on toggle property name */
static bool on_region_toggle(obs_properties_t *props, obs_property_t *prop,
			     obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	for (int i = 0; i < MAX_REGIONS; i++) {
		int n = i + 1;
		char key_en[32], key_sh[32], key_sm[32], key_eh[32],
			key_em[32], key_bs[32], key_be[32];

		snprintf(key_en, sizeof(key_en), "region_%d_enabled", n);
		snprintf(key_sh, sizeof(key_sh), "region_%d_start_h", n);
		snprintf(key_sm, sizeof(key_sm), "region_%d_start_m", n);
		snprintf(key_eh, sizeof(key_eh), "region_%d_end_h", n);
		snprintf(key_em, sizeof(key_em), "region_%d_end_m", n);
		snprintf(key_bs, sizeof(key_bs), "region_%d_btn_start", n);
		snprintf(key_be, sizeof(key_be), "region_%d_btn_end", n);

		bool active = obs_data_get_bool(settings, key_en);

		obs_property_t *p;
		p = obs_properties_get(props, key_sh);
		if (p)
			obs_property_set_visible(p, active);
		p = obs_properties_get(props, key_sm);
		if (p)
			obs_property_set_visible(p, active);
		p = obs_properties_get(props, key_eh);
		if (p)
			obs_property_set_visible(p, active);
		p = obs_properties_get(props, key_em);
		if (p)
			obs_property_set_visible(p, active);
		p = obs_properties_get(props, key_bs);
		if (p)
			obs_property_set_visible(p, active);
		p = obs_properties_get(props, key_be);
		if (p)
			obs_property_set_visible(p, active);
	}
	return true;
}

/* Callback: show/hide blend fields */
static bool on_blend_toggle(obs_properties_t *props, obs_property_t *prop,
			    obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	bool active = obs_data_get_bool(settings, "blend_enabled");
	obs_property_set_visible(
		obs_properties_get(props, "blend_filter_name"), active);
	obs_property_set_visible(
		obs_properties_get(props, "blend_duration_ms"), active);
	return true;
}

/* Callback: button to copy current playback position into region time fields */
static bool on_region_set_button(obs_properties_t *props,
				 obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	video_cutter_t *vc = data;

	const char *prop_name = obs_property_name(property);
	if (!prop_name)
		return false;

	int region_n = 0;
	char which[8] = {0};
	if (sscanf(prop_name, "region_%d_btn_%7s", &region_n, which) != 2)
		return false;
	if (region_n < 1 || region_n > MAX_REGIONS)
		return false;

	obs_source_t *parent = obs_filter_get_parent(vc->source);
	if (!parent)
		return false;

	enum obs_media_state state = obs_source_media_get_state(parent);
	if (state != OBS_MEDIA_STATE_PLAYING &&
	    state != OBS_MEDIA_STATE_PAUSED)
		return false;

	int64_t pos_ms = obs_source_media_get_time(parent);
	if (pos_ms < 0)
		return false;

	int total_seconds = (int)(pos_ms / 1000);
	int hours = total_seconds / 3600;
	int minutes = (total_seconds % 3600) / 60;
	if (hours > 99)
		hours = 99;

	char key_h[32], key_m[32];
	if (strcmp(which, "start") == 0) {
		snprintf(key_h, sizeof(key_h), "region_%d_start_h",
			 region_n);
		snprintf(key_m, sizeof(key_m), "region_%d_start_m",
			 region_n);
	} else if (strcmp(which, "end") == 0) {
		snprintf(key_h, sizeof(key_h), "region_%d_end_h", region_n);
		snprintf(key_m, sizeof(key_m), "region_%d_end_m", region_n);
	} else {
		return false;
	}

	obs_data_t *settings = obs_source_get_settings(vc->source);
	obs_data_set_int(settings, key_h, hours);
	obs_data_set_int(settings, key_m, minutes);
	obs_source_update(vc->source, settings);
	obs_data_release(settings);

	return true;
}

/* Callback: enumerate sibling filters for the blend dropdown */
struct enum_filters_ctx {
	video_cutter_t *vc;
	obs_property_t *list;
};

static void enum_filters_cb(obs_source_t *parent, obs_source_t *child,
			    void *param)
{
	UNUSED_PARAMETER(parent);
	struct enum_filters_ctx *ctx = param;
	if (!ctx || child == ctx->vc->source)
		return;

	const char *name = obs_source_get_name(child);
	if (name)
		obs_property_list_add_string(ctx->list, name, name);
}

static obs_properties_t *vc_get_properties(void *data)
{
	video_cutter_t *vc = data;
	obs_properties_t *props = obs_properties_create();

	/* Top-level toggle */
	obs_properties_add_bool(props, "background_pause",
				obs_module_text("BackgroundPause"));

	/* Group: Cut Behavior */
	obs_properties_t *grp_cut = obs_properties_create();

	obs_properties_add_int_slider(grp_cut, "cut_interval",
				      obs_module_text("CutInterval"), 1, 32,
				      1);
	obs_properties_add_int_slider(grp_cut, "cut_distance",
				      obs_module_text("CutDistance"), 10, 600,
				      10);

	obs_property_t *p_random = obs_properties_add_int_slider(
		grp_cut, "random_range", obs_module_text("RandomRange"), 0,
		300, 5);
	obs_property_set_long_description(
		p_random, obs_module_text("RandomRangeTooltip"));

	obs_properties_add_group(props, "grp_cut",
				 obs_module_text("GroupCut"),
				 OBS_GROUP_NORMAL, grp_cut);

	/* Group: Blend on Cut */
	obs_properties_t *grp_blend = obs_properties_create();

	obs_property_t *blend_toggle = obs_properties_add_bool(
		grp_blend, "blend_enabled", obs_module_text("BlendEnable"));

	obs_property_t *blend_list = obs_properties_add_list(
		grp_blend, "blend_filter_name",
		obs_module_text("BlendFilter"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(blend_list,
				     obs_module_text("BlendFilterNone"), "");

	obs_source_t *parent = obs_filter_get_parent(vc->source);
	if (parent) {
		struct enum_filters_ctx ctx = {vc, blend_list};
		obs_source_enum_filters(parent, enum_filters_cb, &ctx);
	}

	obs_properties_add_int(grp_blend, "blend_duration_ms",
			       obs_module_text("BlendDuration"), 50, 5000,
			       50);

	obs_property_set_modified_callback(blend_toggle, on_blend_toggle);
	obs_property_set_visible(
		obs_properties_get(grp_blend, "blend_filter_name"),
		vc->blend_enabled);
	obs_property_set_visible(
		obs_properties_get(grp_blend, "blend_duration_ms"),
		vc->blend_enabled);

	obs_properties_add_group(props, "grp_blend",
				 obs_module_text("GroupBlend"),
				 OBS_GROUP_NORMAL, grp_blend);

	/* Groups: Playback Regions 1-5 */
	for (int i = 0; i < MAX_REGIONS; i++) {
		int n = i + 1;
		char grp_id[32];
		char key_en[32], key_sh[32], key_sm[32], key_eh[32],
			key_em[32], key_bs[32], key_be[32];
		char grp_label[64];

		snprintf(grp_id, sizeof(grp_id), "grp_region_%d", n);
		snprintf(key_en, sizeof(key_en), "region_%d_enabled", n);
		snprintf(key_sh, sizeof(key_sh), "region_%d_start_h", n);
		snprintf(key_sm, sizeof(key_sm), "region_%d_start_m", n);
		snprintf(key_eh, sizeof(key_eh), "region_%d_end_h", n);
		snprintf(key_em, sizeof(key_em), "region_%d_end_m", n);
		snprintf(key_bs, sizeof(key_bs), "region_%d_btn_start", n);
		snprintf(key_be, sizeof(key_be), "region_%d_btn_end", n);
		snprintf(grp_label, sizeof(grp_label), "%s %d",
			 obs_module_text("GroupRegion"), n);

		obs_properties_t *grp_region = obs_properties_create();

		obs_property_t *region_toggle = obs_properties_add_bool(
			grp_region, key_en, obs_module_text("RegionEnable"));

		obs_property_t *p_sh = obs_properties_add_int(
			grp_region, key_sh, obs_module_text("RegionStartH"), 0,
			99, 1);
		obs_property_t *p_sm = obs_properties_add_int(
			grp_region, key_sm, obs_module_text("RegionStartM"), 0,
			59, 1);
		obs_property_t *p_bs = obs_properties_add_button(
			grp_region, key_bs,
			obs_module_text("RegionSetStart"),
			on_region_set_button);
		obs_property_t *p_eh = obs_properties_add_int(
			grp_region, key_eh, obs_module_text("RegionEndH"), 0,
			99, 1);
		obs_property_t *p_em = obs_properties_add_int(
			grp_region, key_em, obs_module_text("RegionEndM"), 0,
			59, 1);
		obs_property_t *p_be = obs_properties_add_button(
			grp_region, key_be,
			obs_module_text("RegionSetEnd"),
			on_region_set_button);

		bool region_active = vc->regions[i].enabled;
		obs_property_set_visible(p_sh, region_active);
		obs_property_set_visible(p_sm, region_active);
		obs_property_set_visible(p_eh, region_active);
		obs_property_set_visible(p_em, region_active);
		obs_property_set_visible(p_bs, region_active);
		obs_property_set_visible(p_be, region_active);

		obs_property_set_modified_callback(region_toggle,
						   on_region_toggle);

		obs_properties_add_group(props, grp_id, grp_label,
					OBS_GROUP_NORMAL, grp_region);
	}

	return props;
}

/* ----------------------------------------------------------------
 * video_tick — core jump logic
 * ---------------------------------------------------------------- */

static void vc_video_tick(void *data, float seconds)
{
	video_cutter_t *vc = data;

	pthread_mutex_lock(&vc->config_mutex);

	obs_source_t *parent = obs_filter_get_parent(vc->source);
	if (!parent) {
		pthread_mutex_unlock(&vc->config_mutex);
		return;
	}

	/* Always tick blend timer so it cleans up if the filter was just disabled */
	if (vc->blend_active) {
		vc->blend_timer -= seconds;
		if (vc->blend_timer <= 0.0f)
			deactivate_blend(vc, parent);
	}

	/* Filter eye-toggle controls activity */
	if (!obs_source_enabled(vc->source)) {
		vc->elapsed = 0.0f;
		pthread_mutex_unlock(&vc->config_mutex);
		return;
	}

	if (vc->background_pause && !obs_source_showing(parent)) {
		pthread_mutex_unlock(&vc->config_mutex);
		return;
	}

	int64_t duration = get_media_duration(parent);
	int64_t current = obs_source_media_get_time(parent);

	if (duration <= 0 || current < 0) {
		vc->have_last_media_time = false;
		vc->last_duration_ms = 0;
		pthread_mutex_unlock(&vc->config_mutex);
		return;
	}

	if (vc->last_duration_ms > 0 && duration != vc->last_duration_ms) {
		vc->elapsed = 0.0f;
		vc->manual_seek_cooldown = 0.0f;
		vc->own_seek_grace = 0.0f;
		vc->last_duration_ms = duration;
		remember_media_position(vc, current);
		pthread_mutex_unlock(&vc->config_mutex);
		return;
	}
	vc->last_duration_ms = duration;

	enum obs_media_state state = obs_source_media_get_state(parent);
	if (state != OBS_MEDIA_STATE_PLAYING) {
		vc->elapsed = 0.0f;
		vc->manual_seek_cooldown = 0.0f;
		vc->own_seek_grace = 0.0f;
		remember_media_position(vc, current);
		pthread_mutex_unlock(&vc->config_mutex);
		return;
	}

	if (vc->own_seek_grace > 0.0f) {
		vc->own_seek_grace -= seconds;
		vc->elapsed = 0.0f;
		remember_media_position(vc, current);
		pthread_mutex_unlock(&vc->config_mutex);
		return;
	}

	if (vc->manual_seek_cooldown > 0.0f) {
		vc->manual_seek_cooldown -= seconds;
		vc->elapsed = 0.0f;
		remember_media_position(vc, current);
		pthread_mutex_unlock(&vc->config_mutex);
		return;
	}

	if (detected_external_seek(vc, current, seconds)) {
		vc->manual_seek_cooldown = MANUAL_SEEK_COOLDOWN_SEC;
		vc->elapsed = 0.0f;
		remember_media_position(vc, current);
		pthread_mutex_unlock(&vc->config_mutex);
		return;
	}

	/* Check whether we have any active regions and whether current
	 * position is outside all of them — outside = taboo, snap immediately */
	active_region_t active[MAX_REGIONS];
	bool has_regions = build_active_regions(vc, active, duration) > 0;
	bool outside = has_regions &&
		       !is_inside_any_region(vc, current, duration);

	vc->elapsed += seconds;
	bool interval_due = vc->elapsed >= (float)vc->cut_interval_sec;

	if (!outside && !interval_due) {
		remember_media_position(vc, current);
		pthread_mutex_unlock(&vc->config_mutex);
		return;
	}
	vc->elapsed = 0.0f;

	int64_t next = compute_next_position(vc, current, duration);
	if (next < 0)
		next = 0;
	if (duration > 0 && next >= duration)
		next = duration > 1 ? duration - 1 : 0;
	obs_source_media_set_time(parent, next);
	remember_media_position(vc, next);
	vc->own_seek_grace = OWN_SEEK_GRACE_SEC;

	/* Only blend on a planned interval cut, not on outside-snap correction */
	if (interval_due && !outside)
		activate_blend(vc, parent);

	pthread_mutex_unlock(&vc->config_mutex);
}

static void vc_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	video_cutter_t *vc = data;
	obs_source_skip_video_filter(vc->source);
}

/* ----------------------------------------------------------------
 * obs_source_info
 * ---------------------------------------------------------------- */

struct obs_source_info video_cutter_filter_info = {
	.id = "dynamic_video_cutter_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = vc_get_name,
	.create = vc_create,
	.destroy = vc_destroy,
	.update = vc_update,
	.get_defaults = vc_get_defaults,
	.get_properties = vc_get_properties,
	.video_tick = vc_video_tick,
	.video_render = vc_video_render,
};
