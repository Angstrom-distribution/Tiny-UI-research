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
	/* the only rules are the built-in ones for the layer surfaces of the
	 * keyboard and the panel */
	assert(!wl_list_empty(&c->app_rules));
	int builtin_n = 0;
	bool have_wvkbd = false, have_panel = false;
	struct pw_app_rule *builtin;
	wl_list_for_each(builtin, &c->app_rules, link) {
		assert(builtin->kind == PW_RULE_LAYER);
		have_wvkbd |= strcmp(builtin->name, "wvkbd") == 0;
		have_panel |= strcmp(builtin->name, "panel") == 0;
		builtin_n++;
	}
	assert(builtin_n == 2 && have_wvkbd && have_panel);

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
	assert(!c->have_calibration);
	assert(c->pointercal && strcmp(c->pointercal, "/etc/pointercal") == 0);
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

static int test_app_rules(void)
{
	struct pw_config *c = pw_config_load("tests/test-config.ini");
	assert(c != NULL);

	struct pw_hold_params hp;
	bool hit;

	/* mediaplayer: hold_action = none, slop_px = 12 (merged from two sections)
	 * order independence test: [app.mediaplayer] appears before [touch] */
	hit = pw_config_hold(c, PW_RULE_APP, "mediaplayer", &hp);
	assert(hit == true);
	assert(hp.action == PW_HOLD_NONE);
	assert(hp.delay_ms == 300);  /* inherited from [touch] */
	assert(hp.hold_ms == 900);   /* inherited from [touch] */
	assert(hp.slop_px == 12);

	/* org.example.Viewer: hold_ms = 1200 */
	hit = pw_config_hold(c, PW_RULE_APP, "org.example.Viewer", &hp);
	assert(hit == true);
	assert(hp.action == PW_HOLD_RIGHT_CLICK);  /* inherited from [touch] */
	assert(hp.delay_ms == 300);
	assert(hp.hold_ms == 1200);
	assert(hp.slop_px == 8);

	/* layer.panel: slop_px = 4, merged over the built-in rule, which keeps
	 * the hold off */
	hit = pw_config_hold(c, PW_RULE_LAYER, "panel", &hp);
	assert(hit == true);
	assert(hp.action == PW_HOLD_NONE);
	assert(hp.delay_ms == 300);
	assert(hp.hold_ms == 900);
	assert(hp.slop_px == 4);

	/* bad: invalid hold_action is inherited; hold_ms <= delay reverts timings */
	hit = pw_config_hold(c, PW_RULE_APP, "bad", &hp);
	assert(hit == true);
	assert(hp.action == PW_HOLD_RIGHT_CLICK);
	assert(hp.delay_ms == 300);
	assert(hp.hold_ms == 900);
	assert(hp.slop_px == 8);

	/* nonebad: bad slop reverts the timings but keeps the explicit action */
	hit = pw_config_hold(c, PW_RULE_APP, "nonebad", &hp);
	assert(hit == true);
	assert(hp.action == PW_HOLD_NONE);
	assert(hp.delay_ms == 300);
	assert(hp.hold_ms == 900);
	assert(hp.slop_px == 8);

	/* empty [app.] and [layer.] are ignored: five rules plus the built-in wvkbd one
	 * (the file's [layer.panel] merges into the built-in panel rule), none unnamed */
	int n = 0;
	struct pw_app_rule *rule;
	wl_list_for_each(rule, &c->app_rules, link) {
		assert(rule->name && rule->name[0]);
		n++;
	}
	assert(n == 6);

	/* panel as app (no rule): returns globals with false */
	hit = pw_config_hold(c, PW_RULE_APP, "panel", &hp);
	assert(hit == false);
	assert(hp.action == PW_HOLD_RIGHT_CLICK);
	assert(hp.delay_ms == 300);
	assert(hp.hold_ms == 900);
	assert(hp.slop_px == 8);

	/* Nonexistent app: returns globals with false */
	hit = pw_config_hold(c, PW_RULE_APP, "nonexistent", &hp);
	assert(hit == false);
	assert(hp.action == PW_HOLD_RIGHT_CLICK);

	/* NULL name: returns globals with false */
	hit = pw_config_hold(c, PW_RULE_APP, NULL, &hp);
	assert(hit == false);
	assert(hp.action == PW_HOLD_RIGHT_CLICK);

	/* Case sensitivity: MediaPlayer != mediaplayer */
	hit = pw_config_hold(c, PW_RULE_APP, "MediaPlayer", &hp);
	assert(hit == false);

	pw_config_free(c);
	printf("✓ test_app_rules\n");
	return 0;
}

