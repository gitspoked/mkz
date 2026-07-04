#!/bin/sh
# mkz installer.
#   - In the C source tree (Makefile + mkz.c present): build with cc + libzstd and
#     `make install` (uses sudo only if the install prefix isn't writable).
#   - Anywhere else, if cargo is available: install the Rust crate (`cargo install mkz`).
# Override the location with PREFIX, e.g.  PREFIX=$HOME/.local ./install.sh
set -eu

PREFIX="${PREFIX:-/usr/local}"

do_make_install() {
    if [ "$(id -u)" = "0" ] || [ -w "$PREFIX" ]; then
        make install PREFIX="$PREFIX"
    elif command -v sudo >/dev/null 2>&1; then
        echo "mkz: installing to $PREFIX (needs sudo)"
        sudo make install PREFIX="$PREFIX"
    else
        echo "mkz: $PREFIX is not writable and sudo is unavailable." >&2
        echo "mkz: retry as root, or set a writable PREFIX (e.g. PREFIX=\$HOME/.local)." >&2
        exit 1
    fi
}

if [ -f Makefile ] && [ -f mkz.c ]; then
    echo "mkz: building the C version (cc + libzstd)"
    make
    do_make_install
    echo "mkz: installed $PREFIX/bin/mkz  (run 'mkz -h')"
    exit 0
fi

if command -v cargo >/dev/null 2>&1; then
    echo "mkz: no C source here, installing the Rust crate with cargo"
    cargo install mkz
    exit 0
fi

echo "mkz: no C source tree and no cargo found." >&2
echo "mkz: unpack the source tarball and run ./install.sh, or install Rust and run: cargo install mkz" >&2
exit 1
