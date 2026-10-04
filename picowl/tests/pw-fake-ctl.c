/*
 * pw-fake-ctl - an alsa-lib control plugin (ctl_ext) that stands in for a sound
 * card in tests: no kernel driver is needed. One simple mixer element
 * "Master" with a playback volume 0..40 (two channels) and a playback switch.
 *
 * The state lives in a file, $PW_FAKECTL_STATE, as "volume N" and "switch N"
 * lines. A write by a mixer client rewrites the file, and a write to the file
 * by anyone (the test, standing in for another program) is announced to the
 * clients as a value change through inotify, like a driver would.
 *
 * Use it with an ALSA config of
 *   ctl.default { type pwfake }
 *   ctl_type.pwfake { lib "/path/to/libpw-fake-ctl.so" }
 * in $ALSA_CONFIG_PATH.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <alsa/control_external.h>
#include <errno.h>
#include <libgen.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define VOL_MAX 40
#define N_ELEMS 2	/* 0: Master Playback Volume, 1: Master Playback Switch */

struct fake {
	snd_ctl_ext_t ext;
	char path[512];
	int inotify_fd;
	int pending;	/* element events still to be reported */
};

static const char *const elem_names[N_ELEMS] = {
	"Master Playback Volume", "Master Playback Switch",
};

static void load_state(const struct fake *f, long *vol, long *sw)
{
	char line[64];
	FILE *fp = fopen(f->path, "r");

	*vol = 20;
	*sw = 0;
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		long v;
		if (sscanf(line, "volume %ld", &v) == 1)
			*vol = v < 0 ? 0 : v > VOL_MAX ? VOL_MAX : v;
		else if (sscanf(line, "switch %ld", &v) == 1)
			*sw = v ? 1 : 0;
	}
	fclose(fp);
}

static void save_state(const struct fake *f, long vol, long sw)
{
	FILE *fp = fopen(f->path, "w");

	if (!fp)
		return;
	fprintf(fp, "volume %ld\nswitch %ld\n", vol, sw);
	fclose(fp);
}

static void fake_close(snd_ctl_ext_t *ext)
{
	struct fake *f = ext->private_data;

	close(f->inotify_fd);
	free(f);
}

static int fake_elem_count(snd_ctl_ext_t *ext)
{
	(void)ext;
	return N_ELEMS;
}

static void fill_id(snd_ctl_elem_id_t *id, unsigned int idx)
{
	/* The numid is how the hctl layer matches an event to its element. */
	snd_ctl_elem_id_set_numid(id, idx + 1);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, elem_names[idx]);
	snd_ctl_elem_id_set_index(id, 0);
}

static int fake_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id)
{
	(void)ext;
	if (offset >= N_ELEMS)
		return -EINVAL;
	fill_id(id, offset);
	return 0;
}

static snd_ctl_ext_key_t fake_find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id)
{
	(void)ext;
	if (snd_ctl_elem_id_get_interface(id) != SND_CTL_ELEM_IFACE_MIXER ||
			snd_ctl_elem_id_get_index(id) != 0)
		return SND_CTL_EXT_KEY_NOT_FOUND;
	for (unsigned int i = 0; i < N_ELEMS; i++)
		if (!strcmp(snd_ctl_elem_id_get_name(id), elem_names[i]))
			return i;
	return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int fake_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
	int *type, unsigned int *acc, unsigned int *count)
{
	(void)ext;
	*type = key == 0 ? SND_CTL_ELEM_TYPE_INTEGER : SND_CTL_ELEM_TYPE_BOOLEAN;
	*acc = SND_CTL_EXT_ACCESS_READWRITE;
	*count = 2;
	return 0;
}

static int fake_get_integer_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
	long *imin, long *imax, long *istep)
{
	(void)ext; (void)key;
	*imin = 0;
	*imax = VOL_MAX;
	*istep = 1;
	return 0;
}

static int fake_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
	long vol, sw;

	load_state(ext->private_data, &vol, &sw);
	value[0] = value[1] = key == 0 ? vol : sw;
	return 0;
}

