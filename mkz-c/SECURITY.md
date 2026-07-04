# Security Policy

## Supported versions
mkz is at 0.1.x; security fixes land on the latest 0.1 release.

| Version | Supported |
|---------|-----------|
| 0.1.x   | yes       |

## The security-relevant surface
mkz's threat model centers on the **decoder** (extract), because it parses untrusted
archive bytes. It is written defensively:

- every length, offset, and index read from the archive is bounds- and overflow-checked
  before use;
- the decompressed block size is bomb-capped;
- compressed payload lengths are bounded against the actual input;
- the extract sink rejects path traversal (absolute paths, `..`, `.`, empty paths, and
  embedded NUL);
- nothing is written to disk unless the whole stream hashes back to the original (SHA-256);
- the decode and streaming paths are built and exercised under
  `-fsanitize=address,undefined` and are clean on both valid and hostile input.

The encoder consumes trusted (local) data and is lower-risk, but is run under the same
sanitizers.

## Reporting a vulnerability
Please report privately, not in a public issue:

- GitHub: use "Report a vulnerability" (Security Advisories) on the repository, or
- email the maintainer at mk@ntele.net.

Expect acknowledgement within a few days. We will agree on a disclosure timeline, fix the
issue, and credit you in the release notes unless you would rather stay anonymous.
