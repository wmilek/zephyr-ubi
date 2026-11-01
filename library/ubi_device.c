/**
 * \file    ubi_device.c
 * \author  Kamil Kielbasa
 * \brief   Formatting a partition and attaching to it.
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
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include "ubi_attach.h"
#include "ubi_device.h"
#include "ubi_header.h"
#include "ubi_key.h"
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_shell.h"
#include "ubi_state.h"
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/** Draws allowed before a random source that keeps returning zero gives up. */
#define IMAGE_SEQ_DRAW_LIMIT (4)

/** Flash area identifiers there can be, one per value of a byte. */
#define DEVICE_PARTITIONS (UINT8_MAX + 1)

/** Blocks in a row a format may fail to prepare before it takes the flash
 *  itself to be failing. */
#define FORMAT_FAILURES_IN_A_ROW (8)

BUILD_ASSERT(UBI_LEB_UNMAPPED == UINT16_MAX,
	     "the unmapped sentinel must be all ones, so that memset sets it");

BUILD_ASSERT(UBI_ERASE_COUNT_UNKNOWN == UINT32_MAX,
	     "the unknown erase count must be all ones, so that memset sets it");

/* Static function declarations -------------------------------------------- */

/**
 * \brief Ask the flash driver for the dimensions of the partition, and check
 *        that it is made of whole erase blocks of one size.
 *
 *        \p geometry holds what was measured even when it is then rejected,
 *        so that the caller can log it.
 */
static int device_geometry_measure(const struct flash_area *flash_area,
				   struct ubi_geometry *geometry);

/**
 * \brief Report whether this build can manage those dimensions.
 */
static int device_geometry_validate(const struct ubi_geometry *geometry);

/**
 * \brief Open the partition, measure it, take the scratch buffer and derive
 *        the device keys.
 */
static int device_open(struct ubi_device *ubi, const struct ubi_config *config);

/**
 * \brief Undo \ref device_open.
 */
static void device_close(struct ubi_device *ubi);

/**
 * \brief Take the per-block bookkeeping from the heap.
 */
static int device_tables_alloc(struct ubi_device *ubi);

/**
 * \brief Give it back.
 */
static void device_tables_free(struct ubi_device *ubi);

/**
 * \brief Mark a partition as taken, by an attach or by a format.
 *
 * \retval 0
 *         It was free.
 * \retval -EBUSY
 *         Another handle is attached to it, or it is being formatted.
 */
static int device_claim(uint8_t flash_area_id);

/**
 * \brief Mark it as free again.
 */
static void device_release(uint8_t flash_area_id);

/**
 * \brief Draw an image sequence number that is not zero.
 */
static int device_image_seq_draw(uint32_t *image_seq);

/**
 * \brief Start the new image's numbering above everything on the flash, so
 *        that an old volume table left behind cannot outrank the new one.
 */
static int device_format_sqnum(struct ubi_device *ubi);

/**
 * \brief Write both copies of an empty volume table, skipping blocks that
 *        will not take one.
 *
 * \retval 0
 *         Both are down.
 * \retval -ENOSPC
 *         Too few blocks took one.
 * \retval -EIO
 *         Too many blocks in a row failed.
 */
static int device_format_copies(struct ubi_device *ubi);

/**
 * \brief Erase every other volume table copy, which could otherwise stand in
 *        for the new ones once they are erased.
 */
static void device_format_forget(struct ubi_device *ubi);

/* Module variables and constants ------------------------------------------ */

/** Partitions a handle is attached to or a format is running on. */
static ATOMIC_DEFINE(device_partitions, DEVICE_PARTITIONS);

/* Static function definitions --------------------------------------------- */

static int device_geometry_measure(const struct flash_area *flash_area,
				   struct ubi_geometry *geometry)
{
	const struct device *device = flash_area_get_device(flash_area);

	if (NULL == device)
		return -EIO;

	struct flash_pages_info page = { 0 };
	int ret =
		flash_get_page_info_by_offs(device, flash_area->fa_off, &page);

	if (0 != ret)
		return -EIO;

	const struct flash_parameters *parameters =
		flash_get_parameters(device);

	if (NULL == parameters)
		return -EIO;

	const struct ubi_geometry measured = {
		.peb_count = flash_area->fa_size / page.size,
		.peb_size = page.size,
		.leb_size = (UBI_DATA_OFFSET < page.size) ?
				    page.size - UBI_DATA_OFFSET :
				    0,
		.write_block_size = parameters->write_block_size,
		.erase_value = parameters->erase_value,
	};

	*geometry = measured;

