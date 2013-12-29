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


BUILDDIR = ${CURDIR}/build

PROG=${BUILDDIR}/doozeragent

LDFLAGS += -L${BUILDDIR}/libgit2/lib -lgit2 -lm

SRCS =  agent/main.c \
	agent/agent.c \

CFLAGS += -I${BUILDDIR}/libgit2/include/


install: ${PROG}
	install -D ${PROG} "${prefix}/bin/doozeragent"
uninstall:
	rm -f "${prefix}/bin/doozeragent"

include libsvc/libsvc.mk
-include $(DEPS)

${BUILDDIR}/libgit2/include/git2.h:
	mkdir -p ${BUILDDIR}/libgit2/build
	cd ${BUILDDIR}/libgit2/build && cmake ${CURDIR}/libgit2 -DCMAKE_INSTALL_PREFIX=${BUILDDIR}/libgit2 -DBUILD_SHARED_LIBS=OFF -DTHREADSAFE=ON
	cd ${BUILDDIR}/libgit2/build && cmake --build . --target install
