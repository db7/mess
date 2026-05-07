.POSIX:
.SUFFIXES: .bin .o

ROOTDIR=		../..
TIKLCFG=		${ROOTDIR}/tests/tikl.conf
CFLAGS+=		-I${ROOTDIR}
TARGETS=

TARGETS+=		run_offscr.bin
DEPS.run_offscr=	${ROOTDIR}/offscr.o

TARGETS+=		run_links.bin
DEPS.run_links=		${ROOTDIR}/links.o ${ROOTDIR}/offscr.o

TARGETS+=		run_nav.bin
DEPS.run_nav=		${ROOTDIR}/nav.o ${ROOTDIR}/links.o ${ROOTDIR}/offscr.o

all: ${TARGETS}

clean:
	rm -rf *.o ${TARGETS}

test:
	tikl -c ${TIKLCFG} *.c

.o.bin:
	${CC} ${CFLAGS} -o $@ $< ${DEPS.$*}
