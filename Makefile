.POSIX:

TARGETS=	mess simpler
MANPAGE=	mess.1
VERSION_HDR=	version.h

MESS_SRCS=	main.c dispatcher.c pager.c nav.c offscr.c readq.c uri.c \
		links.c styler.c strbuf.c log.c
MESS_HDRS=	dispatcher.h pager.h nav.h offscr.h readq.h uri.h strbuf.h

TESTS_SRC=	tests/test-readq.c \
		tests/test-uri.c \
		tests/test-offscr.c \
		tests/test-styler.c \
		tests/test-links.c \
		tests/test-strbuf.c
TESTS_BIN=	tests/test-readq.bin \
		tests/test-uri.bin \
		tests/test-offscr.bin \
		tests/test-styler.bin \
		tests/test-links.bin \
		tests/test-strbuf.bin

CC=		cc
#CFLAGS=		-O2 -g
CFLAGS=		-O0 -g3
CFLAGS+= 	-I. -std=c11 -Wall -Wextra -Werror

OS=		$(shell uname -s)
CSOURCE=	$(shell if [ $(OS) = Linux ]; then echo _GNU_SOURCE; \
		elif [ $(OS) = Darwin ]; then echo _DARWIN_C_SOURCE; \
		elif [ $(OS) = NetBSD ]; then echo _NETBSD_SOURCE; \
		else echo _POSIX_C_SOURCE=200809L; fi)
CFLAGS+=	-D$(CSOURCE)

# libutil is needed on BSD/macOS for openpty(), not on Linux.
LDLIBS!=	if [ "$(uname -s)" != Linux ]; \
		then echo '-lutil'; fi

PREFIX=		/usr/local
BINDIR=		$(PREFIX)/bin
MANDIR=		$(PREFIX)/share/man
INSTALL=	install

.PHONY: all clean test install coverage coverage-info format
all: $(TARGETS) $(TESTS_BIN) mess.1

clean:
	rm -rf $(TARGETS) $(TESTS_BIN) *.o version.h mess.1
	rm -rf *.dSYM tests/*.dSYM tests/integration/*.dSYM
	rm -rf *.gcno tests/*.gcno tests/integration/*.gcno
	rm -rf *.gcda tests/*.gcda tests/integration/*.gcda
	rm -rf *.gcov tests/*.gcov tests/integration/*.gcov
	${MAKE} -C tests/runner clean

version.h: version.h.in
	./versionize.sh version.h.in > $@

mess.1: mess.1.in
	./versionize.sh mess.1.in > $@

mess: $(MESS_SRCS) version.h
	$(CC) $(CFLAGS) -o $@ $(MESS_SRCS) $(LDLIBS)

simpler: simpler.o nav.o links.o offscr.o styler.o strbuf.o readq.o log.o | version.h
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(TARGETS) $(TESTS_BIN)
	@set -e; \
	for t in $(TESTS_BIN); do \
		echo "Running $$t"; \
		if echo "$$t" | grep -q 'integration'; then \
			$${RUN_INTEGRATION:-} ./$$t; \
		else \
			./$$t; \
		fi; \
	done
	@for f in tests/dispatcher/*.c; do \
		tikl -v -c tests/dispatcher/tikl.conf $$f; \
	done
	${MAKE} -C tests/runner all test

coverage:
	@$(MAKE) clean
	@$(MAKE) CFLAGS="$(CFLAGS) -fprofile-arcs -ftest-coverage" LDFLAGS="$(LDFLAGS) -fprofile-arcs -ftest-coverage" tests/test-links.bin
	#@$(MAKE) test

coverage-info:
	@if ! command -v gcovr >/dev/null 2>&1; then \
		echo "gcovr not found. Please install gcovr to generate coverage summaries." >&2; \
		exit 1; \
	fi
	@gcovr --root . --exclude-directories tikl

install: mess $(MANPAGE)
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 755 mess "$(DESTDIR)$(BINDIR)/mess"
	$(INSTALL) -d "$(DESTDIR)$(MANDIR)/man1"
	$(INSTALL) -m 644 $(MANPAGE) "$(DESTDIR)$(MANDIR)/man1/mess.1"

tests/test-links.bin: tests/test-links.c links.c strbuf.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

tests/tes-offscr.bin: tests/tes-offscr.c links.c offscr.c readq.c uri.c strbuf.c log.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

tests/test-styler.bin: tests/test-styler.c styler.o strbuf.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

tests/test-strbuf.bin: tests/test-strbuf.c strbuf.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test-%.bin: tests/test-%.c links.c offscr.c readq.c uri.c strbuf.c log.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

format:
	@find . -name '*.h' -exec astyle --options=.astylerc {} +
	@find . -name '*.c' -exec astyle --options=.astylerc {} +
