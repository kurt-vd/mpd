/*
 * Copyright (C) 2003-2011 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "fifo_output_plugin.h"
#include "output_api.h"
#include "utils.h"
#include "timer.h"
#include "fd_util.h"
#include "socket_util.h"
#include "open.h"

#include <glib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#ifndef G_OS_WIN32
#include <sys/socket.h>
#include <sys/un.h>
#else /* G_OS_WIN32 */
#include <ws2tcpip.h>
#include <winsock.h>
#endif /* G_OS_WIN32 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "fifo"

#define FIFO_BUFFER_SIZE 65536 /* pipe capacity on Linux >= 2.6.11 */

struct fifo_data {
	struct audio_output base;

	char *path;
	int input;
	int output;
	bool created;
	struct timer *timer;
};

/**
 * The quark used for GError.domain.
 */
static inline GQuark
fifo_output_quark(void)
{
	return g_quark_from_static_string("fifo_output");
}

static struct fifo_data *fifo_data_new(void)
{
	struct fifo_data *ret;

	ret = g_new(struct fifo_data, 1);

	ret->path = NULL;
	ret->input = -1;
	ret->output = -1;
	ret->created = false;

	return ret;
}

static void fifo_data_free(struct fifo_data *fd)
{
	g_free(fd->path);
	g_free(fd);
}

static void fifo_delete(struct fifo_data *fd)
{
	g_debug("Removing FIFO \"%s\"", fd->path);

	if (unlink(fd->path) < 0) {
		g_warning("Could not remove FIFO \"%s\": %s",
			  fd->path, g_strerror(errno));
		return;
	}

	fd->created = false;
}

static void
fifo_close(struct fifo_data *fd)
{
	struct stat st;

	if (fd->input >= 0) {
		close(fd->input);
		fd->input = -1;
	}

	if (fd->output >= 0) {
		close(fd->output);
		fd->output = -1;
	}

	if (fd->created && (stat(fd->path, &st) == 0))
		fifo_delete(fd);
}

static bool
fifo_make(struct fifo_data *fd, GError **error)
{
	if (mkfifo(fd->path, 0666) < 0) {
		g_set_error(error, fifo_output_quark(), errno,
			    "Couldn't create FIFO \"%s\": %s",
			    fd->path, g_strerror(errno));
		return false;
	}

	fd->created = true;

	return true;
}

static bool
fifo_check(struct fifo_data *fd, GError **error)
{
	struct stat st;

	if (stat(fd->path, &st) < 0) {
		if (errno == ENOENT) {
			/* Path doesn't exist */
			return fifo_make(fd, error);
		}

		g_set_error(error, fifo_output_quark(), errno,
			    "Failed to stat FIFO \"%s\": %s",
			    fd->path, g_strerror(errno));
		return false;
	}

	if (!S_ISFIFO(st.st_mode)) {
		g_set_error(error, fifo_output_quark(), 0,
			    "\"%s\" already exists, but is not a FIFO",
			    fd->path);
		return false;
	}

	return true;
}

static bool
fifo_open(struct fifo_data *fd, GError **error)
{
	if (!fifo_check(fd, error))
		return false;

	fd->input = open_cloexec(fd->path, O_RDONLY|O_NONBLOCK|O_BINARY, 0);
	if (fd->input < 0) {
		g_set_error(error, fifo_output_quark(), errno,
			    "Could not open FIFO \"%s\" for reading: %s",
			    fd->path, g_strerror(errno));
		fifo_close(fd);
		return false;
	}

	fd->output = open_cloexec(fd->path, O_WRONLY|O_NONBLOCK|O_BINARY, 0);
	if (fd->output < 0) {
		g_set_error(error, fifo_output_quark(), errno,
			    "Could not open FIFO \"%s\" for writing: %s",
			    fd->path, g_strerror(errno));
		fifo_close(fd);
		return false;
	}

	return true;
}

