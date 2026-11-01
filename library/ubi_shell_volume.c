/**
 * \file    ubi_shell_volume.c
 * \author  Kamil Kielbasa
 * \brief   Diagnostic shell commands for volumes and the logical-to-physical
 *          mapping.
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
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_shell.h"
#include "ubi_volume.h"
#include "ubi_volume_table.h"

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief One volume, as copied out for printing.
 */
struct shell_volume {
	/** Identifier. */
	uint32_t vol_id;
	/** NUL-terminated name. */
	char name[UBI_VOLUME_NAME_MAX_LEN + 1];
	/** Logical blocks reserved. */
	uint32_t leb_count;
	/** Logical blocks backed by a physical one. */
	uint32_t mapped;
};

/**
 * \brief One mapped logical block, as copied out for printing.
 */
struct shell_leb {
	/** Logical block. */
	uint32_t lnum;
	/** Physical block behind it. */
	uint32_t pnum;
	/** The \ref ubi_peb_state of that block. */
	uint32_t state;
	/** Its erase count. */
	uint32_t erase_count;
};

/* Static function declarations -------------------------------------------- */

/**
 * \brief Copy out a volume's summary. The device has to be locked.
 */
static void shell_volume_copy(const struct ubi_volume *volume,
			      struct shell_volume *copy);

/**
 * \brief Find a volume by identifier or, failing that, by name. The device
 *        has to be locked.
 *
 * \retval 0
 *         Found.
 * \retval -ENOENT
 *         No such volume.
 */
static int shell_volume_resolve(struct ubi_device *ubi, const char *name,
				uint32_t *vol_id);

/**
 * \brief Print which physical block backs each mapped logical block of one
 *        volume, the volume table included.
 */
static int shell_volume_map_print(const struct shell *sh,
				  struct ubi_device *ubi, uint32_t vol_id);

/**
 * \brief "ubi volumes <dev>": every volume and how much of it is mapped.
 */
static int shell_cmd_volumes(const struct shell *sh, size_t argc, char **argv);

/**
 * \brief "ubi map <dev> [volume]": the LEB to PEB mapping.
 */
static int shell_cmd_map(const struct shell *sh, size_t argc, char **argv);

/* Static function definitions --------------------------------------------- */

static void shell_volume_copy(const struct ubi_volume *volume,
			      struct shell_volume *copy)
{
	*copy = (struct shell_volume){
		.vol_id = volume->vol_id,
		.leb_count = volume->leb_count,
	};

	strncpy(copy->name, volume->name, sizeof(copy->name) - 1);

	for (uint32_t lnum = 0; lnum < volume->leb_count; ++lnum) {
		if (UBI_LEB_UNMAPPED != volume->eba[lnum])
			copy->mapped += 1;
	}
}

static int shell_volume_resolve(struct ubi_device *ubi, const char *name,
				uint32_t *vol_id)
{
	int err = 0;
	const unsigned long number = shell_strtoul(name, 0, &err);

	if (0 == err && number < UBI_VOLUME_TABLE_VOL_ID &&
	    NULL != ubi_impl_volume_by_id(ubi, (uint32_t)number)) {
		*vol_id = (uint32_t)number;
		return 0;
	}

	return ubi_impl_volume_find(ubi, name, vol_id);
}

static int shell_volume_map_print(const struct shell *sh,
				  struct ubi_device *ubi, uint32_t vol_id)
{
	struct shell_leb lebs[UBI_SHELL_ROWS] = { 0 };
	struct shell_volume summary = { 0 };
	char ec_text[UBI_SHELL_EC_TEXT_SIZE] = { 0 };
	bool started = false;
	bool done = false;
	uint32_t lnum = 0;

