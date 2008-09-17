/*
 * $Id$
 *
 * (c) 2004 by Kalle Wallin <kaw@linux.se>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "screen.h"
#include "screen_utils.h"
#include "config.h"
#include "ncmpc.h"
#include "support.h"
#include "mpdclient.h"
#include "utils.h"
#include "command.h"
#include "options.h"
#include "colors.h"
#include "strfsong.h"
#include "wreadln.h"

#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <locale.h>

#define SCREEN_PLAYLIST_ID     0
#define SCREEN_BROWSE_ID       1
#define SCREEN_ARTIST_ID       2
#define SCREEN_HELP_ID         100
#define SCREEN_KEYDEF_ID       101
#define SCREEN_CLOCK_ID        102
#define SCREEN_SEARCH_ID       103
#define SCREEN_LYRICS_ID	   104



/* screens */
extern const struct screen_functions screen_playlist;
extern const struct screen_functions screen_browse;
#ifdef ENABLE_ARTIST_SCREEN
extern const struct screen_functions screen_artist;
#endif
extern const struct screen_functions screen_help;
#ifdef ENABLE_SEARCH_SCREEN
extern const struct screen_functions screen_search;
#endif
#ifdef ENABLE_KEYDEF_SCREEN
extern const struct screen_functions screen_keydef;
#endif
#ifdef ENABLE_CLOCK_SCREEN
extern const struct screen_functions screen_clock;
#endif
extern const struct screen_functions screen_lyrics;

typedef struct screen_functions * (*screen_get_mode_functions_fn_t) (void);

static const struct
{
	gint id;
	const gchar *name;
	const struct screen_functions *functions;
} screens[] = {
	{ SCREEN_PLAYLIST_ID, "playlist", &screen_playlist },
	{ SCREEN_BROWSE_ID, "browse", &screen_browse },
#ifdef ENABLE_ARTIST_SCREEN
	{ SCREEN_ARTIST_ID, "artist", &screen_artist },
#endif
	{ SCREEN_HELP_ID, "help", &screen_help },
#ifdef ENABLE_SEARCH_SCREEN
	{ SCREEN_SEARCH_ID, "search", &screen_search },
#endif
#ifdef ENABLE_KEYDEF_SCREEN
	{ SCREEN_KEYDEF_ID, "keydef", &screen_keydef },
#endif
#ifdef ENABLE_CLOCK_SCREEN
	{ SCREEN_CLOCK_ID, "clock", &screen_clock },
#endif
#ifdef ENABLE_LYRICS_SCREEN
	{ SCREEN_LYRICS_ID, "lyrics", &screen_lyrics },
#endif
};

#define NUM_SCREENS (sizeof(screens) / sizeof(screens[0]))

static gboolean welcome = TRUE;
static struct screen screen;
static const struct screen_functions *mode_fn = &screen_playlist;
static int seek_id = -1;
static int seek_target_time = 0;

gint
screen_get_id(const char *name)
{
	guint i;

	for (i = 0; i < NUM_SCREENS; ++i)
		if (strcmp(name, screens[i].name) == 0)
			return screens[i].id;

	return -1;
}

static gint
lookup_mode(gint id)
{
	guint i;

	for (i = 0; i < NUM_SCREENS; ++i)
		if (screens[i].id == id)
			return i;

	return -1;
}

gint get_cur_mode_id(void)
{
	return screens[screen.mode].id;
}

static void
switch_screen_mode(gint id, mpdclient_t *c)
{
	gint new_mode;

	if( id == screens[screen.mode].id )
		return;

	new_mode = lookup_mode(id);
	if (new_mode < 0)
		return;

	/* close the old mode */
	if (mode_fn->close != NULL)
		mode_fn->close();

	/* get functions for the new mode */
	D("switch_screen(%s)\n", screens[new_mode].name );
	mode_fn = screens[new_mode].functions;
	screen.mode = new_mode;
	screen.painted = 0;

	/* open the new mode */
	if (mode_fn->open != NULL)
		mode_fn->open(&screen, c);
}

static int
find_configured_screen(const char *name)
{
	unsigned i;

	for (i = 0; options.screen_list[i] != NULL; ++i)
		if (strcmp(options.screen_list[i], name) == 0)
			return i;

	return -1;
}

