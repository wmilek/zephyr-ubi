/**
 * \file    test_leb.c
 * \author  Kamil Kielbasa
 * \brief   Mapping, reading, writing and appending to logical erase blocks.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdint.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "partition.h"
#include "suite.h"

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_leb);

/* What a part accepts is the build's to decide, so it selects the tests. */
UBI_TEST_SUITE(ubi_alignment);

/* Module interface function definitions ----------------------------------- */

/* Tests: the mapping ------------------------------------------------------ */

/*
 * Given: a fresh volume that has mapped nothing.
 * When:  a logical block nobody wrote is read.
 * Then:  it reads as erased, because nothing behind it reads the same as a
 *        block nobody has written.
 */
ZTEST(ubi_leb, test_an_unmapped_block_reads_as_erased)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };
	uint8_t erased[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	memset(erased, UBI_TEST_ERASED, sizeof(erased));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped, "a fresh volume maps nothing");
	zassert_equal(0, info.erase_count);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(erased, read, sizeof(read));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a fresh volume.
 * When:  one of its logical blocks is mapped.
 * Then:  a physical block backs it, the mapping cost a sequence number, and
 *        what it holds reads as erased.
 */
ZTEST(ubi_leb, test_mapping_gives_an_empty_block)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_leb_info info = { 0 };
	uint8_t erased[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	memset(erased, UBI_TEST_ERASED, sizeof(erased));

	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_leb_map(ubi, vol_id, 1));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(before.max_sqnum + 1, after.max_sqnum,
		      "mapping seals one header, so it spends one sequence "
		      "number");

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 1, &info));
	zassert_true(info.mapped);

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)));
	zassert_mem_equal(erased, read, sizeof(read));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a logical block that is already mapped.
 * When:  it is mapped a second time.
 * Then:  the library refuses and writes nothing, because mapping is a
 *        transition rather than a state to assert.
 */
ZTEST(ubi_leb, test_mapping_a_mapped_block_is_refused)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_equal(-EEXIST, ubi_leb_map(ubi, vol_id, 0));

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.max_sqnum, after.max_sqnum,
		      "a refusal must not write anything");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a mapped logical block.
 * When:  it is unmapped, twice.
 * Then:  the physical block joins the reclaim queue, the mapping is gone, and
 *        asking again is not an error.
 */
ZTEST(ubi_leb, test_unmapping_queues_the_block_for_reclaim)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_leb_info info = { 0 };

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(before.reclaimable_pebs + 1, after.reclaimable_pebs);

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped);

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a volume with one block mapped and another left alone.
 * When:  the device is detached and attached again.
 * Then:  the mapping is rebuilt from the flash, and the block nobody mapped
 *        stays unmapped.
 */
ZTEST(ubi_leb, test_a_mapping_survives_a_reattach)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };

	zassert_ok(ubi_leb_map(ubi, vol_id, 2));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 2, &info));
	zassert_true(info.mapped, "the mapping has to be rebuilt from flash");

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 1, &info));
	zassert_false(info.mapped);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: writing through the mapping -------------------------------------- */

/*
 * Given: a logical block written with a whole-block change.
 * When:  the device is detached and attached again.
 * Then:  the block reads back exactly what was written.
 */
ZTEST(ubi_leb, test_a_change_is_readable_after_a_reattach)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x11);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));

	zassert_mem_equal(written, read, sizeof(written));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a logical block holding data, and one never written.
 * When:  a change, an append and a read are asked for with a length of zero.
 * Then:  each succeeds and nothing is written, because a degenerate length
 *        must not spend an erase, mapped or not.
 */
ZTEST(ubi_leb, test_a_length_of_zero_does_nothing)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x44);
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, NULL, 0));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, NULL, 0));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, 0));
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, read, 0));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, NULL, 0));

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.max_sqnum, after.max_sqnum);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written),
			  "the contents had to stay exactly as they were");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: an unmapped logical block.
 * When:  it is appended to without being mapped first.
 * Then:  the write takes a block on its own and the record reads back.
 */
ZTEST(ubi_leb, test_an_append_maps_the_block_it_needs)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_leb_info info = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x55);

	zassert_ok(
		ubi_leb_write_at(ubi, vol_id, 0, 0, written, sizeof(written)));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_true(info.mapped);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a mapped logical block.
 * When:  a read, an append or a change is asked for past the end of it.
 * Then:  each is refused, while exactly the whole block still goes through.
 */
