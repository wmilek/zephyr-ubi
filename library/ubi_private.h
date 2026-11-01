/**
 * \file    ubi_private.h
 * \author  Kamil Kielbasa
 * \brief   Unsorted Block Images (UBI) library-internal types.
 *
 *          Consumers see \ref ubi_device as an opaque declaration. Everything
 *          whose size follows the partition lives behind a pointer and is
 *          taken from the heap when the device attaches.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_PRIVATE_H
#define UBI_PRIVATE_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/slist.h>
#include <zephyr/toolchain.h>

/* PSA headers: */
#include <psa/crypto_types.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_volume_table.h"

/* Defines ----------------------------------------------------------------- */

/** "UBI$" in ASCII. Never written to the flash; marks an attached handle. */
#define UBI_DEVICE_MAGIC (0x55424924UL)

/** Entry in \ref ubi_volume.eba that has no physical block behind it. */
#define UBI_LEB_UNMAPPED (UINT16_MAX)

/** Fewer blocks than this leaves no room to work in. */
#define UBI_MIN_PEB_COUNT (4)

/** Block numbers are 16-bit, and one value is the unmapped sentinel. */
#define UBI_MAX_PEB_COUNT (UINT16_MAX - 1)

/** Erase counts stop here: the flash field is 64-bit, the RAM copy 32-bit. */
#define UBI_MAX_ERASE_COUNT (0x7FFFFFFFUL)

/** Erase count of a block whose header could not be read, until attach
 *  gives it the mean of the others. Above any real count. */
#define UBI_ERASE_COUNT_UNKNOWN (UINT32_MAX)

/** Bytes of \ref ubi_blocks.named for \p peb_count blocks, one bit each. */
#define UBI_NAMED_SIZE(peb_count) DIV_ROUND_UP((peb_count), BITS_PER_BYTE)

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief Lifecycle state of a physical erase block, held in RAM only.
 */
enum ubi_peb_state {
	/** No usable erase counter header: blank, left by an earlier image or
	 *  damaged. Erased and stamped before use. */
	UBI_PEB_UNKNOWN = 0,
	/** Erased and stamped; allocatable with no erase. */
	UBI_PEB_FREE,
	/** Backs a logical erase block. */
	UBI_PEB_MAPPED,
	/** Released by its logical block, awaiting #UBI_MAINTENANCE_RECLAIM. */
	UBI_PEB_RECLAIM,
	/** Valid erase counter header over a damaged volume identifier header
	 *  and data that is not blank. Kept for what it holds. */
	UBI_PEB_CORRUPT,
	/** Refused a write or an erase. #UBI_MAINTENANCE_REPAIR gives it one
	 *  more chance. */
	UBI_PEB_BAD,
	/** Refused again; out of service until the next attach. */
	UBI_PEB_WORN_OUT,
	/** Backs a logical erase block relocation could not read or verify.
	 *  Never moved again; reclaimed once its logical block lets go. */
	UBI_PEB_ERRONEOUS,
	/** Let go of by an unmap, possibly the newest copy of its logical
	 *  block. Erased after the copies a change left, and with every other
	 *  copy of its logical block. */
	UBI_PEB_UNMAPPED,
};

BUILD_ASSERT(UBI_PEB_UNMAPPED <= UINT8_MAX,
	     "block states must fit the byte they are stored in");

/**
 * \brief Fixed dimensions of the managed partition.
 */
struct ubi_geometry {
	/** Physical erase blocks in the partition. */
	uint32_t peb_count;
	/** Size of one physical erase block in bytes. */
	uint32_t peb_size;
	/** Bytes usable per logical erase block. */
	uint32_t leb_size;
	/** Write granularity of the flash. */
	uint32_t write_block_size;
	/** Byte an erase leaves behind, as the driver reports it. */
	uint8_t erase_value;
};

/**
 * \brief Keys UBI derived for itself from the application's handle.
 */
struct ubi_keys {
	/** Authenticates EC and VID headers. */
	psa_key_id_t header;
	/** Authenticates the volume table record. */
	psa_key_id_t volume_table;
};

/**
 * \brief Hooks back into the application.
 */
struct ubi_callbacks {
	/** Integrity event sink. */
	void (*event)(const struct ubi_event *event, void *user_context);
	/** Trust check. */
	enum ubi_state_verdict (*state)(const struct ubi_device_info *info,
					void *user_context);
	/** Passed back to both. */
	void *user_context;
};

/**
 * \brief A volume as tracked at run time.
 */
struct ubi_volume {
	/** Identifier assigned at creation, never reused. */
	uint32_t vol_id;
	/** Logical erase blocks reserved for this volume. */
	uint32_t leb_count;
	/** NUL-terminated volume name. */
	char name[UBI_VOLUME_NAME_MAX_LEN + 1];
	/** Physical block behind each logical block, or #UBI_LEB_UNMAPPED.
	 *  Points into \ref ubi_volumes.eba_pool. */
	uint16_t *eba;
};

