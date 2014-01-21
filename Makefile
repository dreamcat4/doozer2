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


WITH_MYSQL       := yes
WITH_HTTP_SERVER := yes
WITH_LIBGIT2     := yes
WITH_CTRLSOCK    := yes
WITH_CURL        := yes

BUILDDIR = ${CURDIR}/build


PROG=${BUILDDIR}/doozerd


SRCS =  server/main.c \
	server/artifact_serve.c \
	server/project.c \
	server/buildmaster.c \
	server/git.c \
	server/releasemaker.c \
	server/github.c \
	server/restapi.c \
	server/s3.c \
	server/bsdiff.c

BUNDLES += sql

install: ${PROG}
	install -D ${PROG} "${prefix}/bin/doozerd"
	install -D -m 755 doozer "${prefix}/bin/doozer"
uninstall:
	rm -f "${prefix}/bin/doozerd" "${prefix}/bin/doozer"

include libsvc/libsvc.mk
-include config.local
-include $(DEPS)

