/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2010 Bert Vermeulen <bert@biot.com>
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

#include <sigrok.h>

extern struct output_format output_text_bits;
extern struct output_format output_text_hex;
extern struct output_format output_binary;
extern struct output_format output_vcd;
extern struct output_format output_gnuplot;
extern struct output_format output_analog;

struct output_format *output_module_list[] = {
	&output_text_bits,
	&output_text_hex,
	&output_binary,
	&output_vcd,
	&output_gnuplot,
	&output_analog,
	NULL,
};

struct output_format **output_list(void)
{
	return output_module_list;
}