static int test_app_rules_inherit(void)
{
	/* [touch] is non-default and comes after (and before) the rules */
	struct pw_config *c = pw_config_load("tests/test-config-hold.ini");
	assert(c != NULL);

	struct pw_hold_params hp;

	assert(pw_config_hold(c, PW_RULE_APP, "early", &hp));
	assert(hp.action == PW_HOLD_NONE);
	assert(hp.delay_ms == 200 && hp.hold_ms == 1200 && hp.slop_px == 10);

	assert(pw_config_hold(c, PW_RULE_APP, "late", &hp));
	assert(hp.action == PW_HOLD_NONE);
	assert(hp.delay_ms == 200 && hp.hold_ms == 700 && hp.slop_px == 20);

	assert(pw_config_hold(c, PW_RULE_LAYER, "osk", &hp));
	assert(hp.action == PW_HOLD_NONE);
	assert(hp.delay_ms == 100 && hp.hold_ms == 700 && hp.slop_px == 10);

	/* unset hold_ms inherits; a bad one (<= delay) reverts to [touch] */
	assert(pw_config_hold(c, PW_RULE_APP, "unset", &hp));
	assert(hp.action == PW_HOLD_NONE);
	assert(hp.delay_ms == 200 && hp.hold_ms == 700 && hp.slop_px == 10);

	/* bad timings keep the rule's own action (differs from [touch]) */
	assert(pw_config_hold(c, PW_RULE_APP, "explicit", &hp));
	assert(hp.action == PW_HOLD_RIGHT_CLICK);
	assert(hp.delay_ms == 200 && hp.hold_ms == 700 && hp.slop_px == 10);

	/* hold_ms == hold_delay_ms is rejected like in [touch] */
	assert(pw_config_hold(c, PW_RULE_APP, "equal", &hp));
	assert(hp.action == PW_HOLD_NONE);
	assert(hp.delay_ms == 200 && hp.hold_ms == 700);

	pw_config_free(c);
	printf("✓ test_app_rules_inherit\n");
	return 0;
}

