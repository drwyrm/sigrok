/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2011 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#ifdef _WIN32
#include <conio.h>
#else
#include <termios.h>
#endif
#include <unistd.h>
#include <string.h>
#include <glib.h>
#include <sigrok.h>
#include "sigrok-cli.h"

extern int end_acquisition;

#ifdef _WIN32
HANDLE stdin_handle;
DWORD stdin_mode;
#else
struct termios term_orig;
#endif

static int received_anykey(int fd, int revents, void *user_data)
{
	/* Avoid compiler warnings. */
	fd = fd;
	revents = revents;
	user_data = user_data;

	end_acquisition = TRUE;

	return TRUE;
}

/* Turn off buffering on stdin. */
void add_anykey(void)
{
#ifdef _WIN32
	stdin_handle = GetStdHandle(STD_INPUT_HANDLE);

	if (!GetConsoleMode(stdin_handle, &stdin_mode)) {
		/* TODO: Error handling. */
	}

	SetConsoleMode(stdin_handle, 0);
#else
	struct termios term;

	tcgetattr(STDIN_FILENO, &term);
	memcpy(&term_orig, &term, sizeof(struct termios));
	term.c_lflag &= ~(ECHO | ICANON | ISIG);
	tcsetattr(STDIN_FILENO, TCSADRAIN, &term);
#endif

	add_source(STDIN_FILENO, G_IO_IN, -1, received_anykey, NULL);

	printf("Press any key to stop acquisition.\n");
}

/* Restore stdin attributes. */
void clear_anykey(void)
{
#ifdef _WIN32
	SetConsoleMode(stdin_handle, stdin_mode);
#else
	tcflush(STDIN_FILENO, TCIFLUSH);
	tcsetattr(STDIN_FILENO, TCSANOW, &term_orig);
#endif
}