	if (UBI_DATA_OFFSET >= page.size)
		return -EINVAL;

	/* An erase meant for a block that is not exactly one erase block
	 * would reach into whatever lies next to it. */
	for (uint32_t pnum = 0; pnum < measured.peb_count; ++pnum) {
		const off_t at = flash_area->fa_off + (off_t)pnum * page.size;
		struct flash_pages_info next = { 0 };

		ret = flash_get_page_info_by_offs(device, at, &next);

		if (0 != ret || at != next.start_offset ||
		    page.size != next.size)
			return -EINVAL;
	}

	return 0;
}

static int device_geometry_validate(const struct ubi_geometry *geometry)
{
	/* Every volume this build allows has to fit one logical block, and
	 * every write has to land on a whole number of write blocks. */
	if (UBI_VOLUME_TABLE_DATA_MAX_SIZE > geometry->leb_size)
		return -EINVAL;

	if (0 == geometry->write_block_size ||
	    0 != UBI_HEADER_SIZE % geometry->write_block_size)
		return -EINVAL;

	if (UBI_MIN_PEB_COUNT > geometry->peb_count)
		return -EINVAL;

	if (UBI_MAX_PEB_COUNT < geometry->peb_count)
		return -ENOSPC;

	return 0;
}

static int device_open(struct ubi_device *ubi, const struct ubi_config *config)
{
	const struct flash_area *flash_area = NULL;
	int ret = flash_area_open(config->flash_area_id, &flash_area);

	if (0 != ret)
		return -EIO;

	ubi->flash_area = flash_area;

	ret = device_geometry_measure(flash_area, &ubi->geometry);

	if (0 != ret)
		goto close_partition;

	ret = device_geometry_validate(&ubi->geometry);

	if (0 != ret)
		goto close_partition;

	ubi->scratch = k_calloc(1, sizeof(*ubi->scratch));

	if (NULL == ubi->scratch) {
		ret = -ENOMEM;
		goto close_partition;
	}

	ret = ubi_impl_key_derive(config->ikm_key_id, config->key_context,
				  config->key_context_size, &ubi->keys.header,
				  &ubi->keys.volume_table);

	if (0 != ret)
		goto free_scratch;

	/* The volume holding the record has to be reachable before the record
	 * declares anything. */
	ubi->volume_table.volume.vol_id = UBI_VOLUME_TABLE_VOL_ID;
	ubi->volume_table.volume.leb_count = UBI_VOLUME_TABLE_LEB_COUNT;
	ubi->volume_table.volume.eba = ubi->volume_table.eba;
	strcpy(ubi->volume_table.volume.name, UBI_VOLUME_TABLE_NAME);

	memset(ubi->volume_table.eba, 0xFF, sizeof(ubi->volume_table.eba));

	return 0;

free_scratch:
	k_free(ubi->scratch);
	ubi->scratch = NULL;
close_partition:
	flash_area_close(flash_area);
	ubi->flash_area = NULL;

	return ret;
}

static void device_close(struct ubi_device *ubi)
{
	ubi_impl_key_destroy(&ubi->keys.header);
	ubi_impl_key_destroy(&ubi->keys.volume_table);
	k_free(ubi->scratch);
	ubi->scratch = NULL;
	flash_area_close(ubi->flash_area);
	ubi->flash_area = NULL;
}

static int device_tables_alloc(struct ubi_device *ubi)
{
	const uint32_t peb_count = ubi->geometry.peb_count;

	ubi->blocks.state = k_calloc(peb_count, sizeof(uint8_t));
	ubi->blocks.erase_count = k_calloc(peb_count, sizeof(uint32_t));
	ubi->blocks.protect = k_calloc(peb_count, sizeof(uint8_t));
	ubi->volumes.eba_pool = k_calloc(peb_count, sizeof(uint16_t));

	if (IS_ENABLED(CONFIG_UBI_SELF_CHECKS))
		ubi->blocks.named = k_calloc(UBI_NAMED_SIZE(peb_count), 1);

	const bool named_missing = IS_ENABLED(CONFIG_UBI_SELF_CHECKS) &&
				   NULL == ubi->blocks.named;

	if (NULL == ubi->blocks.state || NULL == ubi->blocks.erase_count ||
	    NULL == ubi->blocks.protect || NULL == ubi->volumes.eba_pool ||
	    named_missing) {
		device_tables_free(ubi);
		return -ENOMEM;
	}

	/* A zeroed state is UBI_PEB_UNKNOWN, but an unmapped entry and an
	 * unknown erase count are all ones. */
	memset(ubi->volumes.eba_pool, 0xFF, peb_count * sizeof(uint16_t));
	memset(ubi->blocks.erase_count, 0xFF, peb_count * sizeof(uint32_t));

	return 0;
}

