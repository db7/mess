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
		uri.c links.c styler.c \
		strbuf.c log.c
OBJS=		${SRCS:S/.c/.o/}
OBJS=		${SRCS:.c=.o}

SRCS.test=	$(shell find tests -name '*.c' -depth 1)
SRCS.test!=	find tests -name '*.c' -depth 1
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
	./versionize.sh version.h.in > $@

mess.1: mess.1.in
	./versionize.sh mess.1.in > $@

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
tests/unit-links.bin:	tests/unit-links.o links.o strbuf.o
	${CC} -o $@ tests/unit-links.o links.o strbuf.o ${LDFLAGS}
tests/unit-styler.bin:	tests/unit-styler.o styler.o strbuf.o
	${CC} -o $@ tests/unit-styler.o styler.o strbuf.o ${LDFLAGS}
tests/unit-offscr.bin:	tests/unit-offscr.o links.o offscr.o readq.o uri.o strbuf.o log.o
	${CC} -o $@ tests/unit-offscr.o links.o offscr.o readq.o uri.o strbuf.o log.o ${LDFLAGS}
tests/run-links.bin:	tests/run-links.o links.o offscr.o strbuf.o log.o
	${CC} -o $@ tests/run-links.o links.o offscr.o strbuf.o log.o ${LDFLAGS}
tests/run-offscr.bin:	tests/run-offscr.o offscr.o strbuf.o log.o
	${CC} -o $@ tests/run-offscr.o offscr.o strbuf.o log.o ${LDFLAGS}

.SUFFIXES: .bin
.o.bin:
	${CC} -o $@ $< ${LDFLAGS}

test: all
	@tikl -q -c tests/tikl.conf ${SRCS.test}

# ------------------------------------------------------------------------------
DEPS=	$(shell find . -name '*.d')
DEPS!=	touch version.d && find . -name '*.d'
include ${DEPS}