static void
screen_next_mode(mpdclient_t *c, int offset)
{
	int max = g_strv_length(options.screen_list);
	int current, next;

	/* find current screen */
	current = find_configured_screen(screens[screen.mode].name);
	next = current + offset;
	if (next<0)
		next = max-1;
	else if (next>=max)
		next = 0;

	D("current mode: %d:%d    next:%d\n", current, max, next);
	switch_screen_mode(screen_get_id(options.screen_list[next]), c);
}

static void
paint_top_window2(const char *header, mpdclient_t *c)
{
	char flags[5];
	WINDOW *w = screen.top_window.w;
	char buf[32];

	if (header[0]) {
		colors_use(w, COLOR_TITLE_BOLD);
		mvwaddstr(w, 0, 0, header);
	} else {
		colors_use(w, COLOR_TITLE_BOLD);
		waddstr(w, get_key_names(CMD_SCREEN_HELP, FALSE));
		colors_use(w, COLOR_TITLE);
		waddstr(w, _(":Help  "));
		colors_use(w, COLOR_TITLE_BOLD);
		waddstr(w, get_key_names(CMD_SCREEN_PLAY, FALSE));
		colors_use(w, COLOR_TITLE);
		waddstr(w, _(":Playlist  "));
		colors_use(w, COLOR_TITLE_BOLD);
		waddstr(w, get_key_names(CMD_SCREEN_FILE, FALSE));
		colors_use(w, COLOR_TITLE);
		waddstr(w, _(":Browse  "));
#ifdef ENABLE_ARTIST_SCREEN
		colors_use(w, COLOR_TITLE_BOLD);
		waddstr(w, get_key_names(CMD_SCREEN_ARTIST, FALSE));
		colors_use(w, COLOR_TITLE);
		waddstr(w, _(":Artist  "));
#endif
#ifdef ENABLE_SEARCH_SCREEN
		colors_use(w, COLOR_TITLE_BOLD);
		waddstr(w, get_key_names(CMD_SCREEN_SEARCH, FALSE));
		colors_use(w, COLOR_TITLE);
		waddstr(w, _(":Search  "));
#endif
#ifdef ENABLE_LYRICS_SCREEN
		colors_use(w, COLOR_TITLE_BOLD);
		waddstr(w, get_key_names(CMD_SCREEN_LYRICS, FALSE));
		colors_use(w, COLOR_TITLE);
		waddstr(w, _(":Lyrics  "));
#endif
	}
	if (c->status == NULL || c->status->volume == MPD_STATUS_NO_VOLUME) {
		g_snprintf(buf, 32, _("Volume n/a "));
	} else {
		g_snprintf(buf, 32, _(" Volume %d%%"), c->status->volume);
	}
	colors_use(w, COLOR_TITLE);
	mvwaddstr(w, 0, screen.top_window.cols-my_strlen(buf), buf);

	flags[0] = 0;
	if (c->status != NULL) {
		if (c->status->repeat)
			g_strlcat(flags, "r", sizeof(flags));
		if (c->status->random)
			g_strlcat(flags, "z", sizeof(flags));;
		if (c->status->crossfade)
			g_strlcat(flags, "x", sizeof(flags));
		if (c->status->updatingDb)
			g_strlcat(flags, "U", sizeof(flags));
	}

	colors_use(w, COLOR_LINE);
	mvwhline(w, 1, 0, ACS_HLINE, screen.top_window.cols);
	if (flags[0]) {
		wmove(w,1,screen.top_window.cols-strlen(flags)-3);
		waddch(w, '[');
		colors_use(w, COLOR_LINE_BOLD);
		waddstr(w, flags);
		colors_use(w, COLOR_LINE);
		waddch(w, ']');
	}
	wnoutrefresh(w);
}

static void
paint_top_window(const char *header, mpdclient_t *c, int full_repaint)
{
	static int prev_volume = -1;
	static size_t prev_header_len = -1;
	WINDOW *w = screen.top_window.w;

	if (prev_header_len!=my_strlen(header)) {
		prev_header_len = my_strlen(header);
		full_repaint = 1;
	}

	if (full_repaint) {
		wmove(w, 0, 0);
		wclrtoeol(w);
	}

	if ((c->status != NULL && prev_volume != c->status->volume) ||
	    full_repaint)
		paint_top_window2(header, c);
}

