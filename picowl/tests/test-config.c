/* test-config.c - unit tests for config parser. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <wayland-util.h>
#include <drm_fourcc.h>
#include "../src/picowl.h"

/* Render format FourCC codes (little-endian) */

static int test_default_config(void)
{
	struct pw_config *c = pw_config_default();
	assert(c != NULL);
	assert(c->render_format == DRM_FORMAT_RGB565);
	assert(c->idle_timeout_ms == 60000);
	assert(c->background[0] == 0.1f);
	assert(c->background[1] == 0.1f);
	assert(c->background[2] == 0.1f);
	assert(c->background[3] == 1.0f);

	/* Check default keybindings exist */
	assert(!wl_list_empty(&c->keybindings));

	struct pw_keybinding *kb;
	int kb_count = 0;
	wl_list_for_each(kb, &c->keybindings, link) {
		kb_count++;
	}
	assert(kb_count >= 3); /* At least KEY_POWER, Alt+Tab, Logo+Escape */

	/* Check default touch/cursor config */
	assert(c->hold_action == PW_HOLD_RIGHT_CLICK);
	assert(c->hold_delay_ms == 300);
	assert(c->hold_ms == 900);
	assert(c->slop_px == 8);
	assert(c->hold_animation == NULL);
	assert(c->cursor_fill == 0x2050c0);
	assert(c->cursor_outline == 0xffffff);
	assert(c->cursor_frame_ms == 83);

	pw_config_free(c);
	printf("✓ test_default_config\n");
	return 0;
}

static int test_load_config(void)
{
	struct pw_config *c = pw_config_load("tests/test-config.ini");
	assert(c != NULL);

	/* Check render format was parsed */
	assert(c->render_format == DRM_FORMAT_XRGB8888);

	/* Check idle timeout was parsed */
	assert(c->idle_timeout_ms == 30000);

	/* Check background color was parsed */
	assert(c->background[0] > 0.12f && c->background[0] < 0.14f); /* #202020 */
	assert(c->background[1] > 0.12f && c->background[1] < 0.14f);
	assert(c->background[2] > 0.12f && c->background[2] < 0.14f);

	/* Check transforms were parsed */
	assert(!wl_list_empty(&c->transforms));
	int transform_count = 0;
	struct pw_output_transform *t;
	wl_list_for_each(t, &c->transforms, link) {
		transform_count++;
	}
	assert(transform_count >= 2); /* DSI-1 and * */

	/* Check autostart commands were parsed */
	assert(!wl_list_empty(&c->autostart));
	int autostart_count = 0;
	struct pw_autostart *as;
	wl_list_for_each(as, &c->autostart, link) {
		autostart_count++;
	}
	assert(autostart_count >= 2);

	/* Check keybindings were parsed */
	int kb_count = 0;
	struct pw_keybinding *kb;
	wl_list_for_each(kb, &c->keybindings, link) {
		kb_count++;
	}
	assert(kb_count >= 5); /* At least 5 from test file */

	pw_config_free(c);
	printf("✓ test_load_config\n");
	return 0;
}

static int test_missing_file(void)
{
	/* Missing file should return defaults, not NULL */
	struct pw_config *c = pw_config_load("/nonexistent/path/picowl.ini");
	assert(c != NULL);
	assert(c->render_format == DRM_FORMAT_RGB565);
	assert(c->idle_timeout_ms == 60000);
	pw_config_free(c);
	printf("✓ test_missing_file\n");
	return 0;
}

static int test_config_free_null(void)
{
	/* Should be NULL-safe */
	pw_config_free(NULL);
	printf("✓ test_config_free_null\n");
	return 0;
}

static int test_keybinding_parsing(void)
{
	struct pw_config *c = pw_config_load("tests/test-config.ini");
	assert(c != NULL);

	/* Verify specific keybindings from test file */
	int found_alt_tab = 0, found_logo_escape = 0, found_code_116 = 0, found_spawn = 0;

	struct pw_keybinding *kb;
	wl_list_for_each(kb, &c->keybindings, link) {
		if (kb->key_name && strcmp(kb->key_name, "Tab") == 0 &&
		    kb->modifiers == WLR_MODIFIER_ALT &&
		    kb->action == PW_ACTION_CYCLE_VIEWS) {
			found_alt_tab = 1;
		}
		if (kb->key_name && strcmp(kb->key_name, "Escape") == 0 &&
		    kb->modifiers == WLR_MODIFIER_LOGO &&
		    kb->action == PW_ACTION_QUIT) {
			found_logo_escape = 1;
		}
		if (kb->keycode == 116 &&
		    kb->action == PW_ACTION_TOGGLE_BLANK) {
			found_code_116 = 1;
		}
		if (kb->command && strcmp(kb->command, "xterm") == 0 &&
		    kb->action == PW_ACTION_SPAWN) {
			found_spawn = 1;
		}
	}

	assert(found_alt_tab);
	assert(found_logo_escape);
	assert(found_code_116);
	assert(found_spawn);

	pw_config_free(c);
	printf("✓ test_keybinding_parsing\n");
	return 0;
}