static struct audio_output *
fifo_output_init(const struct config_param *param,
		 GError **error_r)
{
	struct fifo_data *fd;

	GError *error = NULL;
	char *path = config_dup_block_path(param, "path", &error);
	if (!path) {
		if (error != NULL)
			g_propagate_error(error_r, error);
		else
			g_set_error(error_r, fifo_output_quark(), 0,
				    "No \"path\" parameter specified");
		return NULL;
	}

	fd = fifo_data_new();
	fd->path = path;

	if (!ao_base_init(&fd->base, &fifo_output_plugin, param, error_r)) {
		fifo_data_free(fd);
		return NULL;
	}

	if (!fifo_open(fd, error_r)) {
		ao_base_finish(&fd->base);
		fifo_data_free(fd);
		return NULL;
	}

	return &fd->base;
}

static void
fifo_output_finish(struct audio_output *ao)
{
	struct fifo_data *fd = (struct fifo_data *)ao;

	fifo_close(fd);
	ao_base_finish(&fd->base);
	fifo_data_free(fd);
}

static bool
fifo_output_open(struct audio_output *ao, struct audio_format *audio_format,
		 G_GNUC_UNUSED GError **error)
{
	struct fifo_data *fd = (struct fifo_data *)ao;

	fd->timer = timer_new(audio_format);

	return true;
}

static void
fifo_output_close(struct audio_output *ao)
{
	struct fifo_data *fd = (struct fifo_data *)ao;

	timer_free(fd->timer);
}

static void
fifo_output_cancel(struct audio_output *ao)
{
	struct fifo_data *fd = (struct fifo_data *)ao;
	char buf[FIFO_BUFFER_SIZE];
	int bytes = 1;

	timer_reset(fd->timer);

	while (bytes > 0 && errno != EINTR)
		bytes = read(fd->input, buf, FIFO_BUFFER_SIZE);

	if (bytes < 0 && errno != EAGAIN) {
		g_warning("Flush of FIFO \"%s\" failed: %s",
			  fd->path, g_strerror(errno));
	}
}

static unsigned
fifo_output_delay(struct audio_output *ao)
{
	struct fifo_data *fd = (struct fifo_data *)ao;

	return fd->timer->started
		? timer_delay(fd->timer)
		: 0;
}

static size_t
fifo_output_play(struct audio_output *ao, const void *chunk, size_t size,
		 GError **error)
{
	struct fifo_data *fd = (struct fifo_data *)ao;
	ssize_t bytes;

	if (!fd->timer->started)
		timer_start(fd->timer);
	timer_add(fd->timer, size);

	while (true) {
		bytes = write(fd->output, chunk, size);
		if (bytes > 0)
			return (size_t)bytes;

		if (bytes < 0) {
			switch (errno) {
			case EAGAIN:
				/* The pipe is full, so empty it */
				fifo_output_cancel(&fd->base);
				continue;
			case EINTR:
				continue;
			}

			g_set_error(error, fifo_output_quark(), errno,
				    "Failed to write to FIFO %s: %s",
				    fd->path, g_strerror(errno));
			return 0;
		}
	}
}

const struct audio_output_plugin fifo_output_plugin = {
	.name = "fifo",
	.init = fifo_output_init,
	.finish = fifo_output_finish,
	.open = fifo_output_open,
	.close = fifo_output_close,
	.delay = fifo_output_delay,
	.play = fifo_output_play,
	.cancel = fifo_output_cancel,
};

/* fifo to raop_play */
#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "raopplay"

struct raopplay_data {
	struct fifo_data fifo;
	const char *uri;
	int sock;
	int playing;
	union {
		struct sockaddr sa;
		struct sockaddr_un un;
	} peer;
	int peerlen;
};

static inline GQuark
raopplay_output_quark(void)
{
	return g_quark_from_static_string("raopplay_output");
}

