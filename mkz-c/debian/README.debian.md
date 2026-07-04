# Debian packaging for `mkz`

A `debian/` skeleton that builds a `.deb` from the upstream tarball using debhelper.
The C sources, `Makefile`, and `mkz.1` man page are reused unchanged — this dir is just
the Debian metadata.

```
debian/
├── control          # source + binary package metadata, deps
├── rules            # debhelper (dh); overrides install paths to /usr
├── changelog        # version (0.1.0-1) — the source of truth for the package version
├── copyright        # DEP-5, dual MIT / Apache-2.0
├── source/format    # 3.0 (quilt) — non-native (upstream tarball + debian/)
└── README.debian.md # this file
```

## Why it's small

- **GNU make is native on Debian**, so the upstream `Makefile` is used directly
  (`dh_auto_build` runs `make`, `dh_auto_test` runs `make check`,
  `dh_auto_install` runs `make install`). No cc-direct workaround like the OpenBSD port.
- The upstream `Makefile` honors `CPPFLAGS`/`CFLAGS`/`LDFLAGS`, so `dpkg-buildflags`
  hardening (RELRO, PIE, FORTIFY) applies automatically.
- The runtime dependency on `libzstd1` is computed automatically by `dh_shlibdeps`
  (`${shlibs:Depends}`) — nothing to hand-list.

## Build (non-native, 3.0 quilt)

From the source dir, build the upstream tarball and lay it out as Debian expects:

```sh
make dist                                   # produces mkz-0.1.0.tar.gz
mkdir -p /tmp/mkzdeb && cp mkz-0.1.0.tar.gz /tmp/mkzdeb/mkz_0.1.0.orig.tar.gz
cd /tmp/mkzdeb && tar xzf mkz_0.1.0.orig.tar.gz
cp -R /path/to/psrc/mkz-c/debian mkz-0.1.0/debian
cd mkz-0.1.0
dpkg-buildpackage -us -uc -b                # binary-only build -> ../mkz_0.1.0-1_*.deb
```

Then check it:

```sh
lintian ../mkz_0.1.0-1_*.deb
dpkg-deb -c ../mkz_0.1.0-1_*.deb            # should list usr/bin/mkz + usr/share/man/man1/mkz.1.gz
sudo dpkg -i ../mkz_0.1.0-1_*.deb && mkz -V
```

## Before uploading anywhere

Set the three placeholders to real values: `Maintainer`/`Upstream-Contact` (in
`control` + `copyright` + `changelog`), and `Homepage`/`Source` URLs. Bump
`debian/changelog` for each release.

Build deps: `debhelper-compat (= 13)`, `libzstd-dev`, `pkg-config`. Install with
`sudo apt build-dep .` (from the source dir) or `sudo apt install debhelper libzstd-dev pkg-config`.
