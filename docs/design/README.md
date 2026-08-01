# mkz Design Docs

This directory holds explanatory design notes that are broader than a release
note but less normative than `FORMAT.md` or `REGISTRY.md`.

Use this split:

- `FORMAT.md`: byte-level format rules already shipped or accepted.
- `REGISTRY.md`: allocated ids and reserved id spaces.
- `ROADMAP.md`: release sequencing and dependency order.
- `docs/rfcs/`: proposed contracts that still need acceptance.
- `docs/design/`: design background, implementation direction, and staging
  notes that inform future RFCs or releases.

## Current Design Records

| Doc | Purpose |
|-----|---------|
| [north-star.md](north-star.md) | Product and architecture direction tying PAS1, PAS2, autocol v2, and reconstruction fabric together. |
| [hilbert-utf16-ceremony.md](hilbert-utf16-ceremony.md) | Bounded public home for the Hilbert / UTF-16 address ceremony and its archive-format guardrails. |
| [pas2-seekable-segments.md](pas2-seekable-segments.md) | Public explainer for the PAS2 seekable segment direction. |
| [autocol-v2-field-aware-lanes.md](autocol-v2-field-aware-lanes.md) | Field-aware autocol v2 lane direction for JSONL and structured records. |
| [mkz-v2-release-staging.md](mkz-v2-release-staging.md) | Rules for using the v2 staging directory after the 0.1.5 release base. |

## Background Notes

The older draft under `docs/superpowers/specs/` is retained as historical
working context. Public-facing design should point to the cleaned docs in this
directory and to accepted RFCs.

## Promotion Rule

A design doc becomes an RFC when it needs a stable accept/reject contract,
cross-implementation compatibility, registry allocation, or old-reader behavior.
