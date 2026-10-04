/* config.c - INI configuration parser. */
#define _XOPEN_SOURCE 700 /* realpath() */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
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
	if (strcmp(str, "panel") == 0) {
		*action = PW_ACTION_TOGGLE_PANEL;
		return true;
	}
	if (strcmp(str, "osk") == 0) {
		*action = PW_ACTION_OSK;
		return true;
	}
	if (strcmp(str, "quit") == 0) {
		*action = PW_ACTION_QUIT;
		return true;
	}
	return false;
}

/* The argument of the osk action; none means toggle. */
static bool parse_osk_op(const char *str, enum pw_osk_op *op)
{
	if (!str || !*str || strcmp(str, "toggle") == 0) {
		*op = PW_OSK_TOGGLE;
		return true;
	}
	if (strcmp(str, "show") == 0) {
		*op = PW_OSK_SHOW;
		return true;
	}
	if (strcmp(str, "hide") == 0) {
		*op = PW_OSK_HIDE;
		return true;
	}
	return false;
}

static bool parse_hold_button(const char *str, enum pw_hold_button *button)
{
	if (strcmp(str, "right") == 0) {
		*button = PW_HOLD_BTN_RIGHT;
		return true;
	}
	if (strcmp(str, "middle") == 0) {
		*button = PW_HOLD_BTN_MIDDLE;
		return true;
	}
	return false;
}

static bool parse_hold_action(const char *str, enum pw_hold_action *action)
{
	if (strcmp(str, "right-click") == 0) {
		*action = PW_HOLD_RIGHT_CLICK;
		return true;
	}
	if (strcmp(str, "none") == 0) {
		*action = PW_HOLD_NONE;
		return true;
	}
	return false;
}

/* Parse an integer in [lo, hi]. On a bad or out-of-range value, log and keep *out. */
static bool parse_int_log(const char *str, const char *key, int lo, int hi, int *out)
{
	char *end;
	long v = strtol(str, &end, 10);
	if (end == str || *end || v < lo || v > hi) {
		pw_log(WLR_ERROR, "%s out of range [%d..%d], using default", key, lo, hi);
		return false;
	}
	*out = (int)v;
	return true;
}

/* Parse "W:H", two decimal integers in [1..4096] with nothing around them.
 * The bound keeps the layout arithmetic far from overflow and rejects typos
 * like 1920:0. On failure *w and *h are untouched. */
static bool parse_aspect(const char *str, int *w, int *h)
{
	char *end;
	if (!isdigit((unsigned char)str[0]))
		return false;
	long a = strtol(str, &end, 10);
	if (*end != ':' || !isdigit((unsigned char)end[1]))
		return false;
	const char *second = end + 1;
	long b = strtol(second, &end, 10);
	if (*end || a < 1 || a > 4096 || b < 1 || b > 4096)
		return false;
	*w = (int)a;
	*h = (int)b;
	return true;
}

/* [layout] stack = first, second. Anything but two distinct names leaves
 * tiling off, because a half-configured stack would be a silent surprise. */
static void parse_layout_stack(struct pw_config *c, const char *val)
{
	char *copy = strdup(val);
	char *names[2] = { NULL, NULL };
	int n = 0;
	bool bad = false;

	free(c->stack[0]);
	free(c->stack[1]);
	c->stack[0] = c->stack[1] = NULL;
	c->n_stack = 0;
	if (!copy)
		return;

	char *save = NULL;
	for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		tok = trim(tok);
		if (!tok[0])
			continue;
		if (n >= 2) {
			pw_log(WLR_ERROR, "[layout] stack: more than two app_ids, ignoring '%s'", tok);
			continue;
		}
		if (n == 1 && strcmp(names[0], tok) == 0) {
			pw_log(WLR_ERROR, "[layout] stack: duplicate app_id '%s' ignored", tok);
			continue;
		}
		names[n++] = tok;
	}
	if (n == 2) {
		c->stack[0] = strdup(names[0]);
		c->stack[1] = strdup(names[1]);
		if (c->stack[0] && c->stack[1]) {
			c->n_stack = 2;
		} else {
			free(c->stack[0]);
			free(c->stack[1]);
			c->stack[0] = c->stack[1] = NULL;
		}
	} else {
		bad = true;
	}
	if (bad)
		pw_log(WLR_ERROR, "[layout] stack needs two distinct app_ids, tiling is off");
	free(copy);
}

/* [layout] pan = namespace, namespace. An empty value turns panning off. A
 * list with an empty name between commas, a name with whitespace or control
 * characters in it, an over-long name or too many names is rejected as a
 * whole and the previous list stays: a half-read list would pan some
 * keyboards and not others without saying why. Layer namespaces are free-form
 * strings, so the only structure the list can rely on is the comma. */