static void
paint_progress_window(mpdclient_t *c)
{
	double p;
	int width;
	int elapsedTime;

	if (c->status==NULL || IS_STOPPED(c->status->state)) {
		mvwhline(screen.progress_window.w, 0, 0, ACS_HLINE,
			 screen.progress_window.cols);
		wnoutrefresh(screen.progress_window.w);
		return;
	}

	if (c->song && seek_id == c->song->id)
		elapsedTime = seek_target_time;
	else
		elapsedTime = c->status->elapsedTime;

	p = ((double) elapsedTime) / ((double) c->status->totalTime);

	width = (int) (p * (double) screen.progress_window.cols);
	mvwhline(screen.progress_window.w,
		 0, 0,
		 ACS_HLINE,
		 screen.progress_window.cols);
	whline(screen.progress_window.w, '=', width-1);
	mvwaddch(screen.progress_window.w, 0, width-1, 'O');
	wnoutrefresh(screen.progress_window.w);
}

static void
paint_status_window(mpdclient_t *c)
{
	WINDOW *w = screen.status_window.w;
	mpd_Status *status = c->status;
	mpd_Song *song = c->song;
	int elapsedTime = 0;
	const char *str = NULL;
	int x = 0;

	if( time(NULL) - screen.status_timestamp <= SCREEN_STATUS_MESSAGE_TIME )
		return;

	wmove(w, 0, 0);
	wclrtoeol(w);
	colors_use(w, COLOR_STATUS_BOLD);

	switch (status == NULL ? MPD_STATUS_STATE_STOP : status->state) {
	case MPD_STATUS_STATE_PLAY:
		str = _("Playing:");
		break;
	case MPD_STATUS_STATE_PAUSE:
		str = _("[Paused]");
		break;
	case MPD_STATUS_STATE_STOP:
	default:
		break;
	}

	if (str) {
		waddstr(w, str);
		x += my_strlen(str)+1;
	}

	/* create time string */
	memset(screen.buf, 0, screen.buf_size);
	if (status != NULL && (IS_PLAYING(status->state) ||
			       IS_PAUSED(status->state))) {
		if (status->totalTime > 0) {
			/*checks the conf to see whether to display elapsed or remaining time */
			if(!strcmp(options.timedisplay_type,"elapsed"))
				elapsedTime = c->status->elapsedTime;
			else if(!strcmp(options.timedisplay_type,"remaining"))
				elapsedTime = (c->status->totalTime - c->status->elapsedTime);

			if( c->song && seek_id == c->song->id )
				elapsedTime = seek_target_time;
			/*write out the time, using hours if time over 60 minutes*/
			if (c->status->totalTime > 3600) {
				g_snprintf(screen.buf, screen.buf_size,
					   " [%i:%02i:%02i/%i:%02i:%02i]",
					   elapsedTime/3600, (elapsedTime%3600)/60, elapsedTime%60,
					   status->totalTime/3600, (status->totalTime%3600)/60,  status->totalTime%60);
			} else {
				g_snprintf(screen.buf, screen.buf_size,
					   " [%i:%02i/%i:%02i]",
					   elapsedTime/60, elapsedTime%60,
					   status->totalTime/60,   status->totalTime%60 );
			}
		} else {
			g_snprintf(screen.buf, screen.buf_size,
				   " [%d kbps]", status->bitRate );
		}
	} else {
		time_t timep;

		time(&timep);
		strftime(screen.buf, screen.buf_size, "%X ",localtime(&timep));
	}

	/* display song */
	if (status != NULL && (IS_PLAYING(status->state) ||
			       IS_PAUSED(status->state))) {
		char songname[MAX_SONGNAME_LENGTH];
		int width = COLS-x-my_strlen(screen.buf);

		if (song)
			strfsong(songname, MAX_SONGNAME_LENGTH, STATUS_FORMAT, song);
		else
			songname[0] = '\0';

		colors_use(w, COLOR_STATUS);
		/* scroll if the song name is to long */
		if (options.scroll && my_strlen(songname) > (size_t)width) {
			static  scroll_state_t st = { 0, 0 };
			char *tmp = strscroll(songname, options.scroll_sep, width, &st);

			g_strlcpy(songname, tmp, MAX_SONGNAME_LENGTH);
			g_free(tmp);
		}
		//mvwaddnstr(w, 0, x, songname, width);
		mvwaddstr(w, 0, x, songname);
	}

	/* display time string */
	if (screen.buf[0]) {
		x = screen.status_window.cols - strlen(screen.buf);
		colors_use(w, COLOR_STATUS_TIME);
		mvwaddstr(w, 0, x, screen.buf);
	}

	wnoutrefresh(w);
}

