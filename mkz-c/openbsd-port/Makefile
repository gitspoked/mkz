COMMENT =	reversible columnar pre-pass + zstd archiver

V =		0.1.0
DISTNAME =	mkz-${V}
CATEGORIES =	archivers

# TODO: set to the real project page before submission.
HOMEPAGE =	https://example.org/mkz/

# TODO: set to the port maintainer's address before submission.
MAINTAINER =	Matthew Klein <maintainer@example.org>

# mkz is dual-licensed MIT OR Apache-2.0.
PERMIT_PACKAGE =	Yes

# Links libc and libzstd only (SHA-256 and base-95 are vendored).
WANTLIB =		c zstd

# TODO: set to wherever mkz-${V}.tar.gz is published, then run `make makesum`.
MASTER_SITES =		https://example.org/distfiles/

LIB_DEPENDS =		archivers/zstd

# Plain handful of C files — no configure, and no GNU make needed for the port build.
NO_TEST =		Yes

do-build:
	cd ${WRKSRC} && ${CC} ${CFLAGS} -std=c11 -I${LOCALBASE}/include \
		-o mkz mkz.c stream.c pas1.c sha256.c autocol.c archive.c \
		-L${LOCALBASE}/lib -lzstd

do-install:
	${INSTALL_PROGRAM} ${WRKSRC}/mkz ${PREFIX}/bin/mkz
	${INSTALL_MAN} ${WRKSRC}/mkz.1 ${PREFIX}/man/man1/mkz.1

.include <bsd.port.mk>
