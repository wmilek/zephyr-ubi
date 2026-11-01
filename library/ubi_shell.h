/**
 * \file    ubi_shell.h
 * \author  Kamil Kielbasa
 * \brief   Diagnostic shell commands, and the list of attached devices they
 *          walk.
 *
 *          The commands hold the list for as long as they run, so a device
 *          cannot detach under them, and lock a device only while they copy
 *          a few rows out of it, never while they print. Without
 *          \c CONFIG_UBI_SHELL registering and unregistering compile to
 *          nothing.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef UBI_SHELL_H
#define UBI_SHELL_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdint.h>

/* Zephyr headers: */
#if defined(CONFIG_UBI_SHELL)
#include <zephyr/shell/shell.h>
#endif

/* UBI headers: */
#include "ubi_private.h"

/* Defines ----------------------------------------------------------------- */

/** Characters of an erase count as printed, NUL included. */
#define UBI_SHELL_EC_TEXT_SIZE (12)

/** Rows copied out under the device lock before they are printed. */
#define UBI_SHELL_ROWS (16)

#if !defined(CONFIG_UBI_SHELL)
#define ubi_impl_shell_register(ubi) ARG_UNUSED(ubi)
#define ubi_impl_shell_unregister(ubi) ARG_UNUSED(ubi)
#endif

/* Module interface function declarations ---------------------------------- */

#if defined(CONFIG_UBI_SHELL)

/**
 * \brief Make a device just attached visible to the shell.
 *
 * \param[in,out] ubi                   Attached device.
 */
void ubi_impl_shell_register(struct ubi_device *ubi);

/**
 * \brief Take a device about to detach out of the shell's sight, waiting
 *        for any command looking at it to finish.
 *
 * \param[in,out] ubi                   Attached device.
 */
void ubi_impl_shell_unregister(struct ubi_device *ubi);

/**
 * \brief Hold the device list and find the device a command names, by
 *        partition number or label.
 *
 * \retval 0
 *         Found; release the list with ubi_impl_shell_give().
 * \retval -ENODEV
 *         No such device; the list is released and the error printed.
 */
int ubi_impl_shell_take(const struct shell *sh, const char *name,
			struct ubi_device **ubi);

/**
 * \brief Release the device list.
 */
void ubi_impl_shell_give(void);

/**
 * \brief Lock a device while rows are copied out of it.
 *
 * \retval 0
 *         Locked.
 * \retval -EBUSY
 *         Another operation held it too long; the error is printed.
 */
int ubi_impl_shell_lock(const struct shell *sh, struct ubi_device *ubi);

/**
 * \brief Unlock a device.
 */
void ubi_impl_shell_unlock(struct ubi_device *ubi);

/**
 * \brief Name a block state the way the commands print it.
 */
const char *ubi_impl_shell_state_name(uint32_t state);

/**
 * \brief Format an erase count: "~" in front of one attach estimated for a
 *        block with no usable erase counter header, "?" for none at all.
 *
 * \return \p text, or a constant string.
 */
const char *ubi_impl_shell_ec_text(uint32_t state, uint32_t erase_count,
				   char text[UBI_SHELL_EC_TEXT_SIZE]);

#endif

#endif /* UBI_SHELL_H */
