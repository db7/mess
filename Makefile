.POSIX:

CC=		cc
CFLAGS=		-O0 -g3 -Wall -Wextra -Werror
CFLAGS.objs=	-std=c11 -MMD -MP
CPPFLAGS=	-I.
LDFLAGS=
LDLIBS=

CFLAGS.cov=	${CFLAGS} --coverage
LDFLAGS.cov=	${LDFLAGS} --coverage

PREFIX=		/usr/local
BINDIR=		${PREFIX}/bin
MANDIR=		${PREFIX}/share/man
INSTALL=	install

SRCS=		main.c dispatcher.c pager.c \
		nav.c offscr.c readq.c \
		uri.c links.c styler.c ansi.c \
		strbuf.c log.c
OBJS=		${SRCS:S/.c/.o/}
OBJS=		${SRCS:.c=.o}

SRCS.test=	$(shell find tests -maxdepth 1 -name '*.c')
SRCS.test!=	find tests -maxdepth 1 -name '*.c'
TGTS.test=	${SRCS.test:S/.c/.bin/}
TGTS.test=	${SRCS.test:.c=.bin}

# ------------------------------------------------------------------------------
# main targets
# ------------------------------------------------------------------------------

all: mess mess.1 ${TGTS.test}

clean:
	rm -rf mess ${DEPS} ${OBJS} ${TGTS.test}
	@find . \( -name '*.dSYM' \
		-o -name '*.d' \
		-o -name '*.o' \
		-o -name '*.bin' \
		-o -name '*.gcda' \
		-o -name '*.gcno' \
		-o -name '*.gcov' \) -exec rm -rf {} +

distclean: clean
	rm -rf version.sh mess.1

format:
	@find . -name '*.h' -exec clang-format -i --style=file {} +
	@find . -name '*.c' -exec clang-format -i --style=file {} +

install: mess mess.1
	${INSTALL} -d "${DESTDIR}${BINDIR}"
	${INSTALL} -d "${DESTDIR}${MANDIR}/man1"
	${INSTALL} -m 755 mess "${DESTDIR}${BINDIR}/mess"
	${INSTALL} -m 644 mess.1 "${DESTDIR}${MANDIR}/man1/mess.1"

coverage: clean
	@${MAKE} CFLAGS="${CFLAGS.cov}" LDFLAGS="${LDFLAGS.cov}" all

version.h: version.h.in
	scripts/versionize.sh -r version.h.in > $@

mess.1: mess.1.in
	scripts/versionize.sh -r mess.1.in > $@

main.o: version.h

mess: ${OBJS}
	${CC} ${CFLAGS} ${CPPFLAGS} -o $@ ${OBJS} ${LDFLAGS} ${LDLIBS}

.c.o:
	${CC} ${CFLAGS.objs} ${CFLAGS} ${CPPFLAGS} -c -o $@ $<

# ------------------------------------------------------------------------------
# tests
# ------------------------------------------------------------------------------

tests/unit-strbuf.bin:	tests/unit-strbuf.o strbuf.o
	${CC} -o $@ tests/unit-strbuf.o strbuf.o ${LDFLAGS}
tests/unit-uri.bin:	tests/unit-uri.o uri.o
	${CC} -o $@ tests/unit-uri.o uri.o ${LDFLAGS}
tests/unit-readq.bin:	tests/unit-readq.o readq.o
	${CC} -o $@ tests/unit-readq.o readq.o ${LDFLAGS}
tests/unit-links.bin:	tests/unit-links.o links.o ansi.o strbuf.o
	${CC} -o $@ tests/unit-links.o links.o ansi.o strbuf.o ${LDFLAGS}
tests/unit-styler.bin:	tests/unit-styler.o styler.o ansi.o strbuf.o
	${CC} -o $@ tests/unit-styler.o styler.o ansi.o strbuf.o ${LDFLAGS}

tests/unit-offscr.bin:	tests/unit-offscr.o links.o offscr.o readq.o uri.o
tests/unit-offscr.bin:	ansi.o strbuf.o log.o
	${CC} -o $@ tests/unit-offscr.o links.o offscr.o readq.o uri.o ansi.o \
			strbuf.o log.o ${LDFLAGS}
tests/run-links.bin:	tests/run-links.o links.o offscr.o ansi.o strbuf.o log.o
	${CC} -o $@ tests/run-links.o links.o offscr.o ansi.o strbuf.o log.o \
		${LDFLAGS}
tests/run-offscr.bin:	tests/run-offscr.o offscr.o ansi.o strbuf.o log.o
	${CC} -o $@ tests/run-offscr.o offscr.o ansi.o strbuf.o log.o ${LDFLAGS}

.SUFFIXES: .bin
.o.bin:
	${CC} -o $@ $< ${LDFLAGS}

# ------------------------------------------------------------------------------
# tikl tests
# ------------------------------------------------------------------------------

TIKL_VERSION=	0.4.2
TIKL_REPO=	https://github.com/db7/tikl
TIKL_URL=	${TIKL_REPO}/archive/refs/tags/v${TIKL_VERSION}.tar.gz
TIKL_SHA256=	24cb5e84fea331dbfd26acae2826850b45ff7c294c6b166b56c73f7d49b6358e
TIKL=		tikl

TIKL_ENSURE_=	scripts/ensure-cmd.sh --workdir deps --candidate ${TIKL}
TIKL_FIND_=	${TIKL_ENSURE_} -q tikl ${TIKL_VERSION}
TIKL_INSTALL_=	${TIKL_ENSURE_} --url ${TIKL_URL} --sha256 ${TIKL_SHA256} \
			tikl ${TIKL_VERSION}

TIKL_CMD=	$(shell ${TIKL_FIND_})
TIKL_CMD!=	${TIKL_FIND_}


tikl-local-install:
	@${TIKL_INSTALL_}

tikl-check:
	@if [ -z "${TIKL_CMD}" ]; then \
		printf '%s\n' "tikl v${TIKL_VERSION} is necessary for tests."; \
		printf '%s\n' "Set TIKL or run 'make tikl-local-install'"; \
		exit 1; \
	fi

test: tikl-check all
	@${TIKL_CMD} -q -c tests/tikl.conf ${SRCS.test}

# ------------------------------------------------------------------------------
DEPS=	$(shell find . -name '*.d')
DEPS!=	touch version.d && find . -name '*.d'
include ${DEPS}

.PHONY: all clean distclean format install coverage tikl-local-install tikl-check test
