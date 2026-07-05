COMMENT =	reversible columnar pre-pass + zstd archiver

V =		0.1.0
DISTNAME =	mkz-${V}
CATEGORIES =	archivers

HOMEPAGE =	https://github.com/gitspoked/mkz

MAINTAINER =	Matt Klein <mk@ntele.net>

# mkz is dual-licensed MIT OR Apache-2.0.
PERMIT_PACKAGE =	Yes

# Links libc and libzstd only (SHA-256 and base-95 are vendored).
WANTLIB =		c zstd

SITES =		https://github.com/gitspoked/mkz/releases/download/v${V}/

# The release tarball extracts to mkz-c/ (the pure-C tree), not ${DISTNAME}/.
WRKSRC =		${WRKDIR}/mkz-c

LIB_DEPENDS =		archivers/zstd

# Plain handful of C files - no configure, and no GNU make needed
# for the port build.

do-build:
	cd ${WRKSRC} && ${CC} ${CFLAGS} -std=c11 -I${LOCALBASE}/include \
		-o mkz mkz.c stream.c pas1.c sha256.c autocol.c archive.c \
		-L${LOCALBASE}/lib -lzstd

do-install:
	${INSTALL_PROGRAM} ${WRKSRC}/mkz ${PREFIX}/bin/mkz
	${INSTALL_MAN} ${WRKSRC}/mkz.1 ${PREFIX}/man/man1/mkz.1

# Upstream's test binaries: base-95, b95u16, and the PAS1 container
# (including hostile-input rejects).
do-test:
	cd ${WRKSRC} && ${CC} ${CFLAGS} -std=c11 -o test_base95 \
		base95.c test_base95.c && ./test_base95
	cd ${WRKSRC} && ${CC} ${CFLAGS} -std=c11 -o test_b95u16 \
		b95u16.c test_b95u16.c && ./test_b95u16
	cd ${WRKSRC} && ${CC} ${CFLAGS} -std=c11 -I${LOCALBASE}/include -o test_pas1 \
		pas1.c sha256.c autocol.c test_pas1.c -L${LOCALBASE}/lib -lzstd && ./test_pas1

.include <bsd.port.mk>
