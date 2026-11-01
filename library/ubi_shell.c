/**
 * \file    ubi_shell.c
 * \author  Kamil Kielbasa
 * \brief   Diagnostic shell commands for devices and physical blocks, and
 *          the list of attached devices every command walks.
 *
 *          A long listing is consistent row by row, not as a whole: the
 *          device is unlocked between rows, so a slow console never stalls
 *          the application. Nothing here writes to the flash or changes any
 *          state.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_shell.h"
#include "ubi_state.h"
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

/** Erase counts on one line of "ubi ec". */
#define SHELL_EC_PER_LINE (8)

/** Longest wait for the device lock: an operation in progress may be
 *  erasing several blocks. */
#define SHELL_LOCK_TIMEOUT K_SECONDS(5)

/** Characters of a block owner as printed, NUL included. */
#define SHELL_OWNER_TEXT_SIZE (24)

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief One physical block, as copied out for printing.
 */
struct shell_peb {
	/** Its \ref ubi_peb_state. */
	uint32_t state;
	/** Its erase count. */
	uint32_t erase_count;
	/** Erases left before levelling may move it. */
	uint32_t protect;
	/** Volume of the logical block it backs, or #UBI_VOL_ID_INVALID. */
	uint32_t vol_id;
	/** That logical block. */
	uint32_t lnum;
};

/* Static function declarations -------------------------------------------- */

/**
 * \brief Label of the partition under a device, or an empty string.
 */
static const char *shell_device_label(const struct ubi_device *ubi);

/**
 * \brief Find an attached device by partition number or label. The list
 *        has to be held.
 *
 * \return The device, or \c NULL.
 */
static struct ubi_device *shell_device_find(const char *name);

/**
 * \brief Format the logical block a physical one backs, "-" for none.
 */
static const char *shell_owner_text(const struct shell_peb *peb,
				    char text[SHELL_OWNER_TEXT_SIZE]);

/**
 * \brief Copy out a run of physical blocks with the logical blocks they
 *        back. The device has to be locked.
 */
static void shell_pebs_copy(const struct ubi_device *ubi, uint32_t first,
			    uint32_t count, struct shell_peb *pebs);

/**
 * \brief Read the optional "[first [count]]" range of physical blocks.
 *
 * \retval 0
 *         Read.
 * \retval -EINVAL
 *         Not a number, or past the end; the error is printed.
 */
static int shell_range_parse(const struct shell *sh, size_t argc, char **argv,
			     uint32_t peb_count, uint32_t *first,
			     uint32_t *count);

/**
 * \brief "ubi list": every attached device.
 */
static int shell_cmd_list(const struct shell *sh, size_t argc, char **argv);

/**
 * \brief "ubi info <dev>": geometry, block counts, wear and trust state.
 */
static int shell_cmd_info(const struct shell *sh, size_t argc, char **argv);

/**
 * \brief "ubi pebs <dev> [first [count]]": state, erase count and owner of
 *        every physical block.
 */
static int shell_cmd_pebs(const struct shell *sh, size_t argc, char **argv);

/**
 * \brief "ubi ec <dev> [first [count]]": the erase counts, compactly.
 */
static int shell_cmd_ec(const struct shell *sh, size_t argc, char **argv);

/* Module variables and constants ------------------------------------------ */

/** Every attached device, in the order they attached. */
static sys_slist_t shell_devices = SYS_SLIST_STATIC_INIT(&shell_devices);

/** Guards \ref shell_devices; a command holds it while it runs. */
static K_MUTEX_DEFINE(shell_devices_lock);

/* Static function definitions --------------------------------------------- */

static const char *shell_device_label(const struct ubi_device *ubi)
{
	const char *label = NULL;

#if defined(CONFIG_FLASH_MAP_LABELS)
	label = flash_area_label(ubi->flash_area);
#else
	ARG_UNUSED(ubi);
#endif

	return (NULL != label) ? label : "";
}