void
screen_exit(void)
{
	guint i;

	endwin();

	if (mode_fn->close != NULL)
		mode_fn->close();

	/* close and exit all screens (playlist,browse,help...) */
	for (i = 0; i < NUM_SCREENS; ++i) {
		const struct screen_functions *sf = screens[i].functions;

		if (sf->exit)
			sf->exit();
	}

	string_list_free(screen.find_history);
	g_free(screen.buf);
	g_free(screen.findbuf);
}

void
screen_resize(void)
{
	guint i;

	D("Resize rows %d->%d, cols %d->%d\n",screen.rows,LINES,screen.cols,COLS);
	if (COLS<SCREEN_MIN_COLS || LINES<SCREEN_MIN_ROWS) {
		screen_exit();
		fprintf(stderr, _("Error: Screen to small!\n"));
		exit(EXIT_FAILURE);
	}

	resizeterm(LINES, COLS);

	screen.cols = COLS;
	screen.rows = LINES;

	/* top window */
	screen.top_window.cols = screen.cols;
	wresize(screen.top_window.w, 2, screen.cols);

	/* main window */
	screen.main_window.cols = screen.cols;
	screen.main_window.rows = screen.rows-4;
	wresize(screen.main_window.w, screen.main_window.rows, screen.cols);
	wclear(screen.main_window.w);

	/* progress window */
	screen.progress_window.cols = screen.cols;
	wresize(screen.progress_window.w, 1, screen.cols);
	mvwin(screen.progress_window.w, screen.rows-2, 0);

	/* status window */
	screen.status_window.cols = screen.cols;
	wresize(screen.status_window.w, 1, screen.cols);
	mvwin(screen.status_window.w, screen.rows-1, 0);

	screen.buf_size = screen.cols;
	g_free(screen.buf);
	screen.buf = g_malloc(screen.cols);

	/* close and exit all screens (playlist,browse,help...) */
	for (i = 0; i < NUM_SCREENS; ++i) {
		const struct screen_functions *sf = screens[i].functions;

		if (sf->resize)
			sf->resize(screen.main_window.cols, screen.main_window.rows);
	}


	/* ? - without this the cursor becomes visible with aterm & Eterm */
	curs_set(1);
	curs_set(0);

	screen.painted = 0;
}

void
screen_status_message(const char *msg)
{
	WINDOW *w = screen.status_window.w;

	wmove(w, 0, 0);
	wclrtoeol(w);
	colors_use(w, COLOR_STATUS_ALERT);
	waddstr(w, msg);
	wnoutrefresh(w);
	screen.status_timestamp = time(NULL);
}

void
screen_status_printf(const char *format, ...)
{
	char *msg;
	va_list ap;

	va_start(ap,format);
	msg = g_strdup_vprintf(format,ap);
	va_end(ap);
	screen_status_message(msg);
	g_free(msg);
}