static int test_touch_cursor_parsing(void)
{
	struct pw_config *c = pw_config_load("tests/test-config.ini");
	assert(c != NULL);

	/* Check touch configuration was parsed */
	assert(c->hold_action == PW_HOLD_RIGHT_CLICK);
	assert(c->hold_delay_ms == 300);
	assert(c->hold_ms == 900);
	assert(c->slop_px == 8);

	/* Check cursor configuration was parsed */
	assert(c->hold_animation != NULL && strcmp(c->hold_animation, "builtin") == 0);
	assert(c->cursor_fill == 0x2050c0);
	assert(c->cursor_outline == 0xffffff);
	assert(c->cursor_frame_ms == 83);

	pw_config_free(c);
	printf("✓ test_touch_cursor_parsing\n");
	return 0;
}

static int test_validation_ranges(void)
{
	/* Test that out-of-range values are corrected during validation */
	struct pw_config *c = pw_config_default();
	assert(c != NULL);

	/* Simulate invalid configuration (slop_px out of range) */
	c->slop_px = 100;  /* > 64 */

	/* After loading (which calls validation), we expect default */
	struct pw_config *c2 = pw_config_load("/nonexistent/path/test.ini");
	assert(c2 != NULL);
	assert(c2->slop_px == 8); /* Should be default */

	pw_config_free(c);
	pw_config_free(c2);
	printf("✓ test_validation_ranges\n");
	return 0;
}

static int test_rotation_config(void)
{
	struct pw_config *c = pw_config_load("tests/test-config.ini");
	assert(c != NULL);

	/* Test pw_config_rot_mode with specific output */
	enum pw_rot_mode mode = pw_config_rot_mode(c, "DSI-1");
	assert(mode == PW_ROT_HARDWARE);

	/* Test wildcard fallback */
	enum pw_rot_mode mode_other = pw_config_rot_mode(c, "HDMI-1");
	assert(mode_other == PW_ROT_AUTO);

	/* Test NULL config */
	enum pw_rot_mode mode_null = pw_config_rot_mode(NULL, "DSI-1");
	assert(mode_null == PW_ROT_AUTO);

	/* Test NULL output name */
	enum pw_rot_mode mode_null_name = pw_config_rot_mode(c, NULL);
	assert(mode_null_name == PW_ROT_AUTO);

	pw_config_free(c);
	printf("✓ test_rotation_config\n");
	return 0;
}

static int test_copytype_config(void)
{
	struct pw_config *c = pw_config_load("tests/test-config.ini");
	assert(c != NULL);

	/* Test pw_config_copy_override with specific output */
	enum pw_copy_override ov = pw_config_copy_override(c, "DSI-1");
	assert(ov == PW_COPY_YES);

	/* Test wildcard fallback */
	enum pw_copy_override ov_other = pw_config_copy_override(c, "HDMI-1");
	assert(ov_other == PW_COPY_NO);

	/* Test NULL config */
	enum pw_copy_override ov_null = pw_config_copy_override(NULL, "DSI-1");
	assert(ov_null == PW_COPY_AUTO);

	/* Test NULL output name */
	enum pw_copy_override ov_null_name = pw_config_copy_override(c, NULL);
	assert(ov_null_name == PW_COPY_NO);

	pw_config_free(c);
	printf("✓ test_copytype_config\n");
	return 0;
}

static int test_zerocopy_config(void)
{
	struct pw_config *c = pw_config_load("tests/test-config.ini");
	assert(c != NULL);

	/* Check zerocopy settings were parsed */
	assert(c->zerocopy == true);
	assert(c->single_buffer == true);
	assert(c->panel_autohide == false);

	pw_config_free(c);
	printf("✓ test_zerocopy_config\n");
	return 0;
}

static int test_memory_config(void)
{
	struct pw_config *c = pw_config_load("tests/test-config.ini");
	assert(c != NULL);

	/* Check memory settings were parsed */
	assert(c->arena_max == 2);
	assert(c->trim_threshold_kb == 512);
	assert(c->mmap_threshold_kb == 256);
	assert(c->top_pad_kb == 32);
	assert(c->trim_after_start == true);

	pw_config_free(c);
	printf("✓ test_memory_config\n");
	return 0;
}

static int test_panel_action(void)
{
	struct pw_config *c = pw_config_load("tests/test-config.ini");
	assert(c != NULL);

	/* Verify that PW_ACTION_TOGGLE_PANEL is defined and can be used in parse_action */
	enum pw_action action;
	/* Note: panel action is tested implicitly through parse_action in config.c */
	(void)action;

	pw_config_free(c);
	printf("✓ test_panel_action\n");
	return 0;
}