static struct ubi_device *shell_device_find(const char *name)
{
	struct ubi_device *ubi = NULL;
	int err = 0;
	const unsigned long partition = shell_strtoul(name, 0, &err);

	SYS_SLIST_FOR_EACH_CONTAINER(&shell_devices, ubi, shell_node)
	{
		if (0 == err && partition == ubi->flash_area->fa_id)
			return ubi;

		if (0 == strcmp(name, shell_device_label(ubi)))
			return ubi;
	}

	return NULL;
}

static const char *shell_owner_text(const struct shell_peb *peb,
				    char text[SHELL_OWNER_TEXT_SIZE])
{
	if (UBI_VOL_ID_INVALID == peb->vol_id)
		return "-";

	if (UBI_VOLUME_TABLE_VOL_ID == peb->vol_id)
		snprintk(text, SHELL_OWNER_TEXT_SIZE, "vt:%u", peb->lnum);
	else
		snprintk(text, SHELL_OWNER_TEXT_SIZE, "%u:%u", peb->vol_id,
			 peb->lnum);

	return text;
}

static void shell_pebs_copy(const struct ubi_device *ubi, uint32_t first,
			    uint32_t count, struct shell_peb *pebs)
{
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t pnum = first + i;

		pebs[i] = (struct shell_peb){
			.state = ubi_impl_peb_state_get(ubi, pnum),
			.erase_count = ubi->blocks.erase_count[pnum],
			.protect = ubi->blocks.protect[pnum],
			.vol_id = UBI_VOL_ID_INVALID,
		};
	}

	/* One walk over every mapping per window, the volume table's first.
	 * Only mappings live in RAM, so old copies awaiting reclaim show no
	 * owner. */
	for (uint32_t v = 0; v <= ubi->volumes.count; ++v) {
		const struct ubi_volume *volume =
			(0 == v) ? &ubi->volume_table.volume :
				   &ubi->volumes.entries[v - 1];

		for (uint32_t lnum = 0; lnum < volume->leb_count; ++lnum) {
			const uint32_t pnum = volume->eba[lnum];

			if (UBI_LEB_UNMAPPED == pnum || pnum < first ||
			    pnum - first >= count)
				continue;

			pebs[pnum - first].vol_id = volume->vol_id;
			pebs[pnum - first].lnum = lnum;
		}
	}
}

static int shell_range_parse(const struct shell *sh, size_t argc, char **argv,
			     uint32_t peb_count, uint32_t *first,
			     uint32_t *count)
{
	unsigned long value[2] = { 0, peb_count };
	int err = 0;

	for (size_t i = 0; i < 2 && 2 + i < argc; ++i) {
		value[i] = shell_strtoul(argv[2 + i], 0, &err);

		if (0 != err) {
			shell_error(sh, "\"%s\" is not a number", argv[2 + i]);
			return -EINVAL;
		}
	}

	if (value[0] >= peb_count) {
		shell_error(sh, "the device has PEBs 0 to %u", peb_count - 1);
		return -EINVAL;
	}

	*first = (uint32_t)value[0];
	*count = (uint32_t)MIN(value[1], peb_count - value[0]);

	return 0;
}

static int shell_cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	struct ubi_device *ubi = NULL;
	bool any = false;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&shell_devices_lock, K_FOREVER);

	SYS_SLIST_FOR_EACH_CONTAINER(&shell_devices, ubi, shell_node)
	{
		if (0 != ubi_impl_shell_lock(sh, ubi))
			continue;

		const uint32_t volumes = ubi->volumes.count;
		const char *mode = ubi->read_only ? "read-only" :
				   ubi->untrusted ? "untrusted" :
						    "ok";

		ubi_impl_shell_unlock(ubi);

		if (!any)
			shell_print(sh, "%9s  %-16s %6s %10s %7s  %s",
				    "partition", "label", "PEBs", "PEB size",
				    "volumes", "state");

		shell_print(sh, "%9u  %-16s %6u %10u %7u  %s",
			    ubi->flash_area->fa_id,
			    ('\0' == *shell_device_label(ubi)) ?
				    "-" :
				    shell_device_label(ubi),
			    ubi->geometry.peb_count, ubi->geometry.peb_size,
			    volumes, mode);

		any = true;
	}

	k_mutex_unlock(&shell_devices_lock);

	if (!any)
		shell_print(sh, "no UBI device is attached");

	return 0;
}