static void parse_layout_pan(struct pw_config *c, const char *val)
{
	char *names[PW_PAN_MAX];
	int n = 0;
	const char *p = val;
	bool bad = false;

	while (*p && isspace((unsigned char)*p))
		p++;
	if (*p) {
		for (;;) {
			const char *end = strchr(p, ',');
			size_t len = end ? (size_t)(end - p) : strlen(p);
			const char *q = p;
			size_t l = len;

			while (l > 0 && isspace((unsigned char)q[0])) { q++; l--; }
			while (l > 0 && isspace((unsigned char)q[l - 1])) l--;
			if (l == 0 || l >= PW_PAN_NAME_MAX || n >= PW_PAN_MAX) {
				bad = true;
				break;
			}
			for (size_t i = 0; i < l; i++)
				if (isspace((unsigned char)q[i]) || iscntrl((unsigned char)q[i]))
					bad = true;
			if (bad)
				break;
			names[n] = strndup(q, l);
			if (!names[n]) {
				bad = true;
				break;
			}
			n++;
			if (!end)
				break;
			p = end + 1;
		}
	}
	if (bad) {
		pw_log(WLR_ERROR, "[layout] pan: needs comma separated layer namespaces without "
			"spaces inside (at most %d, each shorter than %d), keeping the previous value",
			PW_PAN_MAX, PW_PAN_NAME_MAX);
		while (n > 0)
			free(names[--n]);
		return;
	}
	for (int i = 0; i < c->n_pan; i++)
		free(c->pan[i]);
	for (int i = 0; i < n; i++)
		c->pan[i] = names[i];
	c->n_pan = n;
}

bool pw_config_pan_match(const struct pw_config *config, const char *namespace)
{
	if (!config || !namespace)
		return false;
	for (int i = 0; i < config->n_pan; i++)
		if (strcmp(config->pan[i], namespace) == 0)
			return true;
	return false;
}

/* Parse boolean value and return success. On invalid value, return false and keep the previous value.
 * This version logs errors. For use in config parsing where we want to report problems. */
static bool parse_bool_log(const char *str, const char *key, bool *out)
{
	if (!str || !out)
		return false;

	/* Make case-insensitive by converting to lowercase for comparison */
	char lower[16];
	size_t len = strlen(str);
	if (len >= sizeof(lower))
		return false;  /* Too long */

	for (size_t i = 0; i <= len; i++)
		lower[i] = tolower((unsigned char)str[i]);

	/* Accept yes/no/true/false/1/0/on/off (case-insensitive) */
	if (strcmp(lower, "yes") == 0 || strcmp(lower, "true") == 0 ||
	    strcmp(lower, "1") == 0 || strcmp(lower, "on") == 0) {
		*out = true;
		return true;
	}
	if (strcmp(lower, "no") == 0 || strcmp(lower, "false") == 0 ||
	    strcmp(lower, "0") == 0 || strcmp(lower, "off") == 0) {
		*out = false;
		return true;
	}

	/* Invalid value: log and keep previous value */
	if (key)
		pw_log(WLR_ERROR, "Invalid boolean value for '%s': %s (use yes/no, true/false, 1/0, or on/off)", key, str);
	return false;
}

/* Exactly six finite numbers separated by whitespace. Magnitudes beyond 1e3 are
 * refused: normalized matrices are of order one. */
static bool parse_matrix6(const char *str, float out[6])
{
	const char *p = str;
	for (int i = 0; i < 6; i++) {
		char *end;
		errno = 0;
		double v = strtod(p, &end);
		if (end == p || errno == ERANGE || !isfinite(v) || fabs(v) > 1e3)
			return false;
		out[i] = (float)v;
		p = end;
	}
	while (isspace((unsigned char)*p))
		p++;
	return *p == '\0';
}

static bool parse_hex_color(const char *str, uint32_t *out)
{
	/* Parse #RRGGBB as 0xRRGGBB */
	if (str[0] != '#' || strlen(str) != 7)
		return false;
	for (int i = 1; i < 7; i++)
		if (!isxdigit((unsigned char)str[i]))
			return false;
	*out = (uint32_t)strtoul(str + 1, NULL, 16);
	return true;
}

/* Find or add a rule (for [app.*] and [layer.*] sections, merging repeated ones). */
static struct pw_app_rule *find_or_add_rule(struct pw_config *c, enum pw_rule_kind kind, const char *name)
{
	if (!name || !name[0])
		return NULL;

	struct pw_app_rule *rule;
	wl_list_for_each(rule, &c->app_rules, link) {
		if (rule->kind == kind && rule->name && strcmp(rule->name, name) == 0)
			return rule;
	}

