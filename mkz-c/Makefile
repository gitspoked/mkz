# mkz-c - a dependency-light C port of mkz (autocol + zstd, bit-compatible with the
# Rust crate). Builds with cc + libzstd (+ libcrypto/LibreSSL for SHA-256, or a vendored
# SHA-256). Targets Linux / *BSD (incl. OpenBSD) / macOS.
#
# SPDX-License-Identifier: MIT OR Apache-2.0

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
# zstd is the only hard external dep (SHA-256 is vendored in sha256.c).
ZSTD := $(shell pkg-config --cflags --libs libzstd 2>/dev/null || echo -lzstd)

V       ?= 0.1.4
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/man/man1
INSTALL ?= install

# release tarball contents (self-contained: sources + tests + docs + licenses)
DISTNAME := mkz-$(V)
DIST_SRC  := base95.c base95.h b95u16.c b95u16.h sha256.c sha256.h pas1.c pas1.h \
             autocol.c autocol.h archive.c archive.h stream.c stream.h mkz.c
DIST_TEST := test_base95.c test_b95u16.c test_b95_vectors.c test_vectors_b95.txt \
             test_pas1.c test_autocol.c test_acenc.c test_extract.c
DIST_DOC  := Makefile README.md TLDR.md INSTALL install.sh \
             CHANGELOG.md SECURITY.md NOTICE COPYING mkz.1
DIST_LIC  := LICENSE-MIT LICENSE-APACHE

# `make` builds the CLI by default.
all: mkz

# --- foundation: base-95 + b95u16 (no external deps) ---
test_base95: base95.c test_base95.c base95.h
	$(CC) $(CFLAGS) -o $@ base95.c test_base95.c

test_b95u16: b95u16.c test_b95u16.c b95u16.h
	$(CC) $(CFLAGS) -o $@ b95u16.c test_b95u16.c

# --- ABI pin: both codecs against the shared vector file (Qubed pins to a copy) ---
test_b95_vectors: base95.c b95u16.c test_b95_vectors.c base95.h b95u16.h
	$(CC) $(CFLAGS) -o $@ base95.c b95u16.c test_b95_vectors.c

# --- container: PAS1 framing + libzstd + vendored SHA-256 (+ autocol for flags&1 blocks) ---
test_pas1: pas1.c sha256.c autocol.c test_pas1.c pas1.h sha256.h autocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ pas1.c sha256.c autocol.c test_pas1.c $(ZSTD)

# --- transform: autocol decode, built with sanitizers (the decoder is the safety surface) ---
test_autocol: autocol.c test_autocol.c autocol.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ autocol.c test_autocol.c

# --- transform: autocol encode, built with sanitizers (catches leaks/overflow in encode) ---
test_acenc: autocol.c test_acenc.c autocol.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ autocol.c test_acenc.c

# --- streaming extract: atomicity via staging dir (verify SHA-256, then place) ---
test_extract: stream.c pas1.c sha256.c autocol.c archive.c test_extract.c stream.h pas1.h sha256.h autocol.h archive.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ stream.c pas1.c sha256.c autocol.c archive.c test_extract.c $(ZSTD)

# --- the CLI: create + extract real .mkz archives, streaming (bounded memory for typical text/logs) ---
# Honors CPPFLAGS / CFLAGS / LDFLAGS so distro hardening flags (RELRO, PIE, FORTIFY) apply.
mkz: mkz.c stream.c pas1.c sha256.c autocol.c archive.c stream.h pas1.h sha256.h autocol.h archive.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ mkz.c stream.c pas1.c sha256.c autocol.c archive.c $(LDFLAGS) $(ZSTD)

# --- streaming create/extract under sanitizers (the streaming decode path is untrusted) ---
mkz.asan: mkz.c stream.c pas1.c sha256.c autocol.c archive.c stream.h pas1.h sha256.h autocol.h archive.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -o $@ mkz.c stream.c pas1.c sha256.c autocol.c archive.c $(LDFLAGS) $(ZSTD)

.PHONY: all check install uninstall dist clean
check: test_base95 test_b95u16 test_b95_vectors test_pas1 test_extract
	./test_base95
	./test_b95u16
	./test_b95_vectors
	./test_pas1
	./test_extract

install: mkz
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	$(INSTALL) -m 755 mkz $(DESTDIR)$(BINDIR)/mkz
	$(INSTALL) -m 644 mkz.1 $(DESTDIR)$(MANDIR)/mkz.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/mkz $(DESTDIR)$(MANDIR)/mkz.1

# Build the release distfile (mkz-$(V).tar.gz) and print its SHA-256 + size.
dist:
	rm -rf $(DISTNAME) $(DISTNAME).tar.gz
	mkdir -p $(DISTNAME)
	cp $(DIST_SRC) $(DIST_TEST) $(DIST_DOC) $(DISTNAME)/
	cp $(DIST_LIC) $(DISTNAME)/
	tar czf $(DISTNAME).tar.gz $(DISTNAME)
	rm -rf $(DISTNAME)
	@echo "== $(DISTNAME).tar.gz =="
	@sha256sum $(DISTNAME).tar.gz 2>/dev/null || shasum -a 256 $(DISTNAME).tar.gz
	@echo "SIZE: `wc -c < $(DISTNAME).tar.gz` bytes"

clean:
	rm -f test_base95 test_b95u16 test_b95_vectors test_pas1 test_extract test_autocol test_acenc mkz mkz.asan *.o
	rm -rf $(DISTNAME) $(DISTNAME).tar.gz