ZTEST(ubi_leb, test_a_range_past_the_end_of_a_block_is_refused)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info device = { 0 };
	uint8_t buffer[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(buffer, sizeof(buffer), 0x66);
	zassert_ok(ubi_device_get_info(ubi, &device));
	zassert_ok(ubi_leb_map(ubi, vol_id, 0));

	zassert_equal(-EINVAL, ubi_leb_read(ubi, vol_id, 0, device.leb_size - 1,
					    buffer, sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, vol_id, 0, device.leb_size,
						buffer, sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_change(ubi, vol_id, 0, buffer,
					      device.leb_size + 1));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0,
				device.leb_size - sizeof(buffer), buffer,
				sizeof(buffer)));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a volume, and the first logical block number past its end.
 * When:  that block, or a volume that does not exist, is named.
 * Then:  the block number is refused as an argument and the volume as a
 *        missing one, so the two mistakes stay distinguishable.
 */
ZTEST(ubi_leb, test_a_block_the_volume_does_not_reach_is_refused)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	const uint32_t past_end = UBI_TEST_VOLUME_LEBS;
	struct ubi_leb_info info = { 0 };
	uint8_t buffer[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(buffer, sizeof(buffer), 0x77);

	zassert_equal(-EINVAL, ubi_leb_map(ubi, vol_id, past_end));
	zassert_equal(-EINVAL, ubi_leb_unmap(ubi, vol_id, past_end));
	zassert_equal(-EINVAL, ubi_leb_get_info(ubi, vol_id, past_end, &info));
	zassert_equal(-EINVAL, ubi_leb_read(ubi, vol_id, past_end, 0, buffer,
					    sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_change(ubi, vol_id, past_end, buffer,
					      sizeof(buffer)));

	zassert_equal(-ENOENT, ubi_leb_map(ubi, vol_id + 1, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device whose every block is either in use or waiting for reclaim,
 *        and an application that never runs reclaim.
 * When:  it keeps changing its blocks, many more times than there are blocks.
 * Then:  every change goes through, because a block waiting for reclaim is
 *        erased on the spot once nothing else is left.
 */
ZTEST(ubi_leb, test_changes_carry_on_when_nothing_is_reclaimed)
{
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	const uint32_t leb_count = volume_under_load(&vol_id);

	zassert_ok(ubi_device_get_info(ubi, &info));

	for (uint32_t round = 0; round < 2 * info.peb_count; ++round) {
		pattern_fill(written, sizeof(written), (uint8_t)round);
		zassert_ok(ubi_leb_change(ubi, vol_id, round % leb_count,
					  written, sizeof(written)),
			   "change %u ran out of blocks", round);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: what the write block size imposes -------------------------------- */

#if UBI_TEST_WRITE_BLOCK > 1

/*
 * Given: a flash whose writes must be whole blocks.
 * When:  a change or an append is asked for with a length that is not.
 * Then:  both are refused, and a whole number of write blocks still goes
 *        through and survives a reattach.
 */
ZTEST(ubi_alignment, test_a_length_that_is_not_a_write_block_is_refused)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xD1);

	zassert_equal(-EINVAL, ubi_leb_change(ubi, vol_id, 0, written,
					      sizeof(written) - 1));
	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, vol_id, 1, 0, written,
						sizeof(written) - 1));

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a flash whose writes must be whole blocks.
 * When:  an append is asked for at an offset that is not one.
 * Then:  it is refused, because the offset decides where the next write may
 *        start; an aligned offset goes through.
 */
ZTEST(ubi_alignment, test_an_offset_that_is_not_a_write_block_is_refused)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xE2);

	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, vol_id, 0, 1, written,
						UBI_TEST_WRITE_BLOCK));

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, UBI_TEST_WRITE_BLOCK,
				    written, UBI_TEST_WRITE_BLOCK));

	zassert_ok(ubi_device_deinit(ubi));
}

#else /* UBI_TEST_WRITE_BLOCK > 1 */

/*
 * Given: a flash that takes a single byte at a time.
 * When:  a change and an append are asked for with an odd length and offset.
 * Then:  both go through, because there is no alignment for such a part to
 *        refuse.
 */
ZTEST(ubi_alignment, test_a_byte_addressable_flash_refuses_no_length)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xF3);

	zassert_ok(
		ubi_leb_change(ubi, vol_id, 0, written, sizeof(written) - 1));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, 1, written,
				    sizeof(written) - 1));

	zassert_ok(ubi_device_deinit(ubi));
}

#endif /* UBI_TEST_WRITE_BLOCK > 1 */

/* Tests: appending -------------------------------------------------------- */

/*
 * Given: a fresh volume.
 * When:  one of its blocks is appended to, record by record, up to its last
 *        byte.
 * Then:  nothing more fits: an append at the end, or one straddling it, is
 *        refused and leaves the flash as it was, and every record reads back
 *        after a reattach.
 */
