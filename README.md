# Unsorted Block Images (UBI) for Zephyr

[![CI / Zephyr](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/ci.yml)
[![CI / Documentation](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/docs.yml/badge.svg?branch=main)](https://github.com/kamil-kielbasa/zephyr-ubi/actions/workflows/docs.yml)
[![codecov](https://codecov.io/gh/kamil-kielbasa/zephyr-ubi/branch/main/graph/badge.svg)](https://codecov.io/gh/kamil-kielbasa/zephyr-ubi)

[![Docs](https://img.shields.io/badge/docs-GitHub%20Pages-blue)](https://kamil-kielbasa.github.io/zephyr-ubi/)
[![Release](https://img.shields.io/github/v/release/kamil-kielbasa/zephyr-ubi)](https://github.com/kamil-kielbasa/zephyr-ubi/releases)
[![Zephyr](https://img.shields.io/badge/Zephyr-4.4-blueviolet)](https://zephyrproject.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Unsorted Block Images (UBI) is a volume manager for raw flash, ported from
Linux to Zephyr. Raw flash is erased only in whole blocks, each block wears out
after a limited number of erases, and a power cut can leave an update half
done. UBI splits one flash partition into named volumes and stores their
logical blocks in physical erase blocks in any order, hence *unsorted*. Moving
blocks freely lets it spread erases evenly, replace a block atomically and
take failing blocks out of use. Its own metadata is authenticated with
AES-CMAC.

## Features

- **Named volumes** — created, resized and removed at run time.
- **Atomic block updates** — `ubi_leb_change()` replaces a logical block
  whole: after a power cut, it holds the old contents or the new.
- **Appends** — `ubi_leb_write_at()` adds data to a logical block without an
  erase.
- **Power-cut safety** — volume changes, erases and maintenance steps survive
  a power cut at any point.
- **Wear levelling** — erases are spread over the whole partition, and data
  that does not change is moved so that its block wears too.
- **Bad block handling** — a block that fails a write is taken out of use
  until the next attach; a failed erase makes the device read-only.
- **Authenticated metadata** — block headers and the volume table carry an
  AES-CMAC under a key derived from a PSA Crypto key; damage and tampering are
  reported apart. `CONFIG_UBI_VERIFY_ON_READ` checks the block header again on
  every read.
- **Rollback detection** — a state callback decides whether to trust the
  device, at every attach and every `CONFIG_UBI_STATE_CHECK_INTERVAL` writes.
- **Diagnostic shell** — `CONFIG_UBI_SHELL` adds a read-only `ubi` command
  that shows the LEB to PEB mapping, the state and erase count of every
  block, and the device information.
- **Maintenance on demand** — erasing and wear levelling run in
  `ubi_maintenance()`, on a budget the application chooses; no background
  thread.

Application data is stored as given: neither encrypted nor authenticated.

## Requirements

| Component | Requirement |
|---|---|
| Zephyr | 4.4 |
| Cryptography | PSA Crypto with AES-CMAC, HKDF and SHA-256 (mbedTLS, TF-M or the platform's own) |
| Partition | one fixed partition of 4 to 65534 erase blocks of one size |
| Erase block | larger than the two 64-byte headers and the volume table (320 bytes with 4 volumes) |
| Write block | divides 64 bytes |
| Programming | every write block once after an erase, in any order |
| RAM | a fixed-size handle; on the heap, a scratch buffer and 8 bytes per erase block |
| Stack | under 2 KiB for any call |

Exact figures: [Resources](https://kamil-kielbasa.github.io/zephyr-ubi/operations#resources)
and [Limits](https://kamil-kielbasa.github.io/zephyr-ubi/operations#limits).

## Documentation

Full documentation: <https://kamil-kielbasa.github.io/zephyr-ubi/>.

| Document | What you will find |
|---|---|
| [How it works](https://kamil-kielbasa.github.io/zephyr-ubi/how-it-works) | Logical and physical blocks, attach, damaged headers, bad blocks, wear levelling, maintenance |
| [Security](https://kamil-kielbasa.github.io/zephyr-ubi/security) | How the metadata is protected, threat model, limits, rollback detection, protecting your data |
| [Examples](https://kamil-kielbasa.github.io/zephyr-ubi/examples) | Setup, attaching, volumes, appends, key provisioning, maintenance |
| [Operations](https://kamil-kielbasa.github.io/zephyr-ubi/operations) | Errors, events, power loss, maintenance, key changes, resources |
| [On-flash format](https://kamil-kielbasa.github.io/zephyr-ubi/on-flash-format) | Headers, the volume table and key derivation, byte by byte |
| [API](include/ubi/ubi.h) | The contract of every call |

## License

MIT. See [LICENSE](LICENSE).

## Contact

email: kamkie1996@gmail.com
