.POSIX:

VERSION=	0.1.0
TARGETS=	mess

TESTS_SRC=	tests/test-nav.c \
		tests/test-parser.c \
		tests/test-reader.c \
		tests/test-uri.c

TESTS_BIN=	tests/test-nav.bin \
		tests/test-parser.bin \
		tests/test-reader.bin \
		tests/test-uri.bin

CFLAGS=		-O2 -g
CFLAGS+= 	-I. -std=c11 -Wall -Wextra -Werror
CFLAGS+=	-DMESS_VERSION=\"$(VERSION)\"


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

.PHONY: all clean test install
all: $(TARGETS) $(TESTS_BIN)

clean:
	rm -rf $(TARGETS) $(TESTS_BIN)
	rm -rf *.dSYM tests/*.dSYM tests/integration/*.dSYM

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

mess: main.c dispatcher.c pager.c nav.c readq.c uri.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

install: mess mess.1
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 755 mess "$(DESTDIR)$(BINDIR)/mess"
	$(INSTALL) -d "$(DESTDIR)$(MANDIR)/man1"
	$(INSTALL) -m 644 mess.1 "$(DESTDIR)$(MANDIR)/man1/mess.1"

tests/test-%.bin: tests/test-%.c nav.c readq.c uri.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

format:
	@find . -name '*.h' -exec astyle --options=.astylerc {} +
	@find . -name '*.c' -exec astyle --options=.astylerc {} +