ZTEST(ubi_leb, test_appends_fill_a_block_to_its_last_byte)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	struct ubi_device_info device = { 0 };
	uint8_t record[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint32_t before = 0;
	uint32_t offset = 0;
	uint8_t seed = 0;

	zassert_ok(ubi_device_get_info(ubi, &device));

	while (offset < device.leb_size) {
		const uint32_t length =
			MIN(UBI_TEST_PAYLOAD_SIZE, device.leb_size - offset);

		pattern_fill(record, length, seed);
		zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, offset, record,
					    length));
		offset += length;
		seed++;
	}

	before = partition_fingerprint();

	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, vol_id, 0, device.leb_size,
						record, UBI_TEST_WRITE_BLOCK));
	zassert_equal(-EINVAL,
		      ubi_leb_write_at(ubi, vol_id, 0,
				       device.leb_size - UBI_TEST_WRITE_BLOCK,
				       record, 2 * UBI_TEST_WRITE_BLOCK));

	zassert_equal(before, partition_fingerprint(),
		      "a refused append must not reach the flash");

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	offset = 0;
	seed = 0;

	while (offset < device.leb_size) {
		const uint32_t length =
			MIN(UBI_TEST_PAYLOAD_SIZE, device.leb_size - offset);

		pattern_fill(record, length, seed);
		zassert_ok(ubi_leb_read(ubi, vol_id, 0, offset, read, length));
		zassert_mem_equal(record, read, length,
				  "the record at offset %u", offset);
		offset += length;
		seed++;
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block holding two appended records.
 * When:  the device is detached and attached again, and a third record is
 *        appended right after them.
 * Then:  all three read back after another reattach: the attach hands the
 *        block back as it was, and the caller's offset still holds.
 */
ZTEST(ubi_leb, test_appending_carries_on_after_a_reattach)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t first[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t second[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t third[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(first, sizeof(first), 0x6C);
	pattern_fill(second, sizeof(second), 0x7D);
	pattern_fill(third, sizeof(third), 0x8E);

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, first, sizeof(first)));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, UBI_TEST_PAYLOAD_SIZE,
				    second, sizeof(second)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 2 * UBI_TEST_PAYLOAD_SIZE,
				    third, sizeof(third)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(first, read, sizeof(first));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, UBI_TEST_PAYLOAD_SIZE, read,
				sizeof(read)));
	zassert_mem_equal(second, read, sizeof(second));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 2 * UBI_TEST_PAYLOAD_SIZE, read,
				sizeof(read)));
	zassert_mem_equal(third, read, sizeof(third));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block holding two appended records.
 * When:  it is unmapped, and a new record is appended at offset 0.
 * Then:  the new record starts a fresh block: the rest of it reads as erased
 *        while the old block waits on the flash for reclaim, and the fresh
 *        block still wins after a reattach.
 */
ZTEST(ubi_leb, test_after_an_unmap_an_append_starts_on_a_fresh_block)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t first[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t second[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t fresh[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t erased[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(first, sizeof(first), 0x9F);
	pattern_fill(second, sizeof(second), 0xB0);
	pattern_fill(fresh, sizeof(fresh), 0xC1);
	memset(erased, UBI_TEST_ERASED, sizeof(erased));

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, first, sizeof(first)));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, UBI_TEST_PAYLOAD_SIZE,
				    second, sizeof(second)));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, fresh, sizeof(fresh)));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(fresh, read, sizeof(fresh));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, UBI_TEST_PAYLOAD_SIZE, read,
				sizeof(read)));
	zassert_mem_equal(erased, read, sizeof(read),
			  "the second record stayed with the old block");

	zassert_equal(1, count_data_matching(first, sizeof(first)),
		      "the old block is queued, not erased");

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(fresh, read, sizeof(fresh),
			  "the fresh block has to win the attach");

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, UBI_TEST_PAYLOAD_SIZE, read,
				sizeof(read)));
	zassert_mem_equal(erased, read, sizeof(read));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block changed and then appended to past what the change claimed.
 * When:  the device is attached.
 * Then:  both regions read back, because the seal covers only the bytes the
 *        change really wrote.
 */
ZTEST(ubi_leb, test_an_append_after_a_change_survives_the_attach)
{
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);
	uint8_t changed[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t appended[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(changed, sizeof(changed), 0x4A);
	pattern_fill(appended, sizeof(appended), 0x5B);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, changed, sizeof(changed)));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, sizeof(changed), appended,
				    sizeof(appended)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(changed, read, sizeof(changed));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, sizeof(changed), read,
				sizeof(read)));
	zassert_mem_equal(appended, read, sizeof(appended));

	zassert_ok(ubi_device_deinit(ubi));
}