/**
 * \brief Every volume the application declared, plus the storage they share.
 */
struct ubi_volumes {
	/** Volumes currently defined, occupying the first \ref entries. */
	uint32_t count;
	/** Next volume identifier; identifiers are never reused. */
	uint32_t id_watermark;
	/** Revision of the volume table record in force. */
	uint32_t revision;
	/** Volumes as reconstructed at attach. */
	struct ubi_volume entries[CONFIG_UBI_MAX_NR_OF_VOLUMES];
	/** Storage the \c eba arrays are carved from, one entry per physical
	 *  block: a mapped logical block occupies a physical one. */
	uint16_t *eba_pool;
	/** Entries of \ref eba_pool handed out. */
	uint32_t eba_used;
};

/**
 * \brief The volume that holds the volume table record, and its copies.
 *
 *        Kept apart from \ref ubi_volumes: it has to be reachable before the
 *        record declaring the others is read, and is not the application's.
 */
struct ubi_volume_table {
	/** The volume itself, for the ordinary lookup path. */
	struct ubi_volume volume;
	/** Physical block holding each copy, by copy number. */
	uint16_t eba[UBI_VOLUME_TABLE_LEB_COUNT];
	/** Sequence number each copy carries. */
	uint64_t sqnum[UBI_VOLUME_TABLE_LEB_COUNT];
	/** Copy in force; an update writes the other one first. */
	uint32_t current;
	/** The copies disagree or one is missing, so one erase would cost a
	 *  revision. Cleared by #UBI_MAINTENANCE_REPAIR and any update. */
	bool degraded;
};

/**
 * \brief Per-block bookkeeping, rebuilt from the flash on every attach and
 *        indexed by physical block number.
 */
struct ubi_blocks {
	/** Lifecycle state, a \ref ubi_peb_state in a byte. */
	uint8_t *state;
	/** Erase count, from the erase counter header at attach. */
	uint32_t *erase_count;
	/** Erases still owed before levelling may move the block. */
	uint8_t *protect;
	/** One bit per block for the self-check; only with
	 *  \c CONFIG_UBI_SELF_CHECKS. */
	uint8_t *named;
};

/**
 * \brief Room to build and decode a volume table record in, taken from the
 *        heap while the partition is open.
 */
struct ubi_scratch {
	/** A record being built, or the one attach adopted. */
	struct ubi_volume_table_record record;
	/** One copy as it sits on the flash: its VID header and data area. */
	uint8_t io[UBI_HEADER_SIZE + UBI_VOLUME_TABLE_DATA_MAX_SIZE];
};

/**
 * \brief An attached UBI device; opaque in the public header.
 */
struct ubi_device {
	/** #UBI_DEVICE_MAGIC once attached. */
	uint32_t magic;

	/** Managed partition, open for the lifetime of the attachment. */
	const struct flash_area *flash_area;

	/** Identifies this image; blocks carrying another one are foreign. */
	uint32_t image_seq;

	/** Highest sequence number found at attach or issued since. */
	uint64_t max_sqnum;

	/** Flash writes since the state callback was last consulted. */
	uint32_t writes_since_check;

	/** The state callback withdrew its trust; only an attach clears it. */
	bool untrusted;

	/** A block could not be erased; only an attach clears it. */
	bool read_only;

	/** A callback runs, holding the lock the caller would take again. */
	bool in_callback;

	/** The flash refused a write over a header; erases skip invalidation
	 *  until the next attach. */
	bool invalidation_refused;

	/** Serialises every public operation on this device. */
	struct k_mutex lock;

	/** Dimensions of the partition. */
	struct ubi_geometry geometry;

	/** Keys derived from the application's handle. */
	struct ubi_keys keys;

	/** Hooks back into the application. */
	struct ubi_callbacks callbacks;

	/** Volumes and their logical-to-physical mappings. */
	struct ubi_volumes volumes;

	/** The volume that describes those volumes. */
	struct ubi_volume_table volume_table;

	/** State and wear of every physical erase block. */
	struct ubi_blocks blocks;

	/** Room for volume table records. */
	struct ubi_scratch *scratch;

#if defined(CONFIG_UBI_SHELL)
	/** Place in the list of attached devices the shell walks. */
	sys_snode_t shell_node;
#endif
};

/* Module interface function declarations ---------------------------------- */

/**
 * \brief Take the next sequence number. A write that fails still spends it,
 *        so no two headers carry the same number.
 *
 * \param[in,out] ubi                   Device issuing the number.
 *
 * \return The number.
 */
static inline uint64_t ubi_impl_sqnum_next(struct ubi_device *ubi)
{
	ubi->max_sqnum += 1;

	return ubi->max_sqnum;
}

#endif /* UBI_PRIVATE_H */