	rule = calloc(1, sizeof(*rule));
	if (!rule)
		return NULL;
	rule->name = strdup(name);
	if (!rule->name) {
		free(rule);
		return NULL;
	}
	rule->kind = kind;
	rule->zb_buffers = -1;
	rule->zb_budget_kb = -1;
	wl_list_insert(c->app_rules.prev, &rule->link);
	return rule;
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
	c->hold_action = PW_HOLD_RIGHT_CLICK;
	c->hold_button = PW_HOLD_BTN_RIGHT;
	c->hold_delay_ms = 300;
	c->hold_ms = 900;
	c->slop_px = 8;
	c->pointercal = strdup("/etc/pointercal");
	wl_list_init(&c->app_rules);
	c->osk_restart = true;
	c->pan[0] = strdup("wvkbd");
	c->n_pan = c->pan[0] ? 1 : 0;
	c->hold_animation = NULL;
	c->cursor_fill = 0x2050c0;
	c->cursor_outline = 0xffffff;
	c->cursor_frame_ms = 83;
	/* Rotation, zero-copy, and memory settings */
	wl_list_init(&c->rotation_modes);
	wl_list_init(&c->copy_overrides);
	c->zerocopy = true;
	c->caching_override = PW_CACHING_OV_AUTO;
	c->single_buffer = true;
	c->zb_max_buffers = 3;
	c->zb_budget_kb = 2048;
	c->zb_n_pools = 1;
	c->panel_autohide = true;
	c->arena_max = 1;
	c->trim_threshold_kb = 256;
	c->mmap_threshold_kb = 128;
	c->top_pad_kb = 16;
	c->trim_after_start = true;
	/* Power */
	c->backlight = NULL; /* auto */
	c->low_capacity = 15;
	c->dim_level = 30;
	c->poll_s = 300;
	c->power[PW_PROFILE_AC] = (struct pw_power_timing){ 120, 600, true };
	c->power[PW_PROFILE_BATTERY] = (struct pw_power_timing){ 20, 60, true };
	c->power[PW_PROFILE_LOW] = (struct pw_power_timing){ 10, 30, true };
	c->low_max_brightness_pct = 40;
	c->lease_enable = true;
	c->lease_allow = strdup("mediaplayer");
	c->capture_enable = false;
	/* The keyboard times its own long presses (key repeat, accents), and the
	 * panel's sliders are dragged: a hold must not delay their presses until
	 * the finger moves 8 px or lifts, nor turn a slow drag into a right click
	 * that swallows it. A user [layer.wvkbd] or [layer.panel] merges over
	 * these. The other keys are resolved from [touch] by pw_config_load();
	 * they are filled in here too for a config that is never loaded from a
	 * file. */
	static const char *const no_hold_layers[] = { "wvkbd", "panel" };
	for (size_t i = 0; i < sizeof(no_hold_layers) / sizeof(no_hold_layers[0]); i++) {
		struct pw_app_rule *rule = find_or_add_rule(c, PW_RULE_LAYER, no_hold_layers[i]);
		if (!rule)
			continue;
		rule->hold.action = PW_HOLD_NONE;
		rule->hold.button = c->hold_button;
		rule->hold.delay_ms = c->hold_delay_ms;
		rule->hold.hold_ms = c->hold_ms;
		rule->hold.slop_px = c->slop_px;
		rule->set |= PW_HOLD_SET_ACTION;
	}
	add_default_keybindings(c);
	return c;
}

struct pw_config *pw_config_load(const char *path)
{
	struct pw_config *c = pw_config_default();
	if (!c)
		return NULL;

	FILE *f = NULL;
	const char *loaded_path = NULL;
	char cfgbuf[512]; /* backs loaded_path for the XDG/HOME cases */

	if (path) {
		/* Explicit path from -c: must succeed */
		f = fopen(path, "r");
		if (!f) {
			pw_log(WLR_ERROR, "Failed to open config file '%s': %s", path, strerror(errno));
			return c; /* Still return defaults, but log the error */
		}
		loaded_path = path;
	} else {
		/* Try XDG_CONFIG_HOME/picowl/picowl.ini first */
		const char *xdg_config = getenv("XDG_CONFIG_HOME");
		if (xdg_config && xdg_config[0]) {
			snprintf(cfgbuf, sizeof(cfgbuf), "%s/picowl/picowl.ini", xdg_config);
			f = fopen(cfgbuf, "r");
			if (f) {
				loaded_path = cfgbuf;
			}
		}

		/* Try HOME/.config/picowl/picowl.ini */
		if (!f) {
			const char *home = getenv("HOME");
			if (home) {
				snprintf(cfgbuf, sizeof(cfgbuf), "%s/.config/picowl/picowl.ini", home);
				f = fopen(cfgbuf, "r");
				if (f) {
					loaded_path = cfgbuf;
				}
			}
		}

		/* Try /etc/picowl.ini as final fallback */
		if (!f) {
			f = fopen("/etc/picowl.ini", "r");
			if (f) {
				loaded_path = "/etc/picowl.ini";
			}
		}

		if (!f) {
			pw_log(WLR_INFO, "No config file found; using defaults");
			return c; /* Missing file in implicit search is not an error */
		}
	}

	if (!loaded_path)
		loaded_path = "(unknown)";