static int shell_cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	struct ubi_device_info info = { 0 };
	struct ubi_volume_table table = { 0 };
	struct ubi_device *ubi = NULL;
	bool read_only = false;
	bool untrusted = false;
	uint32_t writes = 0;
	uint64_t mean_tenths = 0;

	ARG_UNUSED(argc);

	int ret = ubi_impl_shell_take(sh, argv[1], &ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_shell_lock(sh, ubi);

	if (0 != ret)
		goto give;

	ret = ubi_impl_state_describe(ubi, &info);
	table = ubi->volume_table;
	read_only = ubi->read_only;
	untrusted = ubi->untrusted;
	writes = ubi->writes_since_check;

	ubi_impl_shell_unlock(ubi);

	if (0 != ret) {
		shell_error(sh, "the block table is inconsistent (%d)", ret);
		goto give;
	}

	if (0 != info.healthy_pebs)
		mean_tenths = info.total_erase_count * 10U / info.healthy_pebs;

	shell_print(sh, "partition:    %u%s%s, offset 0x%lx, %zu bytes",
		    ubi->flash_area->fa_id,
		    ('\0' == *shell_device_label(ubi)) ? "" : " ",
		    shell_device_label(ubi),
		    (unsigned long)ubi->flash_area->fa_off,
		    ubi->flash_area->fa_size);
	shell_print(sh,
		    "geometry:     %u PEBs of %u bytes, LEB %u bytes, "
		    "write block %u, erase value 0x%02x",
		    info.peb_count, info.peb_size, info.leb_size,
		    info.write_block_size, ubi->geometry.erase_value);
	shell_print(sh, "volumes:      %u of %u, %u LEBs free",
		    info.volume_count, CONFIG_UBI_MAX_NR_OF_VOLUMES,
		    info.free_lebs);
	shell_print(sh,
		    "PEBs:         %u free, %u reclaimable, %u relocatable, "
		    "%u corrupt, %u bad, %u healthy",
		    info.free_pebs, info.reclaimable_pebs,
		    info.relocatable_pebs, info.corrupt_pebs, info.bad_pebs,
		    info.healthy_pebs);
	shell_print(sh,
		    "erase counts: min %u, max %u, mean %llu.%llu, total %llu",
		    info.min_erase_count, info.max_erase_count,
		    (unsigned long long)(mean_tenths / 10U),
		    (unsigned long long)(mean_tenths % 10U),
		    (unsigned long long)info.total_erase_count);
	shell_print(sh,
		    "image:        image_seq 0x%08x, revision %u, "
		    "max sqnum %llu",
		    info.image_seq, info.revision,
		    (unsigned long long)info.max_sqnum);
	shell_print(sh,
		    "volume table: copies in PEBs %u and %u, copy %u in "
		    "force%s",
		    (uint32_t)table.eba[0], (uint32_t)table.eba[1],
		    table.current, table.degraded ? ", degraded" : "");
	shell_print(sh,
		    "state:        %s, %s, %u writes since the last state "
		    "check",
		    read_only ? "read-only" : "writable",
		    untrusted ? "untrusted" : "trusted", writes);

give:
	ubi_impl_shell_give();

	return ret;
}