void
ncurses_init(void)
{

	/* initialize the curses library */
	initscr();
	/* initialize color support */
	colors_start();
	/* tell curses not to do NL->CR/NL on output */
	nonl();
	/*  use raw mode (ignore interrupt,quit,suspend, and flow control ) */
#ifdef ENABLE_RAW_MODE
	//  raw();
#endif
	/* don't echo input */
	noecho();
	/* set cursor invisible */
	curs_set(0);
	/* enable extra keys */
	keypad(stdscr, TRUE);
	/* return from getch() without blocking */
	timeout(SCREEN_TIMEOUT);
	/* initialize mouse support */
#ifdef HAVE_GETMOUSE
	if (options.enable_mouse)
		mousemask(ALL_MOUSE_EVENTS, NULL);
#endif

	if (COLS < SCREEN_MIN_COLS || LINES < SCREEN_MIN_ROWS) {
		fprintf(stderr, _("Error: Screen to small!\n"));
		exit(EXIT_FAILURE);
	}

	screen.mode = 0;
	screen.cols = COLS;
	screen.rows = LINES;

	screen.buf  = g_malloc(screen.cols);
	screen.buf_size = screen.cols;
	screen.findbuf = NULL;
	screen.painted = 0;
	screen.start_timestamp = time(NULL);
	screen.input_timestamp = time(NULL);
	screen.last_cmd = CMD_NONE;

	/* create top window */
	screen.top_window.rows = 2;
	screen.top_window.cols = screen.cols;
	screen.top_window.w = newwin(screen.top_window.rows,
				      screen.top_window.cols,
				      0, 0);
	leaveok(screen.top_window.w, TRUE);
	keypad(screen.top_window.w, TRUE);

	/* create main window */
	screen.main_window.rows = screen.rows-4;
	screen.main_window.cols = screen.cols;
	screen.main_window.w = newwin(screen.main_window.rows,
				       screen.main_window.cols,
				       2,
				       0);

	//  leaveok(screen.main_window.w, TRUE); temporary disabled
	keypad(screen.main_window.w, TRUE);

	/* create progress window */
	screen.progress_window.rows = 1;
	screen.progress_window.cols = screen.cols;
	screen.progress_window.w = newwin(screen.progress_window.rows,
					   screen.progress_window.cols,
					   screen.rows-2,
					   0);
	leaveok(screen.progress_window.w, TRUE);

	/* create status window */
	screen.status_window.rows = 1;
	screen.status_window.cols = screen.cols;
	screen.status_window.w = newwin(screen.status_window.rows,
					 screen.status_window.cols,
					 screen.rows-1,
					 0);

	leaveok(screen.status_window.w, FALSE);
	keypad(screen.status_window.w, TRUE);

	if (options.enable_colors) {
		/* set background attributes */
		wbkgd(stdscr, COLOR_PAIR(COLOR_LIST));
		wbkgd(screen.main_window.w,     COLOR_PAIR(COLOR_LIST));
		wbkgd(screen.top_window.w,      COLOR_PAIR(COLOR_TITLE));
		wbkgd(screen.progress_window.w, COLOR_PAIR(COLOR_PROGRESSBAR));
		wbkgd(screen.status_window.w,   COLOR_PAIR(COLOR_STATUS));
		colors_use(screen.progress_window.w, COLOR_PROGRESSBAR);
	}

	refresh();
}

void
screen_init(mpdclient_t *c)
{
	guint i;

	/* initialize screens */
	for (i = 0; i < NUM_SCREENS; ++i) {
		const struct screen_functions *fn = screens[i].functions;

		if (fn->init)
			fn->init(screen.main_window.w,
				 screen.main_window.cols,
				 screen.main_window.rows);
	}

	if (mode_fn->open != NULL)
		mode_fn->open(&screen, c);

	/* initialize wreadln */
	wrln_wgetch = my_wgetch;
	wrln_max_history_length = 16;
}

void
screen_paint(mpdclient_t *c)
{
	const char *title = NULL;

	if (mode_fn->get_title != NULL)
		title = mode_fn->get_title(screen.buf, screen.buf_size);

	D("screen_paint(%s)\n", title);
	/* paint the title/header window */
	if( title )
		paint_top_window(title, c, 1);
	else
		paint_top_window("", c, 1);

	/* paint the main window */
	wclear(screen.main_window.w);
	if (mode_fn->paint != NULL)
		mode_fn->paint(&screen, c);

	paint_progress_window(c);
	paint_status_window(c);
	screen.painted = 1;
	wmove(screen.main_window.w, 0, 0);
	wnoutrefresh(screen.main_window.w);

	/* tell curses to update */
	doupdate();
}