	char line[1024];
	const char *section = NULL;
	struct pw_app_rule *cur_rule = NULL; /* rule of the current [app.*]/[layer.*] section */
	int idle_timeout_ms_legacy = -1; /* Legacy: [idle] timeout_ms, alias [core] idle_timeout_ms */
	bool has_any_blank_after_s = false; /* Track if any [power.*] blank_after_s was set */

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
				cur_rule = NULL;
				if (section && (strcmp(section, "app.") == 0 || strcmp(section, "layer.") == 0))
					pw_log(WLR_ERROR, "Empty name in [%s]; ignoring section", section);
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
				/* Legacy key: feeds the [power.*] blank timeout. */
				idle_timeout_ms_legacy = c->idle_timeout_ms;
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
		} else if (strcmp(section, "osk") == 0) {
			if (strcmp(key, "cmd") == 0) {
				char *dup = *val ? strdup(val) : NULL;
				if (dup || !*val) {
					free(c->osk_cmd);
					c->osk_cmd = dup;
				}
			} else if (strcmp(key, "restart") == 0) {
				parse_bool_log(val, "osk.restart", &c->osk_restart);
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

			enum pw_osk_op osk_op = PW_OSK_TOGGLE;
			if (action == PW_ACTION_OSK && !parse_osk_op(cmd_str, &osk_op)) {
				pw_log(WLR_ERROR, "Unknown osk argument: %s (valid: show, hide, toggle)", cmd_str);
				free(key_dup);
				free(val_dup);
				continue;
			}

			/* Parse modifiers and key */
			struct pw_keybinding *kb = calloc(1, sizeof(*kb));
			if (kb) {
				kb->action = action;
				kb->command = action != PW_ACTION_OSK && cmd_str ? strdup(cmd_str) : NULL;
				kb->osk_op = osk_op;
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
		} else if (strcmp(section, "touch") == 0) {
			/* Touch (tap-and-hold) configuration */
			if (strcmp(key, "hold_action") == 0) {
				enum pw_hold_action action;
				if (parse_hold_action(val, &action)) {
					c->hold_action = action;
				} else {
					pw_log(WLR_ERROR, "Unknown hold_action: %s (valid: right-click, none)", val);
				}
			} else if (strcmp(key, "hold_button") == 0) {
				enum pw_hold_button button;
				if (parse_hold_button(val, &button))
					c->hold_button = button;
				else
					pw_log(WLR_ERROR, "Unknown hold_button: %s (valid: right, middle)", val);
			} else if (strcmp(key, "hold_delay_ms") == 0) {
				c->hold_delay_ms = atoi(val);
			} else if (strcmp(key, "hold_ms") == 0) {
				c->hold_ms = atoi(val);
			} else if (strcmp(key, "slop_px") == 0) {
				c->slop_px = atoi(val);
			} else if (strcmp(key, "calibration") == 0) {
				float m[6];
				if (parse_matrix6(val, m)) {
					memcpy(c->calibration, m, sizeof(m));
					c->have_calibration = true;
				} else {
					pw_log(WLR_ERROR, "Bad [touch] calibration: '%s' (need six "
						"numbers: a b c d e f); ignored", val);
				}
			} else if (strcmp(key, "pointercal") == 0) {
				char *dup = strdup(val);
				if (dup) {
					free(c->pointercal);
					c->pointercal = dup;
				}
			}
		} else if (strcmp(section, "cursor") == 0) {
			/* Cursor (hold animation) configuration */
			if (strcmp(key, "hold_animation") == 0) {
				free(c->hold_animation);
				c->hold_animation = strdup(val);
			} else if (strcmp(key, "fill") == 0) {
				uint32_t color;
				if (parse_hex_color(val, &color)) {
					c->cursor_fill = color;
				} else {
					pw_log(WLR_ERROR, "Invalid color format: %s (use #RRGGBB)", val);
				}
			} else if (strcmp(key, "outline") == 0) {
				uint32_t color;
				if (parse_hex_color(val, &color)) {
					c->cursor_outline = color;
				} else {
					pw_log(WLR_ERROR, "Invalid color format: %s (use #RRGGBB)", val);
				}
			} else if (strcmp(key, "frame_interval_ms") == 0) {
				c->cursor_frame_ms = atoi(val);
			}
		} else if (strcmp(section, "rotation") == 0) {
			/* Per-output rotation mode override */
			enum pw_rot_mode mode;
			if (pw_rot_mode_parse(val, &mode)) {
				struct pw_output_rotmode *rm = calloc(1, sizeof(*rm));
				if (rm) {
					rm->name = strdup(key);
					rm->mode = mode;
					wl_list_insert(c->rotation_modes.prev, &rm->link);
				}
			} else {
				pw_log(WLR_ERROR, "Invalid rotation mode: %s (valid: auto, hardware, software)", val);
			}
		} else if (strcmp(section, "copytype") == 0) {
			/* Per-output copy-type override */
			enum pw_copy_override ov;
			if (pw_copytype_parse(val, &ov)) {
				struct pw_output_copyover *co = calloc(1, sizeof(*co));
				if (co) {
					co->name = strdup(key);
					co->ov = ov;
					wl_list_insert(c->copy_overrides.prev, &co->link);
				}
			} else {
				pw_log(WLR_ERROR, "Invalid copy-type: %s (valid: auto, yes, no)", val);
			}
		} else if (strcmp(section, "zerocopy") == 0) {
			/* Zero-copy and memory settings */
			if (strcmp(key, "enable") == 0) {
				parse_bool_log(val, "zerocopy.enable", &c->zerocopy);
			} else if (strcmp(key, "single_buffer") == 0) {
				parse_bool_log(val, "zerocopy.single_buffer", &c->single_buffer);
			} else if (strcmp(key, "panel_autohide") == 0) {
				parse_bool_log(val, "zerocopy.panel_autohide", &c->panel_autohide);
			} else if (strcmp(key, "max_buffers_per_client") == 0) {
				parse_int_log(val, "max_buffers_per_client", 1, 32, &c->zb_max_buffers);
			} else if (strcmp(key, "caching") == 0) {
				if (!pw_caching_parse(val, &c->caching_override)) {
					pw_log(WLR_ERROR, "Invalid [zerocopy] caching: %s (valid: auto, cacheable, write_combined)", val);
					c->caching_override = PW_CACHING_OV_AUTO;
				}
			} else if (strcmp(key, "budget_kb") == 0) {
				parse_int_log(val, "budget_kb", 0, 65536, &c->zb_budget_kb);
			} else if (strcmp(key, "total_kb") == 0) {
				parse_int_log(val, "total_kb", 0, 65536, &c->zb_total_kb);
			} else {
				pw_log(WLR_ERROR, "Unknown key in [zerocopy]: %s", key);
			}
		} else if (strcmp(section, "lease") == 0) {
			if (strcmp(key, "enable") == 0) {
				parse_bool_log(val, "lease.enable", &c->lease_enable);
			} else if (strcmp(key, "allow") == 0) {
				char *allow = strdup(val);
				if (allow) {
					free(c->lease_allow);
					c->lease_allow = allow;
				}
			} else {
				pw_log(WLR_ERROR, "Unknown key in [lease]: %s", key);
			}
		} else if (strcmp(section, "layout") == 0) {
			if (strcmp(key, "stack") == 0) {
				parse_layout_stack(c, val);
			} else if (strcmp(key, "pan") == 0) {
				parse_layout_pan(c, val);
			} else if (strcmp(key, "focus") == 0) {
				/* checked against stack once the whole file is read, so
				 * the order of the two keys does not matter */
				char *focus = val[0] ? strdup(val) : NULL;
				if (focus || !val[0]) {
					free(c->focus);
					c->focus = focus;
				}
			} else {
				pw_log(WLR_ERROR, "Unknown key in [layout]: %s", key);
			}
		} else if (strcmp(section, "capture") == 0) {
			if (strcmp(key, "enabled") == 0) {
				parse_bool_log(val, "capture.enabled", &c->capture_enable);
			} else {
				pw_log(WLR_ERROR, "Unknown key in [capture]: %s", key);
			}
		} else if (strcmp(section, "memory") == 0) {
			/* Memory configuration */
			if (strcmp(key, "arena_max") == 0) {
				int val_int = atoi(val);
				if (val_int >= 1 && val_int <= 8) {
					c->arena_max = val_int;
				} else {
					pw_log(WLR_ERROR, "arena_max out of range [1..8], using default");
				}
			} else if (strcmp(key, "trim_threshold_kb") == 0) {
				int val_int = atoi(val);
				if (val_int >= 0 && val_int <= 65536) {
					c->trim_threshold_kb = val_int;
				} else {
					pw_log(WLR_ERROR, "trim_threshold_kb out of range [0..65536], using default");
				}
			} else if (strcmp(key, "mmap_threshold_kb") == 0) {
				int val_int = atoi(val);
				if (val_int >= 16 && val_int <= 4096) {
					c->mmap_threshold_kb = val_int;
				} else {
					pw_log(WLR_ERROR, "mmap_threshold_kb out of range [16..4096], using default");
				}
			} else if (strcmp(key, "top_pad_kb") == 0) {
				int val_int = atoi(val);
				if (val_int >= 0 && val_int <= 65536) {
					c->top_pad_kb = val_int;
				} else {
					pw_log(WLR_ERROR, "top_pad_kb out of range [0..65536], using default");
				}
			} else if (strcmp(key, "trim_after_start") == 0) {
				parse_bool_log(val, "memory.trim_after_start", &c->trim_after_start);
			} else {
				pw_log(WLR_ERROR, "Unknown key in [memory]: %s", key);
			}
		} else if (strcmp(section, "core") == 0) {
			/* [core] section for backwards compatibility */
			if (strcmp(key, "idle_timeout_ms") == 0) {
				idle_timeout_ms_legacy = atoi(val);
			}
		} else if (strcmp(section, "power") == 0) {
			/* [power] section: power supply configuration */
			if (strcmp(key, "backlight") == 0) {
				free(c->backlight);
				if (strcmp(val, "auto") == 0) {
					c->backlight = NULL;
				} else {
					c->backlight = strdup(val);
				}
			} else if (strcmp(key, "low_capacity") == 0) {
				int val_int = atoi(val);
				if (val_int >= 0 && val_int <= 100) {
					c->low_capacity = val_int;
				} else {
					pw_log(WLR_INFO, "low_capacity %d out of range [0..100], using default 15", val_int);
				}
			} else if (strcmp(key, "dim_level") == 0) {
				int val_int = atoi(val);
				if (val_int >= 1 && val_int <= 100) {
					c->dim_level = val_int;
				} else {
					pw_log(WLR_INFO, "dim_level %d out of range [1..100], using default 30", val_int);
				}
			} else if (strcmp(key, "poll_s") == 0) {
				int val_int = atoi(val);
				if (val_int == 0 || (val_int >= 30 && val_int <= 86400)) {
					c->poll_s = val_int;
				} else {
					pw_log(WLR_INFO, "poll_s %d out of range (0 or 30..86400), using default 300", val_int);
				}
			}
		} else if (strncmp(section, "power.", 6) == 0) {
			/* [power.ac], [power.battery], [power.low] sections */
			const char *profile_name = section + 6;
			enum pw_power_profile profile;
			bool profile_matched = false;

			if (strcmp(profile_name, "ac") == 0) {
				profile = PW_PROFILE_AC;
				profile_matched = true;
			} else if (strcmp(profile_name, "battery") == 0) {
				profile = PW_PROFILE_BATTERY;
				profile_matched = true;
			} else if (strcmp(profile_name, "low") == 0) {
				profile = PW_PROFILE_LOW;
				profile_matched = true;
			}

			if (profile_matched) {
				if (strcmp(key, "dim_after_s") == 0) {
					int val_int = atoi(val);
					if (val_int >= 0 && val_int <= 86400) {
						c->power[profile].dim_after_s = val_int;
					} else {
						pw_log(WLR_INFO, "[power.%s] dim_after_s %d out of range [0..86400], using default",
							profile_name, val_int);
					}
				} else if (strcmp(key, "blank_after_s") == 0) {
					int val_int = atoi(val);
					if (val_int >= 0 && val_int <= 86400) {
						c->power[profile].blank_after_s = val_int;
						has_any_blank_after_s = true;
					} else {
						pw_log(WLR_INFO, "[power.%s] blank_after_s %d out of range [0..86400], using default",
							profile_name, val_int);
					}
				} else if (strcmp(key, "inhibit") == 0) {
					/* invalid text keeps the default (yes) */
					char name[32];
					snprintf(name, sizeof(name), "power.%s.inhibit", profile_name);
					parse_bool_log(val, name, &c->power[profile].inhibit);
				} else if (strcmp(key, "max_brightness_pct") == 0 && profile == PW_PROFILE_LOW) {
					int val_int = atoi(val);
					if (val_int >= 1 && val_int <= 100) {
						c->low_max_brightness_pct = val_int;
					} else {
						pw_log(WLR_INFO, "[power.low] max_brightness_pct %d out of range [1..100], using default 40", val_int);
					}
				} else {
					pw_log(WLR_ERROR, "Unknown key in [power.%s]: %s", profile_name, key);
				}
			} else {
				pw_log(WLR_ERROR, "Unknown power profile: [power.%s]", profile_name);
			}
		} else if (strncmp(section, "app.", 4) == 0 || strncmp(section, "layer.", 6) == 0) {
			/* [app.<app_id>] / [layer.<namespace>]: per-surface hold overrides */
			if (!cur_rule) {
				bool app = section[0] == 'a';
				cur_rule = find_or_add_rule(c, app ? PW_RULE_APP : PW_RULE_LAYER,
					section + (app ? 4 : 6));
			}
			if (!cur_rule) {
				/* empty name or out of memory: skip the section */
			} else if (strcmp(key, "hold_action") == 0) {
				if (parse_hold_action(val, &cur_rule->hold.action))
					cur_rule->set |= PW_HOLD_SET_ACTION;
				else
					pw_log(WLR_ERROR, "Unknown hold_action in [%s]: %s (valid: right-click, none)", section, val);
			} else if (strcmp(key, "hold_button") == 0) {
				if (parse_hold_button(val, &cur_rule->hold.button))
					cur_rule->set |= PW_HOLD_SET_BUTTON;
				else
					pw_log(WLR_ERROR, "Unknown hold_button in [%s]: %s (valid: right, middle)", section, val);
			} else if (strcmp(key, "hold_delay_ms") == 0) {
				cur_rule->hold.delay_ms = atoi(val);
				cur_rule->set |= PW_HOLD_SET_DELAY;
			} else if (strcmp(key, "hold_ms") == 0) {
				cur_rule->hold.hold_ms = atoi(val);
				cur_rule->set |= PW_HOLD_SET_HOLD;
			} else if (strcmp(key, "slop_px") == 0) {
				cur_rule->hold.slop_px = atoi(val);
				cur_rule->set |= PW_HOLD_SET_SLOP;
			} else if (cur_rule->kind == PW_RULE_APP && strcmp(key, "zerocopy_buffers") == 0) {
				parse_int_log(val, "zerocopy_buffers", 1, 32, &cur_rule->zb_buffers);
			} else if (cur_rule->kind == PW_RULE_APP && strcmp(key, "zerocopy_budget_kb") == 0) {
				parse_int_log(val, "zerocopy_budget_kb", 0, 65536, &cur_rule->zb_budget_kb);
			} else if (cur_rule->kind == PW_RULE_APP && strcmp(key, "aspect") == 0) {
				if (!parse_aspect(val, &cur_rule->aspect_w, &cur_rule->aspect_h))
					pw_log(WLR_ERROR, "Bad aspect '%s' in [%s] (need W:H, 1..4096), ignored",
						val, section);
			} else if (cur_rule->kind == PW_RULE_APP && strcmp(key, "exe") == 0) {
				/* resolve symlinks: it is compared to /proc/<pid>/exe */
				char path[PATH_MAX];
				if (!realpath(val, path)) {
					pw_log(WLR_INFO, "exe %s in [%s]: %s; keeping it as is", val, section, strerror(errno));
					snprintf(path, sizeof(path), "%s", val);
				}
				free(cur_rule->exe);
				cur_rule->exe = strdup(path);
			} else {
				pw_log(WLR_INFO, "Unknown key in [%s]: %s", section, key);
			}
		} else {
			pw_log(WLR_INFO, "Unknown config section: [%s]", section);
		}
	}

	fclose(f);
	free((void*)section);

	if (c->focus && (c->n_stack != 2 || (strcmp(c->focus, c->stack[0]) != 0 &&
			strcmp(c->focus, c->stack[1]) != 0))) {
		pw_log(WLR_ERROR, "[layout] focus '%s' is not one of the two stack apps, ignoring it",
			c->focus);
		free(c->focus);
		c->focus = NULL;
	}

	/* Log which config file was loaded */
	if (loaded_path)
		pw_log(WLR_INFO, "Loaded config from %s", loaded_path);

	/* Backwards compatibility: if legacy [idle] timeout_ms (or [core] idle_timeout_ms) was set and no
	 * [power.*] blank_after_s was configured, apply it to all profiles. */
	if (idle_timeout_ms_legacy >= 0 && !has_any_blank_after_s) {
		int blank_after_s = (idle_timeout_ms_legacy + 999) / 1000; /* round up; 0 stays 0 */
		if (blank_after_s < 0)
			blank_after_s = 0;
		if (blank_after_s > 86400)
			blank_after_s = 86400;
		c->power[PW_PROFILE_AC].blank_after_s = blank_after_s;
		c->power[PW_PROFILE_BATTERY].blank_after_s = blank_after_s;
		c->power[PW_PROFILE_LOW].blank_after_s = blank_after_s;
		pw_log(WLR_INFO, "Applying legacy idle timeout %d ms as blank_after_s=%d for all profiles",
			idle_timeout_ms_legacy, blank_after_s);
	}

	/* Validate touch and cursor configuration ranges */
	if (c->hold_ms <= c->hold_delay_ms) {
		pw_log(WLR_ERROR, "hold_ms (%d) must be greater than hold_delay_ms (%d); using defaults",
			c->hold_ms, c->hold_delay_ms);
		c->hold_ms = 900;
		c->hold_delay_ms = 300;
	}

	if (c->slop_px < 0 || c->slop_px > 64) {
		pw_log(WLR_ERROR, "slop_px (%d) out of range [0..64]; using default 8", c->slop_px);
		c->slop_px = 8;
	}

	if (c->cursor_frame_ms < 20 || c->cursor_frame_ms > 1000) {
		pw_log(WLR_ERROR, "cursor_frame_ms (%d) out of range [20..1000]; using default 83", c->cursor_frame_ms);
		c->cursor_frame_ms = 83;
	}

	/* Validate power configuration ranges (final safety check after parsing) */
	if (c->low_capacity < 0 || c->low_capacity > 100) {
		pw_log(WLR_INFO, "low_capacity (%d) out of range [0..100]; using default 15", c->low_capacity);
		c->low_capacity = 15;
	}

	if (c->dim_level < 1 || c->dim_level > 100) {
		pw_log(WLR_INFO, "dim_level (%d) out of range [1..100]; using default 30", c->dim_level);
		c->dim_level = 30;
	}

	if (c->poll_s != 0 && (c->poll_s < 30 || c->poll_s > 86400)) {
		pw_log(WLR_INFO, "poll_s (%d) out of range (0 or 30..86400); using default 300", c->poll_s);
		c->poll_s = 300;
	}

	if (c->low_max_brightness_pct < 1 || c->low_max_brightness_pct > 100) {
		pw_log(WLR_INFO, "low_max_brightness_pct (%d) out of range [1..100]; using default 40", c->low_max_brightness_pct);
		c->low_max_brightness_pct = 40;
	}

	/* Validate power timing ranges for all profiles */
	for (int i = 0; i < PW_PROFILE_COUNT; i++) {
		if (c->power[i].dim_after_s < 0 || c->power[i].dim_after_s > 86400) {
			pw_log(WLR_INFO, "power[%d].dim_after_s (%d) out of range [0..86400]; using default",
				i, c->power[i].dim_after_s);
			if (i == PW_PROFILE_AC)
				c->power[i].dim_after_s = 120;
			else if (i == PW_PROFILE_BATTERY)
				c->power[i].dim_after_s = 20;
			else
				c->power[i].dim_after_s = 10;
		}
		if (c->power[i].blank_after_s < 0 || c->power[i].blank_after_s > 86400) {
			pw_log(WLR_INFO, "power[%d].blank_after_s (%d) out of range [0..86400]; using default",
				i, c->power[i].blank_after_s);
			if (i == PW_PROFILE_AC)
				c->power[i].blank_after_s = 600;
			else if (i == PW_PROFILE_BATTERY)
				c->power[i].blank_after_s = 60;
			else
				c->power[i].blank_after_s = 30;
		}
	}

	/* [app.*] rules with zerocopy keys get their own buffer pool (0 is the
	 * default pool); the total can be below the sum of the pools. */
	int fixed_kb = c->zb_budget_kb;
	struct pw_app_rule *zr;
	wl_list_for_each(zr, &c->app_rules, link) {
		if (zr->kind != PW_RULE_APP || (zr->zb_buffers < 0 && zr->zb_budget_kb < 0))
			continue;
		zr->zb_pool = c->zb_n_pools++;
		if (zr->zb_budget_kb >= 0)
			fixed_kb += zr->zb_budget_kb;
	}
	if (c->zb_total_kb && c->zb_n_pools > 1 && c->zb_total_kb < fixed_kb)
		pw_log(WLR_INFO, "[zerocopy] total_kb %d is below the pools' sum, pools can overcommit",
			c->zb_total_kb);

	/* Resolve [app.*]/[layer.*] rules: unset keys come from [touch]; bad
	 * timings revert to [touch] (the action and button stay as configured). */
	struct pw_app_rule *rule;
	wl_list_for_each(rule, &c->app_rules, link) {
		struct pw_hold_params *h = &rule->hold;
		if (!(rule->set & PW_HOLD_SET_ACTION))
			h->action = c->hold_action;
		if (!(rule->set & PW_HOLD_SET_BUTTON))
			h->button = c->hold_button;
		if (!(rule->set & PW_HOLD_SET_DELAY))
			h->delay_ms = c->hold_delay_ms;
		if (!(rule->set & PW_HOLD_SET_HOLD))
			h->hold_ms = c->hold_ms;
		if (!(rule->set & PW_HOLD_SET_SLOP))
			h->slop_px = c->slop_px;
		if (h->hold_ms <= h->delay_ms || h->slop_px < 0 || h->slop_px > 64) {
			pw_log(WLR_ERROR, "Bad timings in [%s.%s]; using [touch] values",
				rule->kind == PW_RULE_APP ? "app" : "layer", rule->name);
			h->delay_ms = c->hold_delay_ms;
			h->hold_ms = c->hold_ms;
			h->slop_px = c->slop_px;
		}
	}

	return c;
}

