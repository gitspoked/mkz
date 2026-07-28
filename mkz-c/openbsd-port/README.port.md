# OpenBSD port for `mkz`

A ready-to-drop `archivers/mkz` port for the pure-C `mkz`. It uses the ports
framework's standard build/test/install machinery (no hand-rolled targets and no
patches): the upstream `Makefile` compiles the handful of C files with `${CC}`
and links `libzstd` from `archivers/zstd`.

```
openbsd-port/
|-- Makefile          # the port Makefile (archivers/mkz)
|-- distinfo          # SHA-256 + size of the release distfile
`-- pkg/
    |-- DESCR         # package description
    `-- PLIST         # packing list (@bin bin/mkz + man/man1/mkz.1)
```

## Dropping it into the ports tree

```sh
mkdir -p /usr/ports/archivers/mkz
cp -R openbsd-port/. /usr/ports/archivers/mkz/
cd /usr/ports/archivers/mkz
```

## Build / test / install

```sh
make                 # build (cc + libzstd, driven by the framework)
make fake            # stage into ${WRKINST}
make test            # upstream `check`: base-95, b95u16, PAS1 roundtrip + rejects
make package         # build the package
make install         # install it
```

## Quality gates before submission

```sh
make port-lib-depends-check
make lib-depends-check
portcheck                       # from devel/portcheck
```

## Notes

- **Distfile:** `mkz-${V}.tar.gz` roots at `${DISTNAME}/` (the standard layout),
  so no `WRKDIST` override is needed. Regenerate `distinfo` with `make makesum`
  against the published tarball; do not hand-write it.
- **Release order:** push/tag `v0.1.3` from the current release checkout first, with
  `V = 0.1.3`. For `v0.1.4`, bump `V` to `0.1.4` in this Makefile and regenerate
  `distinfo` against the hosted `mkz-0.1.4.tar.gz`. The v2 working tree starts from
  the `v0.1.4` state.
- **Dependencies:** `archivers/zstd` only (`WANTLIB = c zstd`). SHA-256 and
  base-95 are vendored, so there is no LibreSSL/crypto dependency.
- **Restrictive tar modes:** `FIX_EXTRACT_PERMISSIONS = Yes` handles the
  distfile's file modes.
- **Tests:** `TEST_TARGET = check` wires `make test` to the upstream unit tests
  (base-95, b95u16, PAS1 container roundtrip, and the paranoid decoder rejects).
- **Man page:** `mkz.1` installs to `man/man1`.

Validated on OpenBSD 7.9/arm64: `make build`, `fake`, `test`, `package`, and
`port-lib-depends-check` all pass, and the installed binary round-trips.