static void device_tables_free(struct ubi_device *ubi)
{
	k_free(ubi->blocks.state);
	k_free(ubi->blocks.erase_count);
	k_free(ubi->blocks.protect);
	k_free(ubi->blocks.named);
	k_free(ubi->volumes.eba_pool);

	ubi->blocks.state = NULL;
	ubi->blocks.erase_count = NULL;
	ubi->blocks.protect = NULL;
	ubi->blocks.named = NULL;
	ubi->volumes.eba_pool = NULL;
}

static int device_claim(uint8_t flash_area_id)
{
	const bool taken =
		atomic_test_and_set_bit(device_partitions, flash_area_id);

	return taken ? -EBUSY : 0;
}

static void device_release(uint8_t flash_area_id)
{
	atomic_clear_bit(device_partitions, flash_area_id);
}

static int device_image_seq_draw(uint32_t *image_seq)
{
	/* Zero marks "no image". */
	for (uint32_t attempt = 0; attempt < IMAGE_SEQ_DRAW_LIMIT; ++attempt) {
		uint32_t drawn = 0;
		const psa_status_t status =
			psa_generate_random((uint8_t *)&drawn, sizeof(drawn));

		if (PSA_SUCCESS != status)
			return -EIO;

		if (0 != drawn) {
			*image_seq = drawn;
			return 0;
		}
	}

	return -EIO;
}

static int device_format_sqnum(struct ubi_device *ubi)
{
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		struct ubi_headers headers = { 0 };
		const int ret = ubi_impl_header_read(ubi, pnum, &headers);

		if (0 != ret)
			return ret;

		if (UBI_HEADER_OK == headers.vid_status &&
		    headers.vid.sqnum > ubi->max_sqnum)
			ubi->max_sqnum = headers.vid.sqnum;
	}

	return 0;
}

static int device_format_copies(struct ubi_device *ubi)
{
	struct ubi_volume_table_record *record = &ubi->scratch->record;
	uint32_t written = 0;
	uint32_t failures = 0;

	memset(record, 0, sizeof(*record));
	record->revision = 1;
	record->image_seq = ubi->image_seq;
	record->peb_size = ubi->geometry.peb_size;
	record->peb_count = ubi->geometry.peb_count;

	/* Both copies go down now, so that one complete copy survives every
	 * update from the start. */
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count &&
				written < UBI_VOLUME_TABLE_LEB_COUNT &&
				failures < FORMAT_FAILURES_IN_A_ROW;
	     ++pnum) {
		int ret = ubi_impl_peb_prepare(ubi, pnum);

		if (0 != ret) {
			LOG_WRN("PEB %u cannot be prepared (%d), trying the "
				"next one",
				pnum, ret);
			failures += 1;
			continue;
		}

		ubi->volume_table.eba[written] = (uint16_t)pnum;

		ret = ubi_impl_volume_table_write(ubi, written, record);

		if (0 != ret) {
			LOG_WRN("PEB %u cannot hold a volume table copy (%d), "
				"trying the next one",
				pnum, ret);
			ubi->volume_table.eba[written] = UBI_LEB_UNMAPPED;
			failures += 1;
			continue;
		}

		failures = 0;
		written += 1;
	}

	if (UBI_VOLUME_TABLE_LEB_COUNT == written)
		return 0;

	LOG_ERR("partition %u: only %u of the %d volume table copies could be "
		"written",
		ubi->flash_area->fa_id, written, UBI_VOLUME_TABLE_LEB_COUNT);

	return (FORMAT_FAILURES_IN_A_ROW == failures) ? -EIO : -ENOSPC;
}

static void device_format_forget(struct ubi_device *ubi)
{
	for (uint32_t pnum = 0; pnum < ubi->geometry.peb_count; ++pnum) {
		struct ubi_headers headers = { 0 };

		if (pnum == ubi->volume_table.eba[0] ||
		    pnum == ubi->volume_table.eba[1])
			continue;

		int ret = ubi_impl_header_read(ubi, pnum, &headers);

		if (0 != ret || UBI_HEADER_OK != headers.vid_status ||
		    UBI_VOLUME_TABLE_VOL_ID != headers.vid.vol_id)
			continue;

		ret = ubi_impl_peb_prepare(ubi, pnum);

		if (0 != ret) {
			LOG_WRN("PEB %u: holds an earlier volume table copy "
				"and could not be erased (%d)",
				pnum, ret);
		}
	}
}