void
screen_update(mpdclient_t *c)
{
	static int repeat = -1;
	static int random_enabled = -1;
	static int crossfade = -1;
	static int dbupdate = -1;

	if( !screen.painted )
		return screen_paint(c);

	/* print a message if mpd status has changed */
	if (c->status != NULL) {
		if (repeat < 0) {
			repeat = c->status->repeat;
			random_enabled = c->status->random;
			crossfade = c->status->crossfade;
			dbupdate = c->status->updatingDb;
		}

		if (repeat != c->status->repeat)
			screen_status_printf(c->status->repeat ?
					     _("Repeat is on") :
					     _("Repeat is off"));

		if (random_enabled != c->status->random)
			screen_status_printf(c->status->random ?
					     _("Random is on") :
					     _("Random is off"));

		if (crossfade != c->status->crossfade)
			screen_status_printf(_("Crossfade %d seconds"), c->status->crossfade);

		if (dbupdate && dbupdate != c->status->updatingDb) {
			screen_status_printf(_("Database updated!"));
			mpdclient_browse_callback(c, BROWSE_DB_UPDATED, NULL);
		}

		repeat = c->status->repeat;
		random_enabled = c->status->random;
		crossfade = c->status->crossfade;
		dbupdate = c->status->updatingDb;
	}

	/* update title/header window */
	if (welcome && screen.last_cmd==CMD_NONE &&
	    time(NULL)-screen.start_timestamp <= SCREEN_WELCOME_TIME)
		paint_top_window("", c, 0);
	else if (mode_fn->get_title != NULL) {
		paint_top_window(mode_fn->get_title(screen.buf,screen.buf_size), c, 0);
		welcome = FALSE;
	} else
		paint_top_window("", c, 0);

	/* update the main window */
	if (mode_fn->update != NULL)
		mode_fn->update(&screen, c);

	/* update progress window */
	paint_progress_window(c);

	/* update status window */
	paint_status_window(c);

	/* move the cursor to the origin */
	wmove(screen.main_window.w, 0, 0);
	wnoutrefresh(screen.main_window.w);

	/* tell curses to update */
	doupdate();
}

void
screen_idle(mpdclient_t *c)
{
	if (c->song && seek_id == c->song->id &&
	    (screen.last_cmd == CMD_SEEK_FORWARD ||
	     screen.last_cmd == CMD_SEEK_BACKWARD))
		mpdclient_cmd_seek(c, seek_id, seek_target_time);

	screen.last_cmd = CMD_NONE;
	seek_id = -1;
}

#ifdef HAVE_GETMOUSE
int
screen_get_mouse_event(mpdclient_t *c,
		       list_window_t *lw, int lw_length,
		       unsigned long *bstate, int *row)
{
	MEVENT event;

	/* retreive the mouse event from ncurses */
	getmouse(&event);
	D("mouse: id=%d  y=%d,x=%d,z=%d\n",event.id,event.y,event.x,event.z);
	/* calculate the selected row in the list window */
	*row = event.y - screen.top_window.rows;
	/* copy button state bits */
	*bstate = event.bstate;
	/* if button 2 was pressed switch screen */
	if (event.bstate & BUTTON2_CLICKED) {
		screen_cmd(c, CMD_SCREEN_NEXT);
		return 1;
	}

	/* if the even occured above the list window move up */
	if (*row < 0 && lw) {
		if (event.bstate & BUTTON3_CLICKED)
			list_window_first(lw);
		else
			list_window_previous_page(lw);
		return 1;
	}

	/* if the even occured below the list window move down */
	if ((unsigned)*row >= lw->rows && lw) {
		if (event.bstate & BUTTON3_CLICKED)
			list_window_last(lw, lw_length);
		else
			list_window_next_page(lw, lw_length);
		return 1;
	}

	return 0;
}
#endif

