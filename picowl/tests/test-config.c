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

	if (failed == 0) {
		printf("\nAll tests passed!\n");
		return 0;
	} else {
		printf("\nSome tests failed.\n");
		return 1;
	}
}