static int test_hold_button(void)
{
	struct pw_hold_params hp;
	struct pw_config *c = pw_config_default();

	assert(c != NULL);
	assert(c->hold_button == PW_HOLD_BTN_RIGHT);
	assert(!pw_config_hold(c, PW_RULE_APP, "havoc", &hp));
	assert(hp.button == PW_HOLD_BTN_RIGHT);
	pw_config_free(c);

	/* [touch] is middle and comes after the rules */
	c = pw_config_load("tests/test-config-button.ini");
	assert(c != NULL);
	assert(c->hold_button == PW_HOLD_BTN_MIDDLE);
	assert(pw_config_hold(c, PW_RULE_APP, "havoc", &hp));
	assert(hp.button == PW_HOLD_BTN_MIDDLE);
	/* unset in the rule inherits [touch], also for layers */
	assert(pw_config_hold(c, PW_RULE_APP, "inherit", &hp));
	assert(hp.button == PW_HOLD_BTN_MIDDLE);
	assert(pw_config_hold(c, PW_RULE_LAYER, "plain", &hp));
	assert(hp.button == PW_HOLD_BTN_MIDDLE);
	assert(pw_config_hold(c, PW_RULE_LAYER, "osk", &hp));
	assert(hp.button == PW_HOLD_BTN_MIDDLE);
	/* an explicit right overrides a middle [touch] */
	assert(pw_config_hold(c, PW_RULE_APP, "back", &hp));
	assert(hp.button == PW_HOLD_BTN_RIGHT);
	/* an unknown value is ignored, so the rule inherits */
	assert(pw_config_hold(c, PW_RULE_APP, "badvalue", &hp));
	assert(hp.button == PW_HOLD_BTN_MIDDLE);
	/* bad timings revert the timings only */
	assert(pw_config_hold(c, PW_RULE_APP, "badtiming", &hp));
	assert(hp.button == PW_HOLD_BTN_RIGHT);
	assert(hp.hold_ms == 900);
	/* no rule: the [touch] value */
	assert(!pw_config_hold(c, PW_RULE_APP, "other", &hp));
	assert(hp.button == PW_HOLD_BTN_MIDDLE);
	pw_config_free(c);

	/* a bad [touch] value keeps the default; a rule still applies */
	c = pw_config_load("tests/test-config-button-bad.ini");
	assert(c != NULL);
	assert(c->hold_button == PW_HOLD_BTN_RIGHT);
	assert(pw_config_hold(c, PW_RULE_APP, "havoc", &hp));
	assert(hp.button == PW_HOLD_BTN_MIDDLE);
	assert(!pw_config_hold(c, PW_RULE_APP, "other", &hp));
	assert(hp.button == PW_HOLD_BTN_RIGHT);
	pw_config_free(c);

	printf("✓ test_hold_button\n");
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

	/* limits default to the former compile-time constants */
	assert(c->zb_max_buffers == 3);
	assert(c->zb_budget_kb == 2048);
	assert(c->zb_total_kb == 0);
	assert(c->zb_n_pools == 1);
	assert(c->caching_override == PW_CACHING_OV_AUTO);

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
	assert(!c->power[PW_PROFILE_BATTERY].inhibit);
	assert(c->power[PW_PROFILE_AC].inhibit); /* unset keeps the default */

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
	for (int i = 0; i < PW_PROFILE_COUNT; i++)
		assert(c->power[i].inhibit);

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
		"[power.low]\nmax_brightness_pct = 0\ninhibit = maybe\n");
	struct pw_config *c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	assert(c->low_capacity == 15);
	assert(c->dim_level == 30);
	assert(c->poll_s == 300);
	assert(c->power[PW_PROFILE_AC].blank_after_s == 600);
	assert(c->low_max_brightness_pct == 40);
	assert(c->power[PW_PROFILE_LOW].inhibit); /* typo keeps the default */
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

