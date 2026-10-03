/* config.c - INI configuration parser. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <wayland-util.h>
#include <drm_fourcc.h>
#include "picowl.h"


/* --- Helpers ----------------------------------------------------------- */

static char *trim(char *str)
{
	while (*str && isspace(*str)) str++;
	size_t len = strlen(str);
	while (len > 0 && isspace(str[len-1])) str[--len] = '\0';
	return str;
}

static bool parse_color(const char *str, float *rgba)
{
	if (str[0] != '#' || strlen(str) != 7)
		return false;
	uint32_t color;
	if (sscanf(str, "#%6x", &color) != 1)
		return false;
	rgba[0] = ((color >> 16) & 0xff) / 255.0f;
	rgba[1] = ((color >> 8) & 0xff) / 255.0f;
	rgba[2] = (color & 0xff) / 255.0f;
	rgba[3] = 1.0f;
	return true;
}

static uint32_t parse_render_format(const char *str)
{
	if (strcmp(str, "RGB565") == 0)
		return DRM_FORMAT_RGB565;
	if (strcmp(str, "XRGB8888") == 0)
		return DRM_FORMAT_XRGB8888;
	if (strcmp(str, "ARGB8888") == 0)
		return DRM_FORMAT_ARGB8888;
	pw_log(WLR_INFO, "Unknown render format: %s", str);
	return 0; /* Invalid */
}

static enum wl_output_transform parse_transform(const char *str)
{
	if (strcmp(str, "normal") == 0)
		return WL_OUTPUT_TRANSFORM_NORMAL;
	if (strcmp(str, "90") == 0)
		return WL_OUTPUT_TRANSFORM_90;
	if (strcmp(str, "180") == 0)
		return WL_OUTPUT_TRANSFORM_180;
	if (strcmp(str, "270") == 0)
		return WL_OUTPUT_TRANSFORM_270;
	if (strcmp(str, "flipped") == 0)
		return WL_OUTPUT_TRANSFORM_FLIPPED;
	if (strcmp(str, "flipped-90") == 0)
		return WL_OUTPUT_TRANSFORM_FLIPPED_90;
	if (strcmp(str, "flipped-180") == 0)
		return WL_OUTPUT_TRANSFORM_FLIPPED_180;
	if (strcmp(str, "flipped-270") == 0)
		return WL_OUTPUT_TRANSFORM_FLIPPED_270;
	pw_log(WLR_INFO, "Unknown transform: %s", str);
	return WL_OUTPUT_TRANSFORM_NORMAL;
}

static bool parse_action(const char *str, enum pw_action *action)
{
	if (strcmp(str, "spawn") == 0) {
		*action = PW_ACTION_SPAWN;
		return true;
	}
	if (strcmp(str, "cycle") == 0) {
		*action = PW_ACTION_CYCLE_VIEWS;
		return true;
	}
	if (strcmp(str, "close") == 0) {
		*action = PW_ACTION_CLOSE_VIEW;
		return true;
	}
	if (strcmp(str, "blank") == 0) {
		*action = PW_ACTION_TOGGLE_BLANK;
		return true;
	}
	if (strcmp(str, "rotate") == 0) {
		*action = PW_ACTION_ROTATE;
		return true;
	}
	if (strcmp(str, "quit") == 0) {
		*action = PW_ACTION_QUIT;
		return true;
	}
	return false;
}

static void add_default_keybindings(struct pw_config *c)
{
	struct pw_keybinding *kb;

	/* KEY_POWER (116) = toggle blank */
	kb = calloc(1, sizeof(*kb));
	if (!kb) return;
	kb->keycode = 116;
	kb->modifiers = 0;
	kb->action = PW_ACTION_TOGGLE_BLANK;
	wl_list_insert(c->keybindings.prev, &kb->link);

	/* alt+Tab = cycle */
	kb = calloc(1, sizeof(*kb));
	if (!kb) return;
	kb->key_name = strdup("Tab");
	kb->modifiers = WLR_MODIFIER_ALT;
	kb->action = PW_ACTION_CYCLE_VIEWS;
	wl_list_insert(c->keybindings.prev, &kb->link);

	/* logo+Escape = quit */
	kb = calloc(1, sizeof(*kb));
	if (!kb) return;
	kb->key_name = strdup("Escape");
	kb->modifiers = WLR_MODIFIER_LOGO;
	kb->action = PW_ACTION_QUIT;
	wl_list_insert(c->keybindings.prev, &kb->link);
}

/* --- Public API -------------------------------------------------------- */

struct pw_config *pw_config_default(void)
{
	struct pw_config *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->render_format = DRM_FORMAT_RGB565;
	wl_list_init(&c->transforms);
	wl_list_init(&c->autostart);
	wl_list_init(&c->keybindings);
	c->background[0] = 0.1f;
	c->background[1] = 0.1f;
	c->background[2] = 0.1f;
	c->background[3] = 1.0f;
	c->idle_timeout_ms = 60000; /* 60 seconds */
	add_default_keybindings(c);
	return c;
}

struct pw_config *pw_config_load(const char *path)
{
	struct pw_config *c = pw_config_default();
	if (!c)
		return NULL;

