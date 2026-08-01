# mkz RFCs

This directory holds design records for format, protocol, and product-surface
changes that affect mkz, autocol, or the wider PSRC reconstruction work.

RFCs are not release notes. They are the place to state the contract before
code lands:

- what problem is being solved
- what the decoder must prove
- what is intentionally out of scope
- how older readers fail
- how the change is benchmarked
- how Rust and C stay compatible

## Index

| RFC | Status | Title |
|-----|--------|-------|
| [RFC-0001](RFC-0001-reconstruction-fabric.md) | Draft | Reconstruction Fabric for Snapshot-Aware Sync |
| [RFC-0002](RFC-0002-pas2-seekable-segments.md) | Draft | PAS2 Seekable Segments |
| [RFC-0003](RFC-0003-autocol-v2-field-aware-lanes.md) | Draft | autocol v2 Field-Aware Lanes |
