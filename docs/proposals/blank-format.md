# Format: stamp every block of a verified-blank partition instead of erasing each on first use

_Status: proposal, not implemented._

## Problem

`ubi_device_format()` writes only the image sequence number and the two volume-table copies (`include/ubi/ubi.h:35`). Every other block is left without an erase-counter header, so on attach it is `UBI_PEB_UNKNOWN`. The first write that needs a physical block then goes through `ubi_impl_peb_allocate_for_write()` → `peb_erase_for_write()` → `ubi_impl_peb_prepare()` (`library/ubi_peb.c:275`, `:404`, `:503`), which **erases the block unconditionally** before stamping it, even when the block is factory-blank.

On a new device that means one flash erase per block, spread over the first fill of the volume.

Measured on native_sim with the flash simulator, replaying a store filling up on a ~7 MiB partition of 4 KiB blocks (~1,800 PEBs): **1,709 of the 1,870 erases** during the first fill were these first-use erases of blank blocks. The rest were genuine compactions and metadata writes. Using the MX25R6435F (nRF5340-DK) model, with a 58 ms datasheet-typical 4 KB sector erase (not yet measured on the DK), that is about **99 s of erase time**, roughly 75 % of the whole first fill.

## Proposal

When the caller has verified that **the whole partition reads as erased**, format stamps an erase-counter header (`ec = 0`) on every block, so they all start `FREE`, and erases nothing.

- **Why the risk is limited to a narrow case:** a block can read all 0xFF yet be only weakly erased if an erase was cut short. On a partition where *every* byte is blank and no UBI header exists anywhere, UBI has never erased anything, so it cannot have left a torn erase behind.
- **After format, nothing changes:** every block has a header. A header-less block found on a later attach can only be an erase that was cut short, after `CONFIG_UBI_ERASE_INVALIDATES_HEADERS` zeroed its magic. It keeps today's behaviour: erased before use. No runtime trust and no erase-intent record are needed.
- **Order, so a power cut is safe:** write the volume-table copies first, then stamp the remaining blocks.
  - A cut while stamping leaves an attachable device whose unstamped blocks fall back to erase-on-first-use, so it is only slower.
  - Stamping before the volume table would be unsafe. A cut would leave headers but no device: the partition no longer reads blank, and a caller that only formats blank partitions would refuse it.

### API shape (open)

Either:
- a field in `struct ubi_config`, e.g. `bool trust_blank_format`, meaning "the caller has verified the partition is blank"; or
- a separate entry point, e.g. `ubi_device_format_blank()`, that performs the whole-partition blank check itself.

The second keeps the guarantee inside UBI. The first lets a caller that already reads the whole partition reuse that pass. key_value_db's UBI backend does exactly that: `partition_is_blank()` runs before `ubi_device_format()` on first mount.

## Cost

DK model: read 65.7 µs + 0.258 µs/B; write 157 µs + 12.16 µs/B.

| | per 4 KiB block | ~1,800 blocks |
|---|--:|--:|
| blank check, one 4 KiB read (or already done by the caller) | ~1.1 ms | ~2 s |
| blank check, 128 B pieces (`CONFIG_UBI_IO_CHUNK_SIZE` default) | ~3.2 ms | ~5.7 s |
| stamp a 64 B EC header | ~0.94 ms | ~1.7 s |
| **today: erase on first use** | **~58 ms** | **~105 s** |

On 64 KiB blocks, the blank check is ~17 ms per block as one read (~50 ms in 128 B pieces), against a ~1.06–1.11 s measured block erase on the DK.

## Remaining risk

A full-chip or full-partition erase done **outside** UBI that was cut short, for example on a production programmer, can leave every byte reading 0xFF while cells are weakly erased. Normal reads cannot detect that. This option would trust such a part. Document it as a precondition: the partition must come from a completed factory or programmer erase.

## Done when

- [ ] Format of a verified-blank partition stamps every block, writing the volume table before any other block's header.
- [ ] Attach after it finds no `UNKNOWN` blocks; first writes perform no erase.
- [ ] Power-cut test: a cut at every stamping step leaves an attachable device; unstamped blocks are erased on first use.
- [ ] A non-blank partition, or a later header-less block, behaves exactly as today.
- [ ] Docs: `docs/how-it-works.md` (Attach, Erasing) and `docs/on-flash-format.md` describe the new format path.
