# Hilbert / UTF-16 ceremony

Status: design note

## Purpose

Hilbert / UTF-16 is part of the origin story and a useful explanatory model for
shared symbolic reconstruction. It is not the default mkz archive format and it
is not treated as compression by itself.

This note gives the idea a public, bounded home:

```text
Hilbert / UTF-16 = address ceremony
mkz PAS1        = exact archive core
autocol         = reversible shape transform
PAS2            = seekable segment container
fabric          = shared-state reconstruction layer
```

## What It Means

Both endpoints may share a symbolic library: UTF-16 code units, printable key
alphabets, dictionaries, registries, or other pre-agreed model entries. A value
can then be represented by an address into that library.

Hilbert coordinates are one possible address ceremony:

```text
value -> Hilbert distance -> coordinate
coordinate -> Hilbert distance -> value
```

When the mapping is bijective and the endpoints share the same geometry, the
coordinate can recover the value exactly.

## Correct Boundary

The ceremony is useful when it clarifies:

- shared dictionaries
- deterministic coordinates
- visual proof vectors
- reversible examples
- guest-list / room-number metaphors
- reconstruction fabric profiles

The ceremony is not useful when it would:

- make mkz readers understand Unicode geometry
- replace byte-level archive formats
- claim compression without bit accounting
- blur binary payloads with valid text
- turn optional visualization into required wire machinery

## Compression Rule

Hilbert mapping alone is representation, not compression.

A valid compression claim must count:

- original source bits
- coordinate bits
- headers
- lengths
- checksums
- padding
- registry ids
- error-correction data

If the coordinate stream plus overhead is not smaller than the source
representation, the ceremony did not compress the data.

## UTF-16 Rule

UTF-16 text mode and arbitrary binary mode are different.

For text, UTF-16 code units must obey the selected text profile. Surrogate pairs
matter.

For binary, bytes may be packed into 16-bit words and carried through the same
coordinate machinery, but those words are payload units, not Unicode scalar
values.

## Where It Can Plug In Later

The ceremony may be useful as:

- a demo profile for reconstruction fabric
- a human-readable proof vector set
- a coordinate visualization of payload words
- a shared dictionary address scheme
- a teaching layer for exact versus lossy reconstruction

It should not be a required dependency of PAS1, autocol v1/v2, or PAS2.

## Guardrail

The archive decoder stays boring:

```text
decode bytes
verify bytes
accept or reject
```

The ceremony can explain addresses. It must not weaken the verifier.
