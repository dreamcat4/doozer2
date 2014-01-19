#
#  Copyright (C) 2011 Andreas Öman
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
#


WITH_LIBGIT2 := yes
WITH_CURL    := yes


BUILDDIR = ${CURDIR}/build

PROG=${BUILDDIR}/doozeragent

SRCS =  agent/main.c \
	agent/agent.c \
	agent/job.c \
	agent/git.c \
	agent/doozerctrl.c \
	agent/autobuild.c \
	agent/makefile.c \
	agent/artifact.c \
	agent/heap_btrfs.c \


install: ${PROG}
	install -D ${PROG} "${prefix}/bin/doozeragent"
uninstall:
	rm -f "${prefix}/bin/doozeragent"

include libsvc/libsvc.mk
-include $(DEPS)