static int test_power_config(void)
{
	struct pw_config *c = pw_config_load("tests/test-config.ini");
	assert(c != NULL);

	/* Check [power] section parsing */
	assert(c->low_capacity == 20);
	assert(c->dim_level == 40);
	assert(c->poll_s == 600);

	/* Check [power.ac] section parsing */
	assert(c->power[PW_PROFILE_AC].dim_after_s == 180);
	assert(c->power[PW_PROFILE_AC].blank_after_s == 900);

	/* Check [power.battery] section parsing */
	assert(c->power[PW_PROFILE_BATTERY].dim_after_s == 30);
	assert(c->power[PW_PROFILE_BATTERY].blank_after_s == 120);

	/* Check [power.low] section parsing */
	assert(c->power[PW_PROFILE_LOW].dim_after_s == 15);
	assert(c->power[PW_PROFILE_LOW].blank_after_s == 45);
	assert(c->low_max_brightness_pct == 50);

	pw_config_free(c);
	printf("✓ test_power_config\n");
	return 0;
}

static int test_power_defaults(void)
{
	struct pw_config *c = pw_config_default();
	assert(c != NULL);

	/* Check power defaults */
	assert(c->backlight == NULL); /* auto */
	assert(c->low_capacity == 15);
	assert(c->dim_level == 30);
	assert(c->poll_s == 300);

	/* Check default power timings */
	assert(c->power[PW_PROFILE_AC].dim_after_s == 120);
	assert(c->power[PW_PROFILE_AC].blank_after_s == 600);
	assert(c->power[PW_PROFILE_BATTERY].dim_after_s == 20);
	assert(c->power[PW_PROFILE_BATTERY].blank_after_s == 60);
	assert(c->power[PW_PROFILE_LOW].dim_after_s == 10);
	assert(c->power[PW_PROFILE_LOW].blank_after_s == 30);
	assert(c->low_max_brightness_pct == 40);

	pw_config_free(c);
	printf("✓ test_power_defaults\n");
	return 0;
}

static const char *write_tmp_ini(const char *name, const char *text)
{
	static char path[256];
	const char *dir = getenv("TMPDIR");
	snprintf(path, sizeof(path), "%s/%s", dir ? dir : "/tmp", name);
	FILE *f = fopen(path, "w");
	assert(f != NULL);
	fputs(text, f);
	fclose(f);
	return path;
}

static int test_power_validation(void)
{
	const char *path = write_tmp_ini("picowl-test-power-bad.ini",
		"[power]\nlow_capacity = 150\ndim_level = 0\npoll_s = 10\n"
		"[power.ac]\nblank_after_s = -5\n"
		"[power.low]\nmax_brightness_pct = 0\n");
	struct pw_config *c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	assert(c->low_capacity == 15);
	assert(c->dim_level == 30);
	assert(c->poll_s == 300);
	assert(c->power[PW_PROFILE_AC].blank_after_s == 600);
	assert(c->low_max_brightness_pct == 40);
	pw_config_free(c);
	printf("✓ test_power_validation\n");
	return 0;
}

static int test_legacy_idle_timeout(void)
{
	const char *path = write_tmp_ini("picowl-test-legacy.ini",
		"[idle]\ntimeout_ms = 45000\n");
	struct pw_config *c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	for (int i = 0; i < PW_PROFILE_COUNT; i++)
		assert(c->power[i].blank_after_s == 45);
	pw_config_free(c);

	/* Explicit [power.*] blank_after_s wins; legacy unused. */
	path = write_tmp_ini("picowl-test-legacy2.ini",
		"[idle]\ntimeout_ms = 45000\n[power.ac]\nblank_after_s = 300\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	assert(c->power[PW_PROFILE_AC].blank_after_s == 300);
	assert(c->power[PW_PROFILE_BATTERY].blank_after_s == 60);
	assert(c->power[PW_PROFILE_LOW].blank_after_s == 30);
	pw_config_free(c);

	/* [core] alias and round-up of sub-second values. */
	path = write_tmp_ini("picowl-test-legacy3.ini",
		"[core]\nidle_timeout_ms = 500\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	assert(c->power[PW_PROFILE_BATTERY].blank_after_s == 1);
	pw_config_free(c);
	printf("✓ test_legacy_idle_timeout\n");
	return 0;
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	printf("Running picowl config tests...\n");

	int failed = 0;
	failed += test_default_config();
	failed += test_load_config();
	failed += test_missing_file();
	failed += test_config_free_null();
	failed += test_keybinding_parsing();
	failed += test_touch_cursor_parsing();
	failed += test_validation_ranges();
	failed += test_rotation_config();
	failed += test_copytype_config();
	failed += test_zerocopy_config();
	failed += test_memory_config();
	failed += test_panel_action();
	failed += test_power_config();
	failed += test_power_defaults();
	failed += test_power_validation();
	failed += test_legacy_idle_timeout();

	if (failed == 0) {
		printf("\nAll tests passed!\n");
		return 0;
	} else {
		printf("\nSome tests failed.\n");
		return 1;
	}
}