void pw_config_free(struct pw_config *config)
{
	if (!config)
		return;

	free(config->backlight);
	free(config->lease_allow);
	free(config->stack[0]);
	free(config->stack[1]);
	free(config->focus);
	for (int i = 0; i < config->n_pan; i++)
		free(config->pan[i]);
	free(config->pointercal);

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

	struct pw_output_rotmode *rm, *rm_tmp;
	wl_list_for_each_safe(rm, rm_tmp, &config->rotation_modes, link) {
		free(rm->name);
		free(rm);
	}

	struct pw_output_copyover *co, *co_tmp;
	wl_list_for_each_safe(co, co_tmp, &config->copy_overrides, link) {
		free(co->name);
		free(co);
	}

	struct pw_app_rule *rule, *rule_tmp;
	wl_list_for_each_safe(rule, rule_tmp, &config->app_rules, link) {
		free(rule->name);
		free(rule->exe);
		free(rule);
	}

	free(config->osk_cmd);
	free(config->render_format_pref);
	free(config->hold_animation);
	free(config);
}

enum pw_rot_mode pw_config_rot_mode(const struct pw_config *c, const char *output_name)
{
	if (!c)
		return PW_ROT_AUTO;

	/* Try exact name first */
	if (output_name) {
		struct pw_output_rotmode *rm;
		wl_list_for_each(rm, &c->rotation_modes, link) {
			if (rm->name && strcmp(rm->name, output_name) == 0)
				return rm->mode;
		}
	}