	while (!done) {
		uint32_t rows = 0;
		const int ret = ubi_impl_shell_lock(sh, ubi);

		if (0 != ret)
			return ret;

		/* Looked up again every window: the volume may have been
		 * resized or removed while the last one was printed. */
		const struct ubi_volume *volume =
			ubi_impl_volume_by_id(ubi, vol_id);

		if (NULL == volume) {
			ubi_impl_shell_unlock(ubi);
			shell_error(sh, "volume %u was removed meanwhile",
				    vol_id);
			return -ENOENT;
		}

		if (!started)
			shell_volume_copy(volume, &summary);

		for (; lnum < volume->leb_count && rows < UBI_SHELL_ROWS;
		     ++lnum) {
			const uint32_t pnum = volume->eba[lnum];

			if (UBI_LEB_UNMAPPED == pnum)
				continue;

			lebs[rows] = (struct shell_leb){
				.lnum = lnum,
				.pnum = pnum,
				.state = ubi_impl_peb_state_get(ubi, pnum),
				.erase_count = ubi->blocks.erase_count[pnum],
			};
			rows += 1;
		}

		done = (lnum >= volume->leb_count);

		ubi_impl_shell_unlock(ubi);

		if (!started) {
			if (UBI_VOLUME_TABLE_VOL_ID == vol_id)
				shell_print(sh,
					    "volume table (internal): "
					    "%u LEBs, %u mapped",
					    summary.leb_count, summary.mapped);
			else
				shell_print(sh,
					    "volume %u \"%s\": %u LEBs, "
					    "%u mapped",
					    vol_id, summary.name,
					    summary.leb_count, summary.mapped);

			if (0 != summary.mapped)
				shell_print(sh, "  %6s %6s %10s  %s", "LEB",
					    "PEB", "EC", "state");

			started = true;
		}

		for (uint32_t i = 0; i < rows; ++i)
			shell_print(sh, "  %6u %6u %10s  %s", lebs[i].lnum,
				    lebs[i].pnum,
				    ubi_impl_shell_ec_text(lebs[i].state,
							   lebs[i].erase_count,
							   ec_text),
				    ubi_impl_shell_state_name(lebs[i].state));
	}

	return 0;
}

static int shell_cmd_volumes(const struct shell *sh, size_t argc, char **argv)
{
	struct shell_volume summary = { 0 };
	struct ubi_device *ubi = NULL;

	ARG_UNUSED(argc);

	int ret = ubi_impl_shell_take(sh, argv[1], &ubi);

	if (0 != ret)
		return ret;

	shell_print(sh, "%10s  %-16s %6s %6s", "ID", "name", "LEBs", "mapped");

	/* One volume per lock; one created or removed meanwhile may be
	 * missed. */
	for (uint32_t i = 0;; ++i) {
		ret = ubi_impl_shell_lock(sh, ubi);

		if (0 != ret)
			break;

		const bool more = (i < ubi->volumes.count);

		if (more)
			shell_volume_copy(&ubi->volumes.entries[i], &summary);

		ubi_impl_shell_unlock(ubi);

		if (!more)
			break;

		shell_print(sh, "%10u  %-16s %6u %6u", summary.vol_id,
			    summary.name, summary.leb_count, summary.mapped);
	}

	ubi_impl_shell_give();

	return ret;
}

static int shell_cmd_map(const struct shell *sh, size_t argc, char **argv)
{
	struct ubi_device *ubi = NULL;
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	int ret = ubi_impl_shell_take(sh, argv[1], &ubi);

	if (0 != ret)
		return ret;

	if (3 == argc) {
		ret = ubi_impl_shell_lock(sh, ubi);

		if (0 != ret)
			goto give;

		ret = shell_volume_resolve(ubi, argv[2], &vol_id);

		ubi_impl_shell_unlock(ubi);

		if (0 != ret)
			shell_error(sh, "no volume \"%s\"", argv[2]);
		else
			ret = shell_volume_map_print(sh, ubi, vol_id);

		goto give;
	}

	ret = shell_volume_map_print(sh, ubi, UBI_VOLUME_TABLE_VOL_ID);

	for (uint32_t i = 0; 0 == ret; ++i) {
		ret = ubi_impl_shell_lock(sh, ubi);

		if (0 != ret)
			break;

		const bool more = (i < ubi->volumes.count);

		if (more)
			vol_id = ubi->volumes.entries[i].vol_id;

		ubi_impl_shell_unlock(ubi);

		if (!more)
			break;

		ret = shell_volume_map_print(sh, ubi, vol_id);
	}

give:
	ubi_impl_shell_give();

	return ret;
}

/* Shell commands ---------------------------------------------------------- */

SHELL_SUBCMD_ADD((ubi), volumes, NULL,
		 "Volumes and how many of their LEBs are mapped.\n"
		 "Usage: ubi volumes <dev>",
		 shell_cmd_volumes, 2, 0);

SHELL_SUBCMD_ADD((ubi), map, NULL,
		 "LEB -> PEB mapping with each PEB's erase count, of one\n"
		 "volume (by ID or name) or of all, the volume table first.\n"
		 "Usage: ubi map <dev> [volume]",
		 shell_cmd_map, 2, 1);
