##
## This file is part of the sigrok project.
##
## Copyright (C) 2011 Uwe Hermann <uwe@hermann-uwe.de>
##
## This program is free software: you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation, either version 3 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program.  If not, see <http://www.gnu.org/licenses/>.
##

AM_CPPFLAGS = -DFIRMWARE_DIR='"$(FIRMWARE_DIR)"'

# Local lib, this is NOT meant to be installed!
noinst_LTLIBRARIES = libsigrokoutput.la

libsigrokoutput_la_SOURCES = \
	output_binary.c \
	output_text.c \
	output_vcd.c \
	output_gnuplot.c \
	output_analog.c \
	common.c \
	output.c

libsigrokoutput_la_CFLAGS = \
	-I$(top_srcdir)/libsigrok