static int fake_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
	struct fake *f = ext->private_data;
	long vol, sw;

	load_state(f, &vol, &sw);
	if (key == 0) {
		if (value[0] == vol && value[1] == vol)
			return 0;
		vol = value[0] > value[1] ? value[0] : value[1];
	} else {
		sw = value[0] || value[1];
	}
	save_state(f, vol, sw);
	return 1;
}

static void fake_subscribe(snd_ctl_ext_t *ext, int subscribe)
{
	(void)ext; (void)subscribe;
}

static int fake_read_event(snd_ctl_ext_t *ext, snd_ctl_elem_id_t *id, unsigned int *mask)
{
	struct fake *f = ext->private_data;
	char buf[1024];

	/* snd_mixer_poll_descriptors_revents() never calls poll_revents, so the
	 * notification is drained here, as a driver's event queue would be. */
	if (f->pending <= 0 && read(f->inotify_fd, buf, sizeof(buf)) > 0)
		f->pending = N_ELEMS;
	if (f->pending <= 0)
		return -EAGAIN;
	fill_id(id, N_ELEMS - f->pending);
	f->pending--;
	*mask = SND_CTL_EVENT_MASK_VALUE;
	return 1;
}

static int fake_poll_count(snd_ctl_ext_t *ext)
{
	(void)ext;
	return 1;
}

static int fake_poll_descriptors(snd_ctl_ext_t *ext, struct pollfd *pfds, unsigned int space)
{
	struct fake *f = ext->private_data;

	if (space < 1)
		return 0;
	pfds[0].fd = f->inotify_fd;
	pfds[0].events = POLLIN;
	return 1;
}

static int fake_poll_revents(snd_ctl_ext_t *ext, struct pollfd *pfds, unsigned int nfds,
	unsigned short *revents)
{
	struct fake *f = ext->private_data;
	char buf[1024];

	*revents = 0;
	if (nfds >= 1 && (pfds[0].revents & POLLIN)) {
		/* Any change of the file counts as a change of both elements. */
		if (read(f->inotify_fd, buf, sizeof(buf)) > 0) {
			f->pending = N_ELEMS;
			*revents = POLLIN;
		}
	}
	return 0;
}

static const snd_ctl_ext_callback_t fake_callbacks = {
	.close = fake_close,
	.elem_count = fake_elem_count,
	.elem_list = fake_elem_list,
	.find_elem = fake_find_elem,
	.get_attribute = fake_get_attribute,
	.get_integer_info = fake_get_integer_info,
	.read_integer = fake_read_integer,
	.write_integer = fake_write_integer,
	.subscribe_events = fake_subscribe,
	.read_event = fake_read_event,
	.poll_descriptors_count = fake_poll_count,
	.poll_descriptors = fake_poll_descriptors,
	.poll_revents = fake_poll_revents,
};

SND_CTL_PLUGIN_DEFINE_FUNC(pwfake)
{
	const char *state = getenv("PW_FAKECTL_STATE");
	struct fake *f;
	char dir[512];
	int err;

	(void)root; (void)conf;
	if (!state || !*state)
		return -EINVAL;
	f = calloc(1, sizeof(*f));
	if (!f)
		return -ENOMEM;
	snprintf(f->path, sizeof(f->path), "%s", state);
	snprintf(dir, sizeof(dir), "%s", state);
	f->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (f->inotify_fd < 0 ||
			inotify_add_watch(f->inotify_fd, dirname(dir), IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
		err = -errno;
		if (f->inotify_fd >= 0)
			close(f->inotify_fd);
		free(f);
		return err;
	}
	f->ext.version = SND_CTL_EXT_VERSION;
	f->ext.card_idx = 0;
	strcpy(f->ext.id, "pwfake");
	strcpy(f->ext.driver, "pw-fake-ctl");
	strcpy(f->ext.name, "picowl fake card");
	strcpy(f->ext.longname, "picowl fake card for tests");
	strcpy(f->ext.mixername, "picowl fake mixer");
	f->ext.poll_fd = f->inotify_fd;
	f->ext.callback = &fake_callbacks;
	f->ext.private_data = f;
	err = snd_ctl_ext_create(&f->ext, name, mode);
	if (err < 0) {
		close(f->inotify_fd);
		free(f);
		return err;
	}
	*handlep = f->ext.handle;
	return 0;
}
SND_CTL_PLUGIN_SYMBOL(pwfake);
