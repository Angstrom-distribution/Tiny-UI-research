/* picowl - tiny wlroots compositor for old iPAQ PDAs. See picowl.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include "picowl.h"

static void print_help(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Options:\n");
	printf("  -c CONFIG    Configuration file path (default: /etc/picowl.ini)\n");
	printf("  -s CMD       Startup command to append to autostart\n");
	printf("  -d LEVEL     Debug log level (0-7, default: 1 for WLR_INFO)\n");
	printf("  -h           Show this help message\n");
	printf("  -v           Show version\n");
}

static void print_version(void)
{
	printf("picowl %s\n", PICOWL_VERSION);
}

int main(int argc, char **argv)
{
	const char *config_path = NULL;
	const char *startup_cmd = NULL;
	int debug_level = WLR_INFO;
	int c;

	while ((c = getopt(argc, argv, "c:s:d:hv")) != -1) {
		switch (c) {
		case 'c':
			config_path = optarg;
			break;
		case 's':
			startup_cmd = optarg;
			break;
		case 'd':
			debug_level = atoi(optarg);
			if (debug_level < 0 || debug_level > 7) {
				fprintf(stderr, "Invalid debug level: %s (must be 0-7)\n", optarg);
				return 1;
			}
			break;
		case 'h':
			print_help(argv[0]);
			return 0;
		case 'v':
			print_version();
			return 0;
		case '?':
		default:
			fprintf(stderr, "Use -h for help\n");
			return 1;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
		fprintf(stderr, "Use -h for help\n");
		return 1;
	}

	wlr_log_init(debug_level, NULL);

	/* Load configuration, using defaults if config_path is NULL */
	struct pw_config *config = pw_config_load(config_path);
	if (!config)
		return 1;

	/* Append startup command to autostart list if provided */
	if (startup_cmd) {
		struct pw_autostart *as = calloc(1, sizeof(*as));
		if (!as) {
			pw_log(WLR_ERROR, "Failed to allocate autostart entry");
			pw_config_free(config);
			return 1;
		}
		as->command = strdup(startup_cmd);
		if (!as->command) {
			pw_log(WLR_ERROR, "Failed to allocate startup command");
			free(as);
			pw_config_free(config);
			return 1;
		}
		wl_list_insert(config->autostart.prev, &as->link);
	}

	struct pw_server server = {0};
	int ret = 1;
	if (pw_server_init(&server, config))
		ret = pw_server_run(&server);
	pw_server_finish(&server);
	pw_config_free(config);
	return ret;
}