static struct audio_output *
raopplay_output_init(const struct config_param *param,
		 GError **error_r)
{
	struct raopplay_data *rpd;
	struct audio_output *fd;
	int namelen;
	union {
		struct sockaddr sa;
		struct sockaddr_un un;
	} name;

	fd = fifo_output_init(param, error_r);
	if (!fd)
		return NULL;

	/*
	 * dirty stuff:
	 * - allocate a new data structure,
	 * - copy the already initialized data
	 * - free the already initialized data
	 * - modify a member in the copied data
	 * and yet it works for now.
	 */
	rpd = g_new(struct raopplay_data, 1);
	if (!rpd) {
		g_set_error(error_r, raopplay_output_quark(), 0,
			    "No \"uri\" parameter specified");
		goto fail_realloc;
	}
	rpd->fifo = *(struct fifo_data *)fd;
	g_free(fd);
	/* overrule functions */
	rpd->fifo.base.plugin = &raopplay_output_plugin;
	/* re-assign fd */
	fd = &rpd->fifo.base;

	rpd->uri = config_get_block_string(param, "uri", "@raopplayctl");
	if (!rpd->uri) {
		if (!*error_r)
			g_set_error(error_r, raopplay_output_quark(), 0,
				    "No \"uri\" parameter specified");
		goto fail_uri;
	}
	/* init name */
	name.un.sun_family = AF_UNIX;
	sprintf(name.un.sun_path, "@raopplayctl-%i-mpd", getpid());
	namelen = SUN_LEN(&name.un);
	if (name.un.sun_path[0] == '@')
		name.un.sun_path[0] = 0;

	/* init peer for use in sendto */
	rpd->peer.un.sun_family = AF_UNIX;
	strcpy(rpd->peer.un.sun_path, rpd->uri);
	rpd->peerlen = SUN_LEN(&rpd->peer.un);
	if (rpd->peer.un.sun_path[0] == '@')
		rpd->peer.un.sun_path[0] = 0;

	/* init raopplay */
	rpd->playing = 0;
	rpd->sock = socket_bind_connect(PF_UNIX, SOCK_DGRAM, 0,
			&name.sa, namelen, NULL, 0, error_r);

	if (rpd->sock < 0)
		goto fail_socket;

	return &rpd->fifo.base;

fail_socket:
fail_uri:
fail_realloc:
	fifo_output_finish(fd);
	return NULL;
}

static void
raopplay_output_finish(struct audio_output *ao)
{
	struct raopplay_data *rpd = (void *)ao;

	/* destruct */
	close_socket(rpd->sock);
	fifo_output_finish(ao);
}

static bool
raopplay_output_open(struct audio_output *ao, struct audio_format *audio_format,
		 G_GNUC_UNUSED GError **error)
{
	struct raopplay_data *rpd = (void *)ao;
	char buf[1024];

	sprintf(buf, "play %s\n", rpd->fifo.path);
	if (sendto(rpd->sock, buf, strlen(buf), 0,
				&rpd->peer.sa, rpd->peerlen) < 0) {
		g_set_error(error, raopplay_output_quark(), errno,
			    "send 'play %s' failed", rpd->fifo.path);
		return false;
	}
	if (!fifo_output_open(ao, audio_format, error)) {
		sendto(rpd->sock, "stop\n", 5, 0, &rpd->peer.sa, rpd->peerlen);
		return false;
	}
	return true;
}

static void
raopplay_output_close(struct audio_output *ao)
{
	struct raopplay_data *rpd = (void *)ao;

	fifo_output_close(ao);
	sendto(rpd->sock, "stop\n", 5, 0, &rpd->peer.sa, rpd->peerlen);
}

const struct audio_output_plugin raopplay_output_plugin = {
	.name = "raopplay",
	.init = raopplay_output_init,
	.finish = raopplay_output_finish,
	.open = raopplay_output_open,
	.close = raopplay_output_close,
	.delay = fifo_output_delay,
	.play = fifo_output_play,
	.cancel = fifo_output_cancel,
};
