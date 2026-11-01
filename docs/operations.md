# Operations

What to do when UBI returns an error or reports an event, what a power cut
leaves behind, and what UBI costs. The contract of every call is in
[include/ubi/ubi.h](https://github.com/kamil-kielbasa/zephyr-ubi/blob/main/include/ubi/ubi.h).

## When an attach fails

`ubi_device_init()` writes nothing, so a failed attach leaves the flash as it
was. Format the partition only after `-ENODEV`: after any other error a UBI
device may be on it, and a format would destroy it.

| returns | means | do |
|---|---|---|
| `-ENODEV` | no UBI device: the partition is blank or holds other data | format it, if that is expected |
| `-EBADMSG` | UBI metadata failed verification: a different key, damage or tampering | keep the device; a format destroys its data |
| `-ENOTSUP` | written by a newer release, or with more volumes than `CONFIG_UBI_MAX_NR_OF_VOLUMES` | run the newer firmware, or raise the limit |
| `-EIO` | flash or crypto failure | try again; if it persists, check the hardware |
| `-EROFS` | the state callback returned `UBI_STATE_UNTRUSTED` | treat it as a rollback |
| `-EINVAL` | invalid configuration, a partition geometry UBI cannot use or that differs from the format, or too many corrupt blocks | check the configuration and the devicetree; see [maintenance](#keeping-maintenance-up) |
| `-EACCES` | the key does not exist or does not allow HKDF-SHA256 derivation | fix the key or its policy |
| `-ENOMEM` | not enough heap | see [Resources](#resources) |
| `-EBUSY` | the handle or the partition is in use | detach the other handle first |
| `-ENOSPC` | more than 65534 blocks, or volumes that do not fit the partition | use fewer, larger blocks |

## Events

Events only report: UBI has already acted when the callback runs.

| event | means | do |
|---|---|---|
| `UBI_EVENT_HDR_CORRUPT` | a header failed its CRC: a write or erase cut short, or bit rot | nothing after a power cut; data behind a damaged header is kept, see `corrupt_pebs` |
| `UBI_EVENT_HDR_TAMPERED` | a header failed authentication: modified, or written under another key | expected after a key change until reclaim has run; otherwise keep the device as evidence |
| `UBI_EVENT_VOLUME_TABLE_CORRUPT` | a volume table copy could not be used | nothing while the other copy works |
| `UBI_EVENT_VOLUME_TABLE_DEGRADED` | only one volume table copy is usable, or the copies differ | run `UBI_MAINTENANCE_REPAIR` soon |
| `UBI_EVENT_LEB_ORPHANED` | a block names a volume this device never created | nothing; reclaim erases it |
| `UBI_EVENT_PEB_BAD` | a block was retired: a write or erase failed, or its erase count reached the limit | after a failed write, run `UBI_MAINTENANCE_REPAIR`; after a failed erase the device is read-only |
| `UBI_EVENT_DATA_CORRUPT` | the data of a LEB failed its checksum: a change cut short, or damage | nothing if the previous contents were used; otherwise the LEB keeps the data as it reads |

## Power loss

| cut short | after the reboot |
|---|---|
| `ubi_leb_change()` | the old contents or the new; a LEB with no old contents keeps the part of the new data that was written, reported with `UBI_EVENT_DATA_CORRUPT` |
| `ubi_leb_write_at()` | the part of the data that was written; the application finds the end of its data itself |
| `ubi_leb_erase()`, or reclaim after an unmap | the last contents or none, never older ones |
| volume create, resize or remove | the old layout or the new |
| `UBI_MAINTENANCE_RELOCATE` | the LEB in its old block or in the new one |
| `ubi_device_format()` | the previous device or the new one |

The third row needs `CONFIG_UBI_ERASE_INVALIDATES_HEADERS`, which is on by
default. Without it, an erase cut short can leave a block with valid headers
and part of its data. Data with a checksum is then reported with
`UBI_EVENT_DATA_CORRUPT`; appended data is not.

A failed write is different from a power cut: what the device reports right
after it is what the next attach finds.

## Keeping maintenance up

What one step of each operation does is in the contract of
`ubi_maintenance()`. When to run them:

| operation | when | budget |
|---|---|---|
| `UBI_MAINTENANCE_RECLAIM` | whenever the device is idle | a few blocks; with a full free pool, writes need no erase |
| `UBI_MAINTENANCE_RELOCATE` | `relocatable_pebs` is not zero | one at a time; each step copies a block and erases one |
| `UBI_MAINTENANCE_REPAIR` | after `UBI_EVENT_VOLUME_TABLE_DEGRADED` or `UBI_EVENT_PEB_BAD` | until `remaining` is zero |
| `UBI_MAINTENANCE_DISCARD` | when nothing on the `corrupt_pebs` is needed any more | all of them |

Attach refuses a partition once corrupt blocks reach a twentieth of its good
blocks, rounded down, or eight when that is zero. Discard them before that.

After a failed erase the device is read-only until the next attach: reads
work, and every call that would write returns `-EROFS`; see
[Bad blocks](how-it-works.md#bad-blocks). A part that keeps failing erases is
at the end of its life.

## Changing the key

Everything UBI writes is authenticated with keys derived from the keying
material, so new keying material means a new device:

1. Attach under the old key and copy out what has to survive.
2. Detach and format under the new key.
3. Attach under the new key. Each old block fails its MAC, is reported with
   `UBI_EVENT_HDR_TAMPERED` and counted in `reclaimable_pebs`.
4. Run `UBI_MAINTENANCE_RECLAIM` until `reclaimable_pebs` is zero.
5. Create the volumes and write back what was copied out.

The erase counts are authenticated with the old key, so they are lost with it.

## Diagnostic shell

With `CONFIG_SHELL` and `CONFIG_UBI_SHELL`, a `ubi` shell command shows what
an attached device holds: which physical block backs each logical block, and
how worn every block is. It only reads: nothing goes to the flash and no
state changes. Every attached device is listed automatically, by its
partition number, or by its label with `CONFIG_FLASH_MAP_LABELS`.

| command | shows |
|---|---|
| `ubi list` | the attached devices: partition, label, block count and size, volumes, state |
| `ubi info <dev>` | geometry, block counts, erase count range and mean, rollback counters, where the volume table copies are, read-only and trust state |
| `ubi volumes <dev>` | every volume: identifier, name, LEBs, LEBs mapped |
| `ubi map <dev> [volume]` | the PEB behind each mapped LEB of one volume, by identifier or name, or of all, with each PEB's erase count and state |
| `ubi pebs <dev> [first [count]]` | each PEB's state, erase count, protection left and owner: `vol:LEB`, or `vt:copy` for the volume table |
| `ubi ec <dev> [first [count]]` | the erase count range, then the count of each PEB, eight to a line |

```
uart:~$ ubi map 5
volume table (internal): 2 LEBs, 2 mapped
     LEB    PEB         EC  state
       0      2          2  mapped
       1      0          2  mapped
volume 0 "logs": 4 LEBs, 2 mapped
     LEB    PEB         EC  state
       1      1          2  mapped
       3      3          2  mapped
uart:~$ ubi ec 5 0 10
3 healthy PEBs: min 2, max 2, spread 0; levelling threshold 256; ~ marks a count estimated at attach
     0:          2          2          2         ~1         ~1         ~1         ~1         ~1
     8:         ~1         ~1
```

A count marked `~` belongs to a block with no usable erase counter header,
such as one never formatted: attach gave it the mean of the others. Old
copies waiting for reclaim show no owner, since only the mappings in force
are kept in RAM.

A command locks the device only while it copies a few rows, never while it
prints, so a slow console does not hold up the application; a long listing
is consistent row by row, not as a whole. It waits up to 5 s for an
operation in progress and then gives up with "the device is busy". A device
cannot detach while a command looks at it: `ubi_device_deinit()` waits for
the command to finish.

## Resources

Sizes from the nRF5340 build (Cortex-M33, `-Os`, logging off, mbedTLS),
timings on the nRF5340 DK:

| what | how much |
|---|---|
| code | about 11 KB, plus mbedTLS for AES-CMAC, HKDF and SHA-256 |
| handle, `ubi_device_size()` | 328 bytes with 4 volumes, 2248 with 64 |
| heap at attach | 8 bytes per erase block, and a scratch buffer of 392 bytes with 4 volumes, 3480 with 64 |
| heap during a format | a handle and a scratch buffer of its own |
| stack | 1704 bytes at most for any call on a Cortex-M33, with the default `CONFIG_UBI_IO_CHUNK_SIZE`; a larger chunk adds its growth |
| attach | 151 ms at most for 64 blocks of 4 KiB on the DK's MX25R64 |
| erase | 87 ms for a 4 KiB block and 1.2 s for a 64 KiB one on the same part |

`CONFIG_UBI_SELF_CHECKS` adds one bit per erase block of heap.
`CONFIG_UBI_SHELL` adds a pointer to the handle.

## Limits

- 4 to 65534 erase blocks of one size, each larger than the two headers and
  the volume table record.
- A write block that divides 64 bytes.
- Volume names of 1 to 16 characters.
- Up to `CONFIG_UBI_MAX_NR_OF_VOLUMES` volumes, 64 at most. The option may
  grow from one firmware to the next, never shrink: a build that allows fewer
  volumes than a device holds refuses it with `-ENOTSUP`.
- Erase counts up to 2³¹ − 1.