static int
screen_client_cmd(mpdclient_t *c, command_t cmd)
{
	if (c->connection == NULL || c->status == NULL)
		return 0;

	switch(cmd) {
	case CMD_PLAY:
		mpdclient_cmd_play(c, MPD_PLAY_AT_BEGINNING);
		break;
	case CMD_PAUSE:
		mpdclient_cmd_pause(c, !IS_PAUSED(c->status->state));
		break;
	case CMD_STOP:
		mpdclient_cmd_stop(c);
		break;
	case CMD_SEEK_FORWARD:
		if (!IS_STOPPED(c->status->state)) {
			if (c->song && seek_id != c->song->id) {
				seek_id = c->song->id;
				seek_target_time = c->status->elapsedTime;
			}
			seek_target_time+=options.seek_time;
			if (seek_target_time < c->status->totalTime)
				break;
			seek_target_time = c->status->totalTime;
			/* seek_target_time=0; */
		}
		break;
		/* fall through... */
	case CMD_TRACK_NEXT:
		if (!IS_STOPPED(c->status->state))
			mpdclient_cmd_next(c);
		break;
	case CMD_SEEK_BACKWARD:
		if (!IS_STOPPED(c->status->state)) {
			if (seek_id != c->song->id) {
				seek_id = c->song->id;
				seek_target_time = c->status->elapsedTime;
			}
			seek_target_time-=options.seek_time;
			if (seek_target_time < 0)
				seek_target_time=0;
		}
		break;
	case CMD_TRACK_PREVIOUS:
		if (!IS_STOPPED(c->status->state))
			mpdclient_cmd_prev(c);
		break;
	case CMD_SHUFFLE:
		if (mpdclient_cmd_shuffle(c) == 0)
			screen_status_message(_("Shuffled playlist!"));
		break;
	case CMD_CLEAR:
		if (mpdclient_cmd_clear(c) == 0)
			screen_status_message(_("Cleared playlist!"));
		break;
	case CMD_REPEAT:
		mpdclient_cmd_repeat(c, !c->status->repeat);
		break;
	case CMD_RANDOM:
		mpdclient_cmd_random(c, !c->status->random);
		break;
	case CMD_CROSSFADE:
		if (c->status->crossfade)
			mpdclient_cmd_crossfade(c, 0);
		else
			mpdclient_cmd_crossfade(c, options.crossfade_time);
		break;
	case CMD_DB_UPDATE:
		if (!c->status->updatingDb) {
			if( mpdclient_cmd_db_update_utf8(c,NULL)==0 )
				screen_status_printf(_("Database update started!"));
		} else
			screen_status_printf(_("Database update running..."));
		break;
	case CMD_VOLUME_UP:
		if( c->status->volume!=MPD_STATUS_NO_VOLUME && c->status->volume<100 )
			mpdclient_cmd_volume(c, ++c->status->volume);
		break;
	case CMD_VOLUME_DOWN:
		if( c->status->volume!=MPD_STATUS_NO_VOLUME && c->status->volume>0 )
			mpdclient_cmd_volume(c, --c->status->volume);
		break;

	default:
		return 0;
	}

	return 1;
}

void
screen_cmd(mpdclient_t *c, command_t cmd)
{
	screen.input_timestamp = time(NULL);
	screen.last_cmd = cmd;
	welcome = FALSE;

	if (mode_fn->cmd != NULL && mode_fn->cmd(&screen, c, cmd))
		return;

	if (screen_client_cmd(c, cmd))
		return;

	switch(cmd) {
	case CMD_TOGGLE_FIND_WRAP:
		options.find_wrap = !options.find_wrap;
		screen_status_printf(options.find_wrap ?
				     _("Find mode: Wrapped") :
				     _("Find mode: Normal"));
		break;
	case CMD_TOGGLE_AUTOCENTER:
		options.auto_center = !options.auto_center;
		screen_status_printf(options.auto_center ?
				     _("Auto center mode: On") :
				     _("Auto center mode: Off"));
		break;
	case CMD_SCREEN_UPDATE:
		screen.painted = 0;
		break;
	case CMD_SCREEN_PREVIOUS:
		screen_next_mode(c, -1);
		break;
	case CMD_SCREEN_NEXT:
		screen_next_mode(c, 1);
		break;
	case CMD_SCREEN_PLAY:
		switch_screen_mode(SCREEN_PLAYLIST_ID, c);
		break;
	case CMD_SCREEN_FILE:
		switch_screen_mode(SCREEN_BROWSE_ID, c);
		break;
	case CMD_SCREEN_HELP:
		switch_screen_mode(SCREEN_HELP_ID, c);
		break;
	case CMD_SCREEN_SEARCH:
		switch_screen_mode(SCREEN_SEARCH_ID, c);
		break;
	case CMD_SCREEN_ARTIST:
		switch_screen_mode(SCREEN_ARTIST_ID, c);
		break;
	case CMD_SCREEN_KEYDEF:
		switch_screen_mode(SCREEN_KEYDEF_ID, c);
		break;
	case CMD_SCREEN_CLOCK:
		switch_screen_mode(SCREEN_CLOCK_ID, c);
		break;
	case CMD_SCREEN_LYRICS:
		switch_screen_mode(SCREEN_LYRICS_ID, c);
		break;
	case CMD_QUIT:
		exit(EXIT_SUCCESS);
	default:
		break;
	}
}
