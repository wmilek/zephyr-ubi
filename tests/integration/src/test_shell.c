/**
 * \file    test_shell.c
 * \author  Kamil Kielbasa
 * \brief   The diagnostic shell commands, run through the dummy backend.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "partition.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/** Longest command a test builds. */
#define COMMAND_SIZE (64)

/** Longest line a test looks for. */
#define LINE_SIZE (96)

/** The partition under test, the way the shell names it. */
#define SHELL_PARTITION ((uint32_t)UBI_TEST_PARTITION_ID)

/* Static function declarations -------------------------------------------- */

/**
 * \brief Run one command and keep what it printed.
 *
 * \return What the command returned.
 */
static int run(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * \brief Report whether the last command printed \p text.
 */
static bool printed(const char *text);

/**
 * \brief Physical block behind a logical one, straight from the handle.
 */
static uint32_t leb_peb(uint32_t vol_id, uint32_t lnum);

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_shell);

/** What the last command printed. */
static const char *output = "";

/* Static function definitions --------------------------------------------- */

static int run(const char *format, ...)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	char command[COMMAND_SIZE] = { 0 };
	size_t size = 0;
	va_list args;

	zassert_true(WAIT_FOR(shell_ready(sh), 1000000, k_msleep(1)),
		     "the shell never came up");

	va_start(args, format);
	vsnprintf(command, sizeof(command), format, args);
	va_end(args);

	shell_backend_dummy_clear_output(sh);

	const int ret = shell_execute_cmd(sh, command);

	output = shell_backend_dummy_get_output(sh, &size);

	return ret;
}

static bool printed(const char *text)
{
	return NULL != strstr(output, text);
}

static uint32_t leb_peb(uint32_t vol_id, uint32_t lnum)
{
	for (uint32_t i = 0; i < ubi->volumes.count; ++i) {
		if (ubi->volumes.entries[i].vol_id == vol_id)
			return ubi->volumes.entries[i].eba[lnum];
	}

	ztest_test_fail();

	return UINT32_MAX;
}

/* Module interface function definitions ----------------------------------- */

/* Tests: devices ---------------------------------------------------------- */

/*
 * Given: no device attached.
 * When:  the devices are listed, and one is asked for.
 * Then:  the list says there are none, and the request fails.
 */
ZTEST(ubi_shell, test_no_device_is_listed_before_an_attach)
{
	zassert_ok(run("ubi list"));
	zassert_true(printed("no UBI device is attached"), "%s", output);

	zassert_not_equal(0, run("ubi info %u", SHELL_PARTITION));
	zassert_true(printed("no attached device"), "%s", output);
}

/*
 * Given: a device attached, then detached.
 * When:  the devices are listed each time.
 * Then:  it is listed by its partition only while it is attached.
 */
ZTEST(ubi_shell, test_a_device_is_listed_while_attached)
{
	char line[LINE_SIZE] = { 0 };

	volume_ready(UBI_TEST_VOLUME_LEBS);

	snprintf(line, sizeof(line), "%9u ", SHELL_PARTITION);

	zassert_ok(run("ubi list"));
	zassert_true(printed(line), "%s", output);
	zassert_true(printed(" ok"), "%s", output);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(run("ubi list"));
	zassert_true(printed("no UBI device is attached"), "%s", output);
}

/*
 * Given: a device with one volume.
 * When:  its information is shown.
 * Then:  it carries the volume count, the block counts and the trust state.
 */
ZTEST(ubi_shell, test_info_describes_the_device)
{
	char line[LINE_SIZE] = { 0 };

	volume_ready(UBI_TEST_VOLUME_LEBS);

	snprintf(line, sizeof(line), "%u PEBs of %u bytes",
		 (uint32_t)UBI_TEST_PEB_COUNT, (uint32_t)UBI_TEST_PEB_SIZE);

	zassert_ok(run("ubi info %u", SHELL_PARTITION));
	zassert_true(printed(line), "%s", output);
	zassert_true(printed("volumes:      1 of"), "%s", output);
	zassert_true(printed("writable, trusted"), "%s", output);
}

/* Tests: volumes and mapping ---------------------------------------------- */

/*
 * Given: a volume with two of its blocks written.
 * When:  the volumes are listed and the volume's mapping is shown, by name
 *        and by identifier.
 * Then:  the volume is listed with two blocks mapped, and the mapping names
 *        the physical block behind each written one.
 */