static int shell_cmd_pebs(const struct shell *sh, size_t argc, char **argv)
{
	struct shell_peb pebs[UBI_SHELL_ROWS] = { 0 };
	char ec_text[UBI_SHELL_EC_TEXT_SIZE] = { 0 };
	char owner_text[SHELL_OWNER_TEXT_SIZE] = { 0 };
	struct ubi_device *ubi = NULL;
	uint32_t first = 0;
	uint32_t count = 0;

	int ret = ubi_impl_shell_take(sh, argv[1], &ubi);

	if (0 != ret)
		return ret;

	ret = shell_range_parse(sh, argc, argv, ubi->geometry.peb_count, &first,
				&count);

	if (0 != ret)
		goto give;

	shell_print(sh, "%6s  %-10s %10s %7s  %s", "PEB", "state", "EC",
		    "protect", "owner");

	for (uint32_t done = 0; done < count;) {
		const uint32_t rows =
			MIN(count - done, (uint32_t)UBI_SHELL_ROWS);

		ret = ubi_impl_shell_lock(sh, ubi);

		if (0 != ret)
			break;

		shell_pebs_copy(ubi, first + done, rows, pebs);

		ubi_impl_shell_unlock(ubi);

		for (uint32_t i = 0; i < rows; ++i)
			shell_print(sh, "%6u  %-10s %10s %7u  %s",
				    first + done + i,
				    ubi_impl_shell_state_name(pebs[i].state),
				    ubi_impl_shell_ec_text(pebs[i].state,
							   pebs[i].erase_count,
							   ec_text),
				    pebs[i].protect,
				    shell_owner_text(&pebs[i], owner_text));

		done += rows;
	}

give:
	ubi_impl_shell_give();

	return ret;
}

static int shell_cmd_ec(const struct shell *sh, size_t argc, char **argv)
{
	struct ubi_device_info info = { 0 };
	uint32_t counts[SHELL_EC_PER_LINE] = { 0 };
	uint32_t states[SHELL_EC_PER_LINE] = { 0 };
	char ec_text[UBI_SHELL_EC_TEXT_SIZE] = { 0 };
	char line[SHELL_EC_PER_LINE * UBI_SHELL_EC_TEXT_SIZE + 1] = { 0 };
	struct ubi_device *ubi = NULL;
	uint32_t first = 0;
	uint32_t count = 0;

	int ret = ubi_impl_shell_take(sh, argv[1], &ubi);

	if (0 != ret)
		return ret;

	ret = shell_range_parse(sh, argc, argv, ubi->geometry.peb_count, &first,
				&count);

	if (0 != ret)
		goto give;

	ret = ubi_impl_shell_lock(sh, ubi);

	if (0 != ret)
		goto give;

	ret = ubi_impl_state_describe(ubi, &info);

	ubi_impl_shell_unlock(ubi);

	if (0 != ret) {
		shell_error(sh, "the block table is inconsistent (%d)", ret);
		goto give;
	}

	shell_print(sh,
		    "%u healthy PEBs: min %u, max %u, spread %u; levelling "
		    "threshold %u; ~ marks a count estimated at attach",
		    info.healthy_pebs, info.min_erase_count,
		    info.max_erase_count,
		    info.max_erase_count - info.min_erase_count,
		    CONFIG_UBI_WEAR_LEVELING_THRESHOLD);

	for (uint32_t done = 0; done < count;) {
		const uint32_t rows =
			MIN(count - done, (uint32_t)SHELL_EC_PER_LINE);
		size_t used = 0;

		ret = ubi_impl_shell_lock(sh, ubi);

		if (0 != ret)
			break;

		for (uint32_t i = 0; i < rows; ++i) {
			counts[i] = ubi->blocks.erase_count[first + done + i];
			states[i] =
				ubi_impl_peb_state_get(ubi, first + done + i);
		}

		ubi_impl_shell_unlock(ubi);

		/* At most 12 characters an entry: the line holds them all. */
		for (uint32_t i = 0; i < rows; ++i)
			used += (size_t)snprintk(
				&line[used], sizeof(line) - used, " %10s",
				ubi_impl_shell_ec_text(states[i], counts[i],
						       ec_text));

		shell_print(sh, "%6u:%s", first + done, line);

		done += rows;
	}

give:
	ubi_impl_shell_give();

	return ret;
}

/* Module interface function definitions ----------------------------------- */