/* Module interface function definitions ----------------------------------- */

int ubi_impl_device_format(const struct ubi_config *config)
{
	struct ubi_device *ubi = NULL;
	int ret = device_claim(config->flash_area_id);

	if (0 != ret) {
		LOG_ERR("partition %u is attached; detach it before formatting",
			config->flash_area_id);
		return ret;
	}

	/* A format needs the partition, geometry and keys of a handle, but
	 * none of its per-block bookkeeping. */
	ubi = k_calloc(1, sizeof(*ubi));

	if (NULL == ubi) {
		LOG_ERR("no memory for a device handle of %zu bytes",
			sizeof(*ubi));
		ret = -ENOMEM;
		goto release_partition;
	}

	ret = device_open(ubi, config);

	if (0 != ret) {
		LOG_ERR("cannot open partition %u (%d): %u blocks of %u bytes, "
			"write granularity %u",
			config->flash_area_id, ret, ubi->geometry.peb_count,
			ubi->geometry.peb_size, ubi->geometry.write_block_size);
		goto free_handle;
	}

	ret = device_image_seq_draw(&ubi->image_seq);

	if (0 != ret) {
		LOG_ERR("cannot draw an image sequence number for partition %u",
			config->flash_area_id);
		goto close_partition;
	}

	ret = device_format_sqnum(ubi);

	if (0 != ret) {
		LOG_ERR("partition %u cannot be read through (%d)",
			config->flash_area_id, ret);
		goto close_partition;
	}

	ret = device_format_copies(ubi);

	if (0 != ret)
		goto close_partition;

	device_format_forget(ubi);

	LOG_INF("formatted partition %u of %u blocks: image_seq=0x%08x, "
		"volume table in PEB %u and PEB %u",
		config->flash_area_id, ubi->geometry.peb_count, ubi->image_seq,
		ubi->volume_table.eba[0], ubi->volume_table.eba[1]);

close_partition:
	device_close(ubi);
free_handle:
	k_free(ubi);
release_partition:
	device_release(config->flash_area_id);

	return ret;
}

int ubi_impl_device_init(struct ubi_device *ubi,
			 const struct ubi_config *config)
{
	int ret = 0;

	memset(ubi, 0, sizeof(*ubi));
	k_mutex_init(&ubi->lock);

	ubi->callbacks.event = config->event_cb;
	ubi->callbacks.state = config->state_cb;
	ubi->callbacks.user_context = config->user_context;

	ret = device_claim(config->flash_area_id);

	if (0 != ret) {
		LOG_ERR("partition %u is attached or being formatted already",
			config->flash_area_id);
		return ret;
	}

	ret = device_open(ubi, config);

	if (0 != ret) {
		LOG_ERR("cannot open partition %u (%d): %u blocks of %u bytes, "
			"write granularity %u",
			config->flash_area_id, ret, ubi->geometry.peb_count,
			ubi->geometry.peb_size, ubi->geometry.write_block_size);
		goto release_partition;
	}

	ret = device_tables_alloc(ubi);

	if (0 != ret) {
		LOG_ERR("no memory for the bookkeeping of %u erase blocks",
			ubi->geometry.peb_count);
		goto close_partition;
	}

	ret = ubi_impl_attach(ubi);

	if (0 != ret)
		goto free_tables;

	if (IS_ENABLED(CONFIG_UBI_SELF_CHECKS)) {
		ret = ubi_impl_state_self_check(ubi);

		if (0 != ret)
			goto free_tables;
	}

	ret = ubi_impl_state_check(ubi);

	if (0 != ret) {
		LOG_ERR("partition %u: the state check refused this device",
			config->flash_area_id);
		goto free_tables;
	}

	ubi->magic = UBI_DEVICE_MAGIC;

	ubi_impl_shell_register(ubi);

	LOG_INF("attached partition %u: image_seq=0x%08x, %u volumes, "
		"revision %u",
		config->flash_area_id, ubi->image_seq, ubi->volumes.count,
		ubi->volumes.revision);

	return 0;

free_tables:
	device_tables_free(ubi);
close_partition:
	device_close(ubi);
release_partition:
	device_release(config->flash_area_id);
	memset(ubi, 0, sizeof(*ubi));

	return ret;
}

void ubi_impl_device_deinit(struct ubi_device *ubi)
{
	const uint8_t flash_area_id = ubi->flash_area->fa_id;

	ubi_impl_shell_unregister(ubi);

	device_tables_free(ubi);
	device_close(ubi);
	device_release(flash_area_id);

	memset(ubi, 0, sizeof(*ubi));
}
