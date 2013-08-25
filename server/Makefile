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

include libsvc/libsvc.mk

prefix ?= /usr/local

BUILDDIR = ${CURDIR}/build

PROG=${BUILDDIR}/doozer


CFLAGS  += -Wall -Werror -Wwrite-strings -Wno-deprecated-declarations 
CFLAGS  += -Wmissing-prototypes -std=gnu99
CFLAGS  += $(shell mysql_config --cflags)
CFLAGS  += $(shell pkg-config --cflags libcurl)

LDFLAGS += -lpthread -lssl -lcrypto
LDFLAGS += $(shell mysql_config --libs_r)
LDFLAGS += -L${BUILDDIR}/libgit2/lib -lgit2
LDFLAGS += $(shell pkg-config --libs libcurl)
LDFLAGS += -lssl -lbz2

SRCS =  src/main.c \
	src/artifact_serve.c \
	src/project.c \
	src/buildmaster.c \
	src/git.c \
	src/releasemaker.c \
	src/github.c \
	src/restapi.c \
	src/cmd.c \

SRCS += ${libunixservice_SRCS:%.c=libsvc/%.c}

SRCS += src/bsdiff.c

# Various transformations
SRCS  += $(SRCS-yes)
DLIBS += $(DLIBS-yes)
SLIBS += $(SLIBS-yes)
OBJS=    $(SRCS:%.c=$(BUILDDIR)/%.o)
OBJS_EXTRA = $(SRCS_EXTRA:%.c=$(BUILDDIR)/%.so)
DEPS=    ${OBJS:%.o=%.d}

# Common CFLAGS for all files
CFLAGS_com  = -g -funsigned-char -O2 -D_FILE_OFFSET_BITS=64
CFLAGS_com += -I${BUILDDIR} -I${CURDIR}
CFLAGS_com += -I${BUILDDIR}/libgit2/include/

all: ${PROG}

.PHONY:	clean distclean

${PROG}: $(OBJS) ${OBJS_EXTRA} Makefile
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) ${LDFLAGS_cfg}

${BUILDDIR}/%.o: %.c Makefile ${BUILDDIR}/libgit2/include/git2.h
	@mkdir -p $(dir $@)
	$(CC) -MD -MP $(CFLAGS_com) $(CFLAGS) -c -o $@ $(CURDIR)/$<

clean:
	rm -rf ${BUILDDIR}/src
	find . -name "*~" | xargs rm -f

distclean: clean
	rm -rf build.*

install: ${PROG}
	install -D ${PROG} "${prefix}/bin/doozer"

uninstall:
	rm -f "${prefix}/bin/doozer"

# Include dependency files if they exist.
-include $(DEPS)

${BUILDDIR}/libgit2/include/git2.h:
	mkdir -p ${BUILDDIR}/libgit2/build
	cd ${BUILDDIR}/libgit2/build && cmake ${CURDIR}/libgit2 -DCMAKE_INSTALL_PREFIX=${BUILDDIR}/libgit2 -DBUILD_SHARED_LIBS=OFF -DTHREADSAFE=ON
	cd ${BUILDDIR}/libgit2/build && cmake --build . --target install
