/*
 * pw-bare-server - the smallest Wayland server that a client can connect to:
 * one wl_output and nothing else, no compositor, no shm, no layer shell. It
 * stands in for a compositor that lacks what picowl-panel needs.
 *
 * usage: pw-bare-server SOCKET-NAME   (in $XDG_RUNTIME_DIR; runs until killed)
 */
#include <stdio.h>
#include <stdlib.h>
#include <wayland-server.h>

static void bind_output(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	if (!wl_resource_create(client, &wl_output_interface, 1, id)) {
		wl_client_post_no_memory(client);
		return;
	}
	(void)version;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: pw-bare-server SOCKET-NAME\n");
		return 2;
	}
	struct wl_display *display = wl_display_create();
	if (!display || wl_display_add_socket(display, argv[1]) < 0) {
		fprintf(stderr, "pw-bare-server: cannot create the socket %s\n", argv[1]);
		return 1;
	}
	wl_global_create(display, &wl_output_interface, 1, NULL, bind_output);
	printf("pw-bare-server: listening on %s\n", argv[1]);
	fflush(stdout);
	wl_display_run(display);
	wl_display_destroy(display);
	return 0;
}