ZTEST(ubi_shell, test_map_shows_the_physical_block_behind_each_leb)
{
	uint8_t data[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	char line[LINE_SIZE] = { 0 };
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

	zassert_ok(ubi_leb_change(ubi, vol_id, 1, data, sizeof(data)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 3, data, sizeof(data)));

	snprintf(line, sizeof(line), "%10u  %-16s %6u %6u", vol_id, "logs",
		 (uint32_t)UBI_TEST_VOLUME_LEBS, 2U);

	zassert_ok(run("ubi volumes %u", SHELL_PARTITION));
	zassert_true(printed(line), "%s", output);

	zassert_ok(run("ubi map %u logs", SHELL_PARTITION));
	zassert_true(printed("\"logs\": 4 LEBs, 2 mapped"), "%s", output);

	for (uint32_t lnum = 1; lnum <= 3; lnum += 2) {
		snprintf(line, sizeof(line), "  %6u %6u %10u  mapped", lnum,
			 leb_peb(vol_id, lnum), leb_wear(vol_id, lnum));
		zassert_true(printed(line), "no \"%s\" in:\n%s", line, output);
	}

	zassert_ok(run("ubi map %u %u", SHELL_PARTITION, vol_id));
	zassert_true(printed("\"logs\": 4 LEBs, 2 mapped"), "%s", output);
}

/*
 * Given: a volume with a block written.
 * When:  the mapping of every volume is shown.
 * Then:  both copies of the volume table come first, then the volume.
 */
ZTEST(ubi_shell, test_map_without_a_volume_shows_them_all)
{
	uint8_t data[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(run("ubi map %u", SHELL_PARTITION));

	const char *table = strstr(output, "volume table (internal): 2 LEBs, "
					   "2 mapped");
	const char *logs = strstr(output, "\"logs\": 4 LEBs, 1 mapped");

	zassert_not_null(table, "%s", output);
	zassert_not_null(logs, "%s", output);
	zassert_true(table < logs, "%s", output);
}

/*
 * Given: a device.
 * When:  a volume it does not have is asked for.
 * Then:  the command fails and says so.
 */
ZTEST(ubi_shell, test_map_of_a_missing_volume_fails)
{
	volume_ready(UBI_TEST_VOLUME_LEBS);

	zassert_not_equal(0, run("ubi map %u nothing", SHELL_PARTITION));
	zassert_true(printed("no volume \"nothing\""), "%s", output);
}

/* Tests: physical blocks -------------------------------------------------- */

/*
 * Given: a volume with a block written.
 * When:  the physical block behind it is shown.
 * Then:  it is mapped, carries its erase count, and names its owner.
 */
ZTEST(ubi_shell, test_pebs_names_the_owner_of_a_block)
{
	uint8_t data[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	char line[LINE_SIZE] = { 0 };
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

	zassert_ok(ubi_leb_change(ubi, vol_id, 2, data, sizeof(data)));

	const uint32_t pnum = leb_peb(vol_id, 2);
	char owner[16] = { 0 };

	snprintf(owner, sizeof(owner), "%u:%u", vol_id, 2U);
	snprintf(line, sizeof(line), "%6u  %-10s %10u", pnum, "mapped",
		 leb_wear(vol_id, 2));

	zassert_ok(run("ubi pebs %u %u 1", SHELL_PARTITION, pnum));
	zassert_true(printed(line), "no \"%s\" in:\n%s", line, output);
	zassert_true(printed(owner), "%s", output);

	for (uint32_t copy = 0; copy < 2; ++copy) {
		snprintf(owner, sizeof(owner), "vt:%u", copy);

		zassert_ok(run("ubi pebs %u %u 1", SHELL_PARTITION,
			       (uint32_t)ubi->volume_table.eba[copy]));
		zassert_true(printed(owner), "%s", output);
	}
}

/*
 * Given: a device.
 * When:  a range starting past the last block is asked for.
 * Then:  the command fails and says so.
 */
ZTEST(ubi_shell, test_pebs_past_the_end_fails)
{
	volume_ready(UBI_TEST_VOLUME_LEBS);

	zassert_not_equal(0, run("ubi pebs %u %u", SHELL_PARTITION,
				 (uint32_t)UBI_TEST_PEB_COUNT));
	zassert_true(printed("the device has PEBs 0 to"), "%s", output);

	zassert_not_equal(0, run("ubi ec %u x", SHELL_PARTITION));
	zassert_true(printed("\"x\" is not a number"), "%s", output);
}

/*
 * Given: a freshly formatted device.
 * When:  the erase counts of its first blocks are shown.
 * Then:  the summary carries the spread, one line holds eight counts, and
 *        the blocks never formatted show the count attach estimated.
 */
ZTEST(ubi_shell, test_ec_summarises_and_lists_the_erase_counts)
{
	volume_ready(UBI_TEST_VOLUME_LEBS);

	zassert_ok(run("ubi ec %u 0 10", SHELL_PARTITION));
	zassert_true(printed("healthy PEBs: min"), "%s", output);
	zassert_true(printed(" ~"), "no estimated count in:\n%s", output);
	zassert_true(printed("     0:"), "%s", output);
	zassert_true(printed("     8:"), "%s", output);
	zassert_false(printed("    16:"), "%s", output);
}
