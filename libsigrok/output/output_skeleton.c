/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2010 YOURNAME <YOUREMAIL>
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

#include <stdint.h>
#include <sigrok.h>

static int init(struct output *o)
{
	return 0;
}

static int data(struct output *o, char *data_in, uint64_t length_in,
		char **data_out, uint64_t *length_out)
{
	return SIGROK_OK;
}

static int event(struct output *o, int event_type, char **data_out,
		 uint64_t *length_out)
{
	return SIGROK_OK;
}

struct output_format output_foo = {
	"foo",
	"The foo format",
	DF_LOGIC,
	init,
	data,
	event,
};