static int test_zerocopy_limits(void)
{
	struct pw_config *d = pw_config_default();
	assert(d != NULL);
	assert(d->zb_max_buffers == 3 && d->zb_budget_kb == 2048);
	assert(d->zb_total_kb == 0 && d->zb_n_pools == 1);
	assert(pw_config_app(d, "mediaplayer") == NULL);
	pw_config_free(d);

	struct pw_config *c = pw_config_load("tests/test-config-zb.ini");
	assert(c != NULL);
	assert(c->zb_max_buffers == 5);
	assert(c->zb_budget_kb == 1024);
	assert(c->zb_total_kb == 3072);

	/* two [app.mediaplayer] sections merge into one rule */
	int n = 0;
	struct pw_app_rule *r;
	wl_list_for_each(r, &c->app_rules, link)
		if (r->kind == PW_RULE_APP && strcmp(r->name, "mediaplayer") == 0)
			n++;
	assert(n == 1);
	const struct pw_app_rule *a = pw_config_app(c, "mediaplayer");
	assert(a && a->zb_buffers == 7 && a->zb_budget_kb == 4200 && a->zb_pool == 1);
	/* exe is resolved: no "..", and it names the same file */
	assert(a->exe && a->exe[0] == '/' && !strstr(a->exe, "..") &&
		strcmp(a->exe, "/bin/../bin/sh") != 0);

	/* dotted app_id; a budget of 0 is a pool which holds nothing */
	a = pw_config_app(c, "org.example.Player");
	assert(a && a->zb_buffers == 2 && a->zb_budget_kb == 0 && a->zb_pool == 2);
	assert(a->exe == NULL);

	/* only budget given: count inherits; exe which cannot be resolved stays literal */
	a = pw_config_app(c, "noexe");
	assert(a && a->zb_buffers == -1 && a->zb_budget_kb == 500 && a->zb_pool == 3);
	assert(a->exe && strcmp(a->exe, "/nonexistent/dir/player") == 0);

	/* no zerocopy keys: inherits everything, no pool */
	a = pw_config_app(c, "holdonly");
	assert(a && a->zb_buffers == -1 && a->zb_budget_kb == -1 && a->zb_pool == 0);

	/* out of range values are dropped, which leaves no pool */
	a = pw_config_app(c, "bad");
	assert(a && a->zb_buffers == -1 && a->zb_budget_kb == -1 && a->zb_pool == 0);

	/* [layer.*] has no buffer pool */
	wl_list_for_each(r, &c->app_rules, link)
		if (r->kind == PW_RULE_LAYER) {
			assert(strcmp(r->name, "osk") == 0 || strcmp(r->name, "wvkbd") == 0 ||
				strcmp(r->name, "panel") == 0);
			assert(r->zb_buffers == -1 && r->zb_pool == 0);
		}
	assert(pw_config_app(c, "osk") == NULL);

	/* default pool + mediaplayer, Player, noexe */
	assert(c->zb_n_pools == 4);

	/* exact, case-sensitive, NULL-safe */
	assert(pw_config_app(c, "MediaPlayer") == NULL);
	assert(pw_config_app(c, "nope") == NULL);
	assert(pw_config_app(c, NULL) == NULL);
	assert(pw_config_app(NULL, "mediaplayer") == NULL);
	pw_config_free(c);

	/* bad globals keep the defaults */
	const char *path = write_tmp_ini("picowl-test-zblim.ini",
		"[zerocopy]\nmax_buffers_per_client = 0\nbudget_kb = 70000\ntotal_kb = -1\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	assert(c->zb_max_buffers == 3 && c->zb_budget_kb == 2048 && c->zb_total_kb == 0);
	pw_config_free(c);

	path = write_tmp_ini("picowl-test-zblim2.ini",
		"[zerocopy]\nmax_buffers_per_client = 33\nbudget_kb = 0\ntotal_kb = 65536\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	assert(c->zb_max_buffers == 3 && c->zb_budget_kb == 0 && c->zb_total_kb == 65536);
	pw_config_free(c);

	/* [zerocopy] caching: case-insensitive, an invalid value keeps auto */
	path = write_tmp_ini("picowl-test-zbcache.ini", "[zerocopy]\ncaching = Cacheable\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL && c->caching_override == PW_CACHING_OV_CACHEABLE);
	pw_config_free(c);

	path = write_tmp_ini("picowl-test-zbcache2.ini", "[zerocopy]\ncaching = WRITE_COMBINED\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL && c->caching_override == PW_CACHING_OV_WC);
	pw_config_free(c);

	path = write_tmp_ini("picowl-test-zbcache3.ini",
		"[zerocopy]\ncaching = cacheable\ncaching = wc\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL && c->caching_override == PW_CACHING_OV_AUTO);
	pw_config_free(c);

	printf("✓ test_zerocopy_limits\n");
	return 0;
}

static int test_lease_config(void)
{
	/* Defaults: leasing on, only the media player may lease. */
	struct pw_config *c = pw_config_default();
	assert(c != NULL);
	assert(c->lease_enable == true);
	assert(c->lease_allow && strcmp(c->lease_allow, "mediaplayer") == 0);
	pw_config_free(c);

	c = pw_config_load("tests/test-config.ini");
	assert(c != NULL);
	assert(c->lease_enable == false);
	assert(strcmp(c->lease_allow, "mediaplayer, vlc ,foot") == 0);
	pw_config_free(c);

	/* An empty list is kept (rejects every request); a bad boolean and an
	 * unknown key leave the defaults. */
	const char *path = write_tmp_ini("picowl-test-lease.ini",
		"[lease]\nallow =\nenable = maybe\nbogus = 1\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	assert(c->lease_enable == true);
	assert(c->lease_allow && c->lease_allow[0] == '\0');
	pw_config_free(c);

	path = write_tmp_ini("picowl-test-lease2.ini", "[lease]\nallow = *\nenable = yes\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	assert(c->lease_enable == true);
	assert(strcmp(c->lease_allow, "*") == 0);
	pw_config_free(c);
	printf("✓ test_lease_config\n");
	return 0;
}

static int test_capture_config(void)
{
	/* Off unless asked for: any client could read the screen. */
	struct pw_config *c = pw_config_default();
	assert(c != NULL && c->capture_enable == false);
	pw_config_free(c);

	const char *path = write_tmp_ini("picowl-test-capture.ini",
		"[capture]\nenabled = true\nbogus = 1\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL && c->capture_enable == true);
	pw_config_free(c);

	path = write_tmp_ini("picowl-test-capture2.ini", "[capture]\nenabled = maybe\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL && c->capture_enable == false);
	pw_config_free(c);
	printf("✓ test_capture_config\n");
	return 0;
}

static int test_touch_calibration(void)
{
	struct pw_config *c = pw_config_load("tests/test-config-cal.ini");
	assert(c);
	assert(c->have_calibration);
	assert(c->calibration[0] == 1.487044f && c->calibration[5] == 1.203639f);
	assert(strcmp(c->pointercal, "/run/pointercal") == 0);
	pw_config_free(c);

	/* five numbers: ignored, the other key still applies */
	c = pw_config_load("tests/test-config-calbad.ini");
	assert(c);
	assert(!c->have_calibration);
	assert(strcmp(c->pointercal, "/etc/other-pointercal") == 0);
	pw_config_free(c);

	/* seven numbers: ignored */
	c = pw_config_load("tests/test-config-calbad2.ini");
	assert(c);
	assert(!c->have_calibration);
	pw_config_free(c);

	printf("✓ test_touch_calibration\n");
	return 0;
}

static int test_layout_config(void)
{
	/* off by default */
	struct pw_config *c = pw_config_default();
	assert(c != NULL && c->n_stack == 0 && c->stack[0] == NULL);
	pw_config_free(c);

	c = pw_config_load("tests/test-config-layout.ini");
	assert(c != NULL);
	/* trimmed, in order, the third name ignored */
	assert(c->n_stack == 2);
	assert(strcmp(c->stack[0], "mediaplayer") == 0);
	assert(strcmp(c->stack[1], "havoc") == 0);

	const struct pw_app_rule *a = pw_config_app(c, "mediaplayer");
	assert(a && a->aspect_w == 4 && a->aspect_h == 3);
	a = pw_config_app(c, "havoc");
	assert(a && a->aspect_w == 0 && a->aspect_h == 0);
	const char *bad[] = { "bad.x", "bad.big", "bad.three", "bad.neg", "bad.space", "bad.empty" };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		a = pw_config_app(c, bad[i]);
		assert(a && a->aspect_w == 0 && a->aspect_h == 0);
	}
	a = pw_config_app(c, "max");
	assert(a && a->aspect_w == 4096 && a->aspect_h == 1);
	a = pw_config_app(c, "min");
	assert(a && a->aspect_w == 1 && a->aspect_h == 1);
	a = pw_config_app(c, "twice");
	assert(a && a->aspect_w == 3 && a->aspect_h == 2);
	/* [layer.*] has no aspect */
	struct pw_app_rule *r;
	wl_list_for_each(r, &c->app_rules, link)
		if (r->kind == PW_RULE_LAYER)
			assert(r->aspect_w == 0 && r->aspect_h == 0);
	pw_config_free(c);

	/* a stack which is not two distinct names leaves tiling off */
	const char *texts[] = {
		"[layout]\nstack = onlyone\n",
		"[layout]\nstack = same, same\n",
		"[layout]\nstack =\n",
		"[layout]\nstack = , ,\n",
	};
	for (unsigned i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
		const char *path = write_tmp_ini("picowl-test-layout.ini", texts[i]);
		c = pw_config_load(path);
		remove(path);
		assert(c != NULL && c->n_stack == 0);
		assert(c->stack[0] == NULL && c->stack[1] == NULL);
		pw_config_free(c);
	}

	/* a duplicate in a longer list is skipped, the next name takes its place;
	 * a later stack replaces an earlier one */
	const char *path = write_tmp_ini("picowl-test-layout.ini",
		"[layout]\nstack = a, a, b\n[layout]\nstack = c, d\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL && c->n_stack == 2);
	assert(strcmp(c->stack[0], "c") == 0 && strcmp(c->stack[1], "d") == 0);
	pw_config_free(c);
	path = write_tmp_ini("picowl-test-layout.ini", "[layout]\nstack = a, a, b\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL && c->n_stack == 2);
	assert(strcmp(c->stack[0], "a") == 0 && strcmp(c->stack[1], "b") == 0);
	pw_config_free(c);

	/* [layout] pan: the keyboard by default, a list, or nothing */
	c = pw_config_default();
	assert(c != NULL && c->n_pan == 1);
	assert(pw_config_pan_match(c, "wvkbd"));
	assert(!pw_config_pan_match(c, "wvkbd2") && !pw_config_pan_match(c, "wvk"));
	assert(!pw_config_pan_match(c, "WVKBD") && !pw_config_pan_match(c, ""));
	assert(!pw_config_pan_match(c, NULL) && !pw_config_pan_match(NULL, "wvkbd"));
	pw_config_free(c);

	struct { const char *text; int n; const char *first; const char *last; } pan_ok[] = {
		{ "[layout]\npan = squeekboard\n", 1, "squeekboard", "squeekboard" },
		{ "[layout]\npan =  wvkbd , onboard ,third\n", 3, "wvkbd", "third" },
		{ "[layout]\npan =\n", 0, NULL, NULL },
		{ "[layout]\npan =   \n", 0, NULL, NULL },
		/* a later list replaces an earlier one */
		{ "[layout]\npan = a, b\n[layout]\npan = c\n", 1, "c", "c" },
		{ "[layout]\npan = a, b\n[layout]\npan =\n", 0, NULL, NULL },
		/* a rejected value keeps what was there */
		{ "[layout]\npan = a\npan = x,,y\n", 1, "a", "a" },
		{ "[layout]\npan = x y\n", 1, "wvkbd", "wvkbd" },
		{ "[layout]\npan = a,\n", 1, "wvkbd", "wvkbd" },
		{ "[layout]\npan = ,a\n", 1, "wvkbd", "wvkbd" },
		{ "[layout]\npan = ,\n", 1, "wvkbd", "wvkbd" },
		{ "[layout]\npan = a,b,c,d,e,f,g,h,i\n", 1, "wvkbd", "wvkbd" },
		{ "[layout]\npan = a,b,c,d,e,f,g,h\n", 8, "a", "h" },
		{ "[layout]\npan = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n",
			1, "wvkbd", "wvkbd" },
	};
	for (unsigned i = 0; i < sizeof(pan_ok) / sizeof(pan_ok[0]); i++) {
		path = write_tmp_ini("picowl-test-layout.ini", pan_ok[i].text);
		c = pw_config_load(path);
		remove(path);
		if (!c || c->n_pan != pan_ok[i].n)
			fprintf(stderr, "pan case %u: n_pan %d expected %d\n", i,
				c ? c->n_pan : -1, pan_ok[i].n);
		assert(c != NULL && c->n_pan == pan_ok[i].n);
		if (pan_ok[i].n) {
			assert(strcmp(c->pan[0], pan_ok[i].first) == 0);
			assert(strcmp(c->pan[c->n_pan - 1], pan_ok[i].last) == 0);
		}
		pw_config_free(c);
	}
	path = write_tmp_ini("picowl-test-layout.ini", "[layout]\npan = a, b\n");
	c = pw_config_load(path);
	remove(path);
	assert(c->n_pan == 2 && pw_config_pan_match(c, "b") && !pw_config_pan_match(c, "wvkbd"));
	pw_config_free(c);

	/* [layout] focus: off by default, one of the two stack names otherwise */
	c = pw_config_default();
	assert(c->focus == NULL);
	pw_config_free(c);
	struct { const char *text; const char *want; } focus_cases[] = {
		{ "[layout]\nstack = a, b\nfocus = b\n", "b" },
		{ "[layout]\nstack = a, b\nfocus =  a \n", "a" },
		{ "[layout]\nfocus = a\nstack = a, b\n", "a" },	/* order of the keys */
		{ "[layout]\nstack = a, b\nfocus = a\nfocus = b\n", "b" },
		{ "[layout]\nstack = a, b\nfocus = a\nfocus =\n", NULL },
		{ "[layout]\nstack = a, b\nfocus = c\n", NULL },
		{ "[layout]\nstack = a, b\nfocus = A\n", NULL },
		{ "[layout]\nstack = a, b\nfocus = a, b\n", NULL },
		{ "[layout]\nfocus = a\n", NULL },	/* no stack, no tiling */
		{ "[layout]\nstack = a\nfocus = a\n", NULL },
		{ "[layout]\nstack = a, b\nfocus = a\nstack = c, d\n", NULL },
	};
	for (unsigned i = 0; i < sizeof(focus_cases) / sizeof(focus_cases[0]); i++) {
		path = write_tmp_ini("picowl-test-layout.ini", focus_cases[i].text);
		c = pw_config_load(path);
		remove(path);
		if (!c || (focus_cases[i].want ? !c->focus || strcmp(c->focus, focus_cases[i].want)
				: c->focus != NULL))
			fprintf(stderr, "focus case %u failed\n", i);
		assert(c != NULL);
		assert(focus_cases[i].want ? c->focus && strcmp(c->focus, focus_cases[i].want) == 0
			: c->focus == NULL);
		pw_config_free(c);
	}

	printf("✓ test_layout_config\n");
	return 0;
}

/* The binding for the first key with this evdev code, or NULL. */
static const struct pw_keybinding *find_code(const struct pw_config *c, uint32_t code)
{
	const struct pw_keybinding *kb;
	wl_list_for_each(kb, &c->keybindings, link)
		if (!kb->key_name && kb->keycode == code)
			return kb;
	return NULL;
}

static int test_osk_config(void)
{
	struct pw_config *c = pw_config_default();
	struct pw_hold_params hp;
	const struct pw_keybinding *kb;

	/* defaults: no keyboard to supervise, restart on, no hold on its surface */
	assert(c != NULL);
	assert(c->osk_cmd == NULL);
	assert(c->osk_restart == true);
	assert(pw_config_hold(c, PW_RULE_LAYER, "wvkbd", &hp));
	assert(hp.action == PW_HOLD_NONE);
	assert(hp.delay_ms == 300 && hp.hold_ms == 900 && hp.slop_px == 8);
	/* the rule is for the layer namespace only */
	assert(!pw_config_hold(c, PW_RULE_APP, "wvkbd", &hp));
	assert(hp.action == PW_HOLD_RIGHT_CLICK);
	/* the panel's sliders are dragged: the press is not deferred either */
	assert(pw_config_hold(c, PW_RULE_LAYER, "panel", &hp));
	assert(hp.action == PW_HOLD_NONE);
	assert(hp.delay_ms == 300 && hp.hold_ms == 900 && hp.slop_px == 8);
	assert(!pw_config_hold(c, PW_RULE_APP, "panel", &hp));
	assert(hp.action == PW_HOLD_RIGHT_CLICK);
	pw_config_free(c);

	const char *path = write_tmp_ini("picowl-test-osk.ini",
		"[osk]\n"
		"cmd = /usr/bin/wvkbd-ipaq --hidden --auto\n"
		"restart = no\n"
		"[keybindings]\n"
		"code:397 = osk toggle\n"
		"code:398 = osk show\n"
		"code:399 = osk hide\n"
		"code:400 = osk\n"
		"code:401 = osk   hide  \n"
		"code:402 = osk sideways\n"
		"code:403 = spawn osk show\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	assert(c->osk_cmd && strcmp(c->osk_cmd, "/usr/bin/wvkbd-ipaq --hidden --auto") == 0);
	assert(c->osk_restart == false);
	kb = find_code(c, 397);
	assert(kb && kb->action == PW_ACTION_OSK && kb->osk_op == PW_OSK_TOGGLE);
	assert(kb->command == NULL);
	kb = find_code(c, 398);
	assert(kb && kb->action == PW_ACTION_OSK && kb->osk_op == PW_OSK_SHOW);
	kb = find_code(c, 399);
	assert(kb && kb->action == PW_ACTION_OSK && kb->osk_op == PW_OSK_HIDE);
	/* no argument means toggle; surrounding blanks are trimmed */
	kb = find_code(c, 400);
	assert(kb && kb->action == PW_ACTION_OSK && kb->osk_op == PW_OSK_TOGGLE);
	kb = find_code(c, 401);
	assert(kb && kb->action == PW_ACTION_OSK && kb->osk_op == PW_OSK_HIDE);
	/* an unknown argument drops the binding; spawn keeps its text as is */
	assert(find_code(c, 402) == NULL);
	kb = find_code(c, 403);
	assert(kb && kb->action == PW_ACTION_SPAWN && strcmp(kb->command, "osk show") == 0);
	pw_config_free(c);

	/* an empty cmd disables; a later one wins; a bad restart keeps the default */
	path = write_tmp_ini("picowl-test-osk.ini",
		"[osk]\ncmd = first\ncmd =\nrestart = maybe\n");
	c = pw_config_load(path);
	assert(c != NULL && c->osk_cmd == NULL && c->osk_restart == true);
	pw_config_free(c);
	path = write_tmp_ini("picowl-test-osk.ini", "[osk]\ncmd = first\ncmd = second\nrestart = yes\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL && strcmp(c->osk_cmd, "second") == 0 && c->osk_restart == true);
	pw_config_free(c);

	/* a user [layer.wvkbd] merges over the built-in rule, before or after
	 * [touch], and can bring the hold back */
	path = write_tmp_ini("picowl-test-osk.ini",
		"[layer.wvkbd]\nslop_px = 20\n"
		"[touch]\nhold_ms = 1200\nhold_button = middle\n");
	c = pw_config_load(path);
	assert(c != NULL);
	assert(pw_config_hold(c, PW_RULE_LAYER, "wvkbd", &hp));
	assert(hp.action == PW_HOLD_NONE && hp.slop_px == 20);
	assert(hp.hold_ms == 1200 && hp.button == PW_HOLD_BTN_MIDDLE);
	pw_config_free(c);
	path = write_tmp_ini("picowl-test-osk.ini",
		"[layer.wvkbd]\nhold_action = right-click\nhold_ms = 700\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	assert(pw_config_hold(c, PW_RULE_LAYER, "wvkbd", &hp));
	assert(hp.action == PW_HOLD_RIGHT_CLICK && hp.hold_ms == 700);
	/* still one rule for it, not two */
	int n = 0;
	struct pw_app_rule *rule;
	wl_list_for_each(rule, &c->app_rules, link)
		if (rule->kind == PW_RULE_LAYER && strcmp(rule->name, "wvkbd") == 0)
			n++;
	assert(n == 1);
	pw_config_free(c);

	/* the same for the panel's built-in rule: a [layer.panel] of the user
	 * merges over it, [touch] values reach it, and it stays one rule */
	path = write_tmp_ini("picowl-test-osk.ini",
		"[layer.panel]\nslop_px = 20\n"
		"[touch]\nhold_ms = 1200\n");
	c = pw_config_load(path);
	assert(c != NULL);
	assert(pw_config_hold(c, PW_RULE_LAYER, "panel", &hp));
	assert(hp.action == PW_HOLD_NONE && hp.slop_px == 20 && hp.hold_ms == 1200);
	pw_config_free(c);
	path = write_tmp_ini("picowl-test-osk.ini",
		"[layer.panel]\nhold_action = right-click\n");
	c = pw_config_load(path);
	remove(path);
	assert(c != NULL);
	assert(pw_config_hold(c, PW_RULE_LAYER, "panel", &hp));
	assert(hp.action == PW_HOLD_RIGHT_CLICK);
	n = 0;
	wl_list_for_each(rule, &c->app_rules, link)
		if (rule->kind == PW_RULE_LAYER && strcmp(rule->name, "panel") == 0)
			n++;
	assert(n == 1);
	pw_config_free(c);

	printf("✓ test_osk_config\n");
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
	failed += test_app_rules();
	failed += test_app_rules_inherit();
	failed += test_hold_button();
	failed += test_osk_config();
	failed += test_validation_ranges();
	failed += test_rotation_config();
	failed += test_copytype_config();
	failed += test_zerocopy_config();
	failed += test_zerocopy_limits();
	failed += test_memory_config();
	failed += test_panel_action();
	failed += test_power_config();
	failed += test_power_defaults();
	failed += test_power_validation();
	failed += test_legacy_idle_timeout();
	failed += test_lease_config();
	failed += test_capture_config();
	failed += test_touch_calibration();
	failed += test_layout_config();

	if (failed == 0) {
		printf("\nAll tests passed!\n");
		return 0;
	} else {
		printf("\nSome tests failed.\n");
		return 1;
	}
}
