# Changelog

## Unreleased

### API

- `ubi_leb_read()` with a length of zero succeeds without a buffer, as its
  description says and as `ubi_leb_change()` and `ubi_leb_write_at()` already
  do; it used to return `-EINVAL` when the buffer was `NULL`.

## v0.1.3 (2026-10-07)

No change to the library or to the on-flash format.

### CI

- A run that GitHub failed although every job passed, for example by never
  starting a job, is re-run once in full.

## v0.1.2 (2026-10-06)

No change to the library code or to the on-flash format.

### API

- Every call and type in `include/ubi/ubi.h` and `include/ubi/types.h` is
  described briefly: what it does, what it expects and what each error means.
- `ubi_volume_resize()` says what a shrink needs, `ubi_leb_map()` what it is
  for, and `ubi_maintenance()` what one step of each operation does.
- `struct ubi_device_info` and `struct ubi_volume_info` list their fields by
  topic; code that names the fields is not affected.
- The public headers have no `extern "C"` blocks and no Doxygen groups.
- The `ubi_event_cb_t` and `ubi_state_cb_t` typedefs are gone:
  `struct ubi_config` declares `event_cb` and `state_cb` itself. Code that
  only assigns the callbacks is not affected.

### Documentation

- The README says what Unsorted Block Images are and what they give, and
  lists only the features this UBI has.
- How it works explains logical and physical blocks, and has sections on
  damaged headers, bad blocks and wear levelling.
- Security first explains how the metadata is protected, then gives the
  threat model, what is detected, the limits, good practice for rollback
  detection with a check in pseudocode, and advice on protecting data.
- The key provisioning example imports a fixed key.
- Every page says the same as the API, in shorter and plainer sentences.
- Appends follow one rule everywhere: rising order, until the block is
  changed, unmapped or erased.
- The block state diagram no longer has a resize send a mapped block to
  reclaim.
- The home page has a tile for every guide page, and error codes in tables
  no longer break after the minus sign.
- Diagrams and images open full screen on a click.

## v0.1.1 (2026-10-02)

No change to the library or to the on-flash format.

### Documentation

- The README compares UBI for Zephyr with Linux UBI and lists the
  requirements in tables; adding the module is described in the examples.
- The documentation site shows the version it documents and the commit it was
  built from.

### CI

- A smoke run (project rules, unit tests, integration tests and the sample)
  has to pass before anything else starts.
- The other configurations run in shards side by side. Every job and every long
  step has a time limit, and the Zephyr setup gets a second attempt; a run that
  failed only there is re-run once on fresh runners.
- CI runs on pushes to `main`, on pull requests and on demand.
- Library coverage is merged across the shards and sent to Codecov; the README
  shows it next to the CI and release badges.

## v0.1.0 (2026-09-30)

First release.

### Storage

- One flash partition divided into named volumes, created, resized and removed
  at run time.
- Volumes are read and written in logical blocks; UBI decides where each one
  lives and moves it over time.
- `ubi_leb_change()` replaces a block atomically: after a power cut it holds
  the previous contents or the new ones.
- `ubi_leb_write_at()` appends at an offset the application chooses.
- `ubi_leb_erase()` takes a block and every older copy of it off the flash.

### Lifetime

- Wear is spread across the partition by allocation and by relocating rarely
  rewritten data.
- Erasing, wear levelling, repair and discarding damaged blocks run only when
  the application calls `ubi_maintenance()`, on a budget it chooses.
- A block that fails a write is retired and given another chance later; a
  failed erase makes the device read-only until the next attach.
- Header magics are cleared before an erase, so an erase cut short is
  recognised.

### Integrity and security

- Headers and the volume table carry an AES-CMAC under keys derived with
  HKDF-SHA256 from a PSA key handle; `key_context` separates partitions
  sharing keying material.
- Damage and tampering are reported apart through the event callback; blocks
  damaged behind a valid erase counter header are kept until discarded.
- Two copies of the volume table, each updated into a fresh block; the device
  carries on with one and reports it.
- A state callback, at attach and every `CONFIG_UBI_STATE_CHECK_INTERVAL`
  writes, decides whether to trust the device; a refusal leaves it read-only.
- Attach writes nothing, and only `-ENODEV` means a partition holds nothing
  to lose.
- Application data is not encrypted.

### Compatibility

- On-flash format version 1, with headers laid out as in Linux UBI.
- Zephyr 4.4 with PSA Crypto; tested on `native_sim` and on the nRF5340 DK's
  external flash.