void ubi_impl_shell_register(struct ubi_device *ubi)
{
	k_mutex_lock(&shell_devices_lock, K_FOREVER);
	sys_slist_append(&shell_devices, &ubi->shell_node);
	k_mutex_unlock(&shell_devices_lock);
}

void ubi_impl_shell_unregister(struct ubi_device *ubi)
{
	k_mutex_lock(&shell_devices_lock, K_FOREVER);

	const bool found =
		sys_slist_find_and_remove(&shell_devices, &ubi->shell_node);

	k_mutex_unlock(&shell_devices_lock);

	__ASSERT(found, "a device detached that was never listed");
	ARG_UNUSED(found);
}

int ubi_impl_shell_take(const struct shell *sh, const char *name,
			struct ubi_device **ubi)
{
	k_mutex_lock(&shell_devices_lock, K_FOREVER);

	*ubi = shell_device_find(name);

	if (NULL != *ubi)
		return 0;

	k_mutex_unlock(&shell_devices_lock);

	shell_error(sh, "no attached device \"%s\"; \"ubi list\" shows them",
		    name);

	return -ENODEV;
}

void ubi_impl_shell_give(void)
{
	k_mutex_unlock(&shell_devices_lock);
}

int ubi_impl_shell_lock(const struct shell *sh, struct ubi_device *ubi)
{
	if (0 == k_mutex_lock(&ubi->lock, SHELL_LOCK_TIMEOUT))
		return 0;

	shell_error(sh, "the device is busy; try again");

	return -EBUSY;
}

void ubi_impl_shell_unlock(struct ubi_device *ubi)
{
	k_mutex_unlock(&ubi->lock);
}

const char *ubi_impl_shell_state_name(uint32_t state)
{
	switch ((enum ubi_peb_state)state) {
	case UBI_PEB_UNKNOWN:
		return "unknown";
	case UBI_PEB_FREE:
		return "free";
	case UBI_PEB_MAPPED:
		return "mapped";
	case UBI_PEB_RECLAIM:
		return "reclaim";
	case UBI_PEB_CORRUPT:
		return "corrupt";
	case UBI_PEB_BAD:
		return "bad";
	case UBI_PEB_WORN_OUT:
		return "worn-out";
	case UBI_PEB_ERRONEOUS:
		return "erroneous";
	case UBI_PEB_UNMAPPED:
		return "unmapped";
	default:
		return "invalid";
	}
}

const char *ubi_impl_shell_ec_text(uint32_t state, uint32_t erase_count,
				   char text[UBI_SHELL_EC_TEXT_SIZE])
{
	if (UBI_ERASE_COUNT_UNKNOWN == erase_count)
		return "?";

	snprintk(text, UBI_SHELL_EC_TEXT_SIZE,
		 (UBI_PEB_UNKNOWN == state) ? "~%u" : "%u", erase_count);

	return text;
}

/* Shell commands ---------------------------------------------------------- */

SHELL_SUBCMD_SET_CREATE(ubi_shell_commands, (ubi));

SHELL_SUBCMD_ADD((ubi), list, NULL,
		 "List the attached devices.\n"
		 "Usage: ubi list",
		 shell_cmd_list, 1, 0);

SHELL_SUBCMD_ADD((ubi), info, NULL,
		 "Geometry, block counts, wear and trust state.\n"
		 "Usage: ubi info <dev>",
		 shell_cmd_info, 2, 0);

SHELL_SUBCMD_ADD((ubi), pebs, NULL,
		 "State, erase count, protection and owner (vol:LEB, vt for\n"
		 "the volume table) of each PEB.\n"
		 "Usage: ubi pebs <dev> [first [count]]",
		 shell_cmd_pebs, 2, 2);

SHELL_SUBCMD_ADD((ubi), ec, NULL,
		 "Erase count summary, then the count of each PEB.\n"
		 "Usage: ubi ec <dev> [first [count]]",
		 shell_cmd_ec, 2, 2);

SHELL_CMD_REGISTER(ubi, &ubi_shell_commands,
		   "UBI diagnostics. <dev> is a partition number or label.",
		   NULL);