	FILE *f = NULL;
	if (path) {
		f = fopen(path, "r");
	} else {
		/* Try XDG_CONFIG_HOME/picowl/picowl.ini first */
		const char *home = getenv("HOME");
		if (home) {
			char buf[512];
			snprintf(buf, sizeof(buf), "%s/.config/picowl/picowl.ini", home);
			f = fopen(buf, "r");
		}
		/* Try /etc/picowl.ini as fallback */
		if (!f)
			f = fopen("/etc/picowl.ini", "r");
	}

	if (!f)
		return c; /* Missing file is not an error */

	char line[1024];
	const char *section = NULL;

	while (fgets(line, sizeof(line), f)) {
		char *p = trim(line);

		/* Skip comments and empty lines */
		if (!p || !p[0] || p[0] == '#' || p[0] == ';')
			continue;

		/* Parse section header */
		if (p[0] == '[') {
			char *end = strchr(p, ']');
			if (end) {
				*end = '\0';
				free((void*)section);
				section = strdup(p + 1);
			}
			continue;
		}

		/* Parse key = value */
		char *eq = strchr(p, '=');
		if (!eq)
			continue;

		*eq = '\0';
		char *key = trim(p);
		char *val = trim(eq + 1);

		if (!section)
			continue;

		/* Process by section */
		if (strcmp(section, "render") == 0) {
			if (strcmp(key, "format") == 0) {
				uint32_t fmt = parse_render_format(val);
				if (fmt)
					c->render_format = fmt;
			}
		} else if (strcmp(section, "output") == 0) {
			/* output name = transform */
			struct pw_output_transform *t = calloc(1, sizeof(*t));
			if (t) {
				t->name = strdup(key);
				t->transform = parse_transform(val);
				wl_list_insert(c->transforms.prev, &t->link);
			}
		} else if (strcmp(section, "idle") == 0) {
			if (strcmp(key, "timeout_ms") == 0) {
				c->idle_timeout_ms = atoi(val);
			}
		} else if (strcmp(section, "background") == 0) {
			if (strcmp(key, "color") == 0) {
				if (!parse_color(val, c->background)) {
					pw_log(WLR_ERROR, "Invalid color: %s", val);
				}
			}
		} else if (strcmp(section, "autostart") == 0) {
			if (strcmp(key, "cmd") == 0) {
				struct pw_autostart *as = calloc(1, sizeof(*as));
				if (as) {
					as->command = strdup(val);
					wl_list_insert(c->autostart.prev, &as->link);
				}
			}
		} else if (strcmp(section, "keybindings") == 0) {
			/* modifiers+key = action [cmd] */
			char *key_dup = strdup(key);
			char *val_dup = strdup(val);

			if (!key_dup || !val_dup) {
				free(key_dup);
				free(val_dup);
				continue;
			}

			/* Parse action and command */
			char *space = strchr(val_dup, ' ');
			const char *cmd_str = NULL;
			if (space) {
				*space = '\0';
				cmd_str = trim(space + 1);
			}

			enum pw_action action;
			if (!parse_action(trim(val_dup), &action)) {
				pw_log(WLR_ERROR, "Unknown keybinding action: %s", val_dup);
				free(key_dup);
				free(val_dup);
				continue;
			}

			/* Parse modifiers and key */
			struct pw_keybinding *kb = calloc(1, sizeof(*kb));
			if (kb) {
				kb->action = action;
				kb->command = cmd_str ? strdup(cmd_str) : NULL;
				kb->modifiers = 0;

				/* Parse modifiers */
				if (strstr(key_dup, "alt"))
					kb->modifiers |= WLR_MODIFIER_ALT;
				if (strstr(key_dup, "ctrl"))
					kb->modifiers |= WLR_MODIFIER_CTRL;
				if (strstr(key_dup, "shift"))
					kb->modifiers |= WLR_MODIFIER_SHIFT;
				if (strstr(key_dup, "logo"))
					kb->modifiers |= WLR_MODIFIER_LOGO;

				/* Extract key (last part after last '+') */
				char *key_part = strrchr(key_dup, '+');
				if (!key_part)
					key_part = key_dup;
				else
					key_part++;

				/* Parse key as "code:116" or "KeyName" */
				if (strncmp(key_part, "code:", 5) == 0) {
					kb->keycode = atoi(key_part + 5);
				} else {
					kb->key_name = strdup(key_part);
				}

				wl_list_insert(c->keybindings.prev, &kb->link);
			}

			free(key_dup);
			free(val_dup);
		} else {
			pw_log(WLR_INFO, "Unknown config section: [%s]", section);
		}
	}

	fclose(f);
	free((void*)section);
	return c;
}

void pw_config_free(struct pw_config *config)
{
	if (!config)
		return;

	struct pw_output_transform *t, *t_tmp;
	wl_list_for_each_safe(t, t_tmp, &config->transforms, link) {
		free(t->name);
		free(t);
	}

	struct pw_autostart *as, *as_tmp;
	wl_list_for_each_safe(as, as_tmp, &config->autostart, link) {
		free(as->command);
		free(as);
	}

	struct pw_keybinding *kb, *kb_tmp;
	wl_list_for_each_safe(kb, kb_tmp, &config->keybindings, link) {
		free(kb->key_name);
		free(kb->command);
		free(kb);
	}

	free(config->render_format_pref);
	free(config);
}