	/* Try wildcard "*" */
	struct pw_output_rotmode *rm;
	wl_list_for_each(rm, &c->rotation_modes, link) {
		if (rm->name && strcmp(rm->name, "*") == 0)
			return rm->mode;
	}

	/* Default to AUTO */
	return PW_ROT_AUTO;
}

enum pw_copy_override pw_config_copy_override(const struct pw_config *c, const char *output_name)
{
	if (!c)
		return PW_COPY_AUTO;

	/* Try exact name first */
	if (output_name) {
		struct pw_output_copyover *co;
		wl_list_for_each(co, &c->copy_overrides, link) {
			if (co->name && strcmp(co->name, output_name) == 0)
				return co->ov;
		}
	}

	/* Try wildcard "*" */
	struct pw_output_copyover *co;
	wl_list_for_each(co, &c->copy_overrides, link) {
		if (co->name && strcmp(co->name, "*") == 0)
			return co->ov;
	}

	/* Default to AUTO */
	return PW_COPY_AUTO;
}

const struct pw_app_rule *pw_config_app(const struct pw_config *c, const char *app_id)
{
	if (!c || !app_id)
		return NULL;
	struct pw_app_rule *rule;
	wl_list_for_each(rule, &c->app_rules, link)
		if (rule->kind == PW_RULE_APP && rule->name && strcmp(rule->name, app_id) == 0)
			return rule;
	return NULL;
}

bool pw_config_hold(const struct pw_config *c, enum pw_rule_kind kind,
	const char *name, struct pw_hold_params *out)
{
	if (!c || !out)
		return false;

	/* Try exact name match */
	if (name) {
		struct pw_app_rule *rule;
		wl_list_for_each(rule, &c->app_rules, link) {
			if (rule->kind == kind && rule->name && strcmp(rule->name, name) == 0) {
				*out = rule->hold;
				return true;
			}
		}
	}

	/* No match; use global [touch] settings */
	out->action = c->hold_action;
	out->button = c->hold_button;
	out->delay_ms = c->hold_delay_ms;
	out->hold_ms = c->hold_ms;
	out->slop_px = c->slop_px;
	return false;
}
