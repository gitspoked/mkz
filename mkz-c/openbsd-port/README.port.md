# OpenBSD port skeleton for `mkz`

A ready-to-drop `archivers/mkz` port for the pure-C `mkz`. It builds the handful of
C source files directly with `${CC}` (no GNU make needed) and links `libzstd` from
`archivers/zstd`.

```
openbsd-port/
├── Makefile          # the port Makefile (archivers/mkz)
└── pkg/
    ├── DESCR         # package description
    └── PLIST         # packing list (bin/mkz + man/man1/mkz.1)
```

## Dropping it into the ports tree

```sh
mkdir -p /usr/ports/archivers/mkz
cp -R openbsd-port/. /usr/ports/archivers/mkz/
cd /usr/ports/archivers/mkz
```

## Before it will build (three TODOs in the Makefile)

1. **`MASTER_SITES`** — set to wherever `mkz-0.1.0.tar.gz` is published.
   Build the distfile from the source dir with `make dist` (one level up); it prints
   the SHA-256 and size.
2. **`HOMEPAGE`** and **`MAINTAINER`** — set to the real project URL and your address.
3. Fetch the distfile and generate `distinfo`:

   ```sh
   make makesum        # fetches the distfile and writes distinfo (OpenBSD uses base64 SHA-256)
   ```

   Do **not** hand-write `distinfo`: `make makesum` produces the correct base64 digest
   and byte size against the actually-published tarball.

## Build / test / install

```sh
make                 # build
make fake            # stage into ${WRKINST}
make package         # build the package
make install         # install the package
PORTS_PRIVSEP=Yes make package   # (as usual on a normal ports setup)
```

Quality gates before submission:

```sh
make port-lib-depends-check
portcheck                       # from devel/portcheck
make lib-depends-check
```

## Notes

- **Dependencies:** `archivers/zstd` only (`WANTLIB = c zstd`). SHA-256 and base-95
  are vendored, so there is no LibreSSL/crypto dependency.
- **No test target:** the port sets `NO_TEST = Yes`. The upstream tarball ships a
  `make check` (C↔C unit tests), but that path uses GNU make; run it manually with
  `gmake check` if desired.
- **Man page:** `mkz.1` is installed to `man/man1`.
- The build is just six `.c` files; if upstream adds a source file, update the
  `do-build` line in the Makefile and (if it installs anything new) `pkg/PLIST`.
