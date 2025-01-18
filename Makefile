TARGETS=	lless mess
TESTS_SRC=	$(wildcard test-*.c)
TESTS_BIN=	$(TESTS_SRC:.c=.bin)

CFLAGS=		-I. -O0 -g -Wall -Wextra -Werror
CFLAGS+=	-D_NETBSD_SOURCE -lutil
#CFLAGS+=	-D_GNU_SOURCE

.PHONY: default clean
default: $(TARGETS) $(TESTS_BIN)

clean:
	rm -rf $(TARGETS) $(TESTS_BIN) *.dSYM

lless: main.c nav.c readq.c
	$(CC) $(CFLAGS) -o $@ $^

mess: mess.c
	$(CC) $(CFLAGS) -o $@ $<

test-%.bin: test-%.c nav.c readq.c
	$(CC) $(CFLAGS) -o $@ $^
