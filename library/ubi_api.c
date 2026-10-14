/**
 * \file    ubi_api.c
 * \author  Kamil Kielbasa
 * \brief   The boundary between the application and the library.
 *
 *          Every function \c <ubi/ubi.h> declares is defined here. Each one
 *          checks its arguments, takes the device lock, clears a write with
 *          the state guard, and delegates.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_device.h"
#include "ubi_leb.h"
#include "ubi_maintenance.h"
#include "ubi_private.h"
#include "ubi_state.h"
#include "ubi_volume.h"
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations -------------------------------------------- */

/**
 * \brief Reject a configuration that cannot drive the library.
 */
static int api_config_validate(const struct ubi_config *config);

/**
 * \brief Report whether a handle is one this library attached.
 */
static bool api_device_is_attached(const struct ubi_device *ubi);

/**
 * \brief Report whether an identifier is one the application may name.
 */
static bool api_vol_id_is_public(uint32_t vol_id);

/**
 * \brief Report whether an operation is one this build performs.
 */
static bool api_maintenance_op_is_known(enum ubi_maintenance_op operation);

/**
 * \brief Take the device lock, unless the caller is one of the device's own
 *        callbacks.
 *
 * \retval 0
 *         Locked.
 * \retval -EDEADLK
 *         Called from a callback; nothing was locked.
 */
static int api_lock(struct ubi_device *ubi);

/**
 * \brief Release the lock, after the self-check under
 *        \c CONFIG_UBI_SELF_CHECKS.
 *
 * \return \p ret, or \c -EFAULT when the bookkeeping contradicts itself.
 */
static int api_unlock(struct ubi_device *ubi, int ret);

/* Static function definitions --------------------------------------------- */

static int api_config_validate(const struct ubi_config *config)
{
	if (NULL == config)
		return -EINVAL;

	if (PSA_KEY_ID_NULL == config->ikm_key_id)
		return -EINVAL;

	if (NULL == config->event_cb || NULL == config->state_cb)
		return -EINVAL;

	if (UBI_KEY_CONTEXT_MAX_SIZE < config->key_context_size ||
	    (NULL == config->key_context && 0 != config->key_context_size))
		return -EINVAL;

	return 0;
}

static bool api_device_is_attached(const struct ubi_device *ubi)
{
	return (NULL != ubi) && (UBI_DEVICE_MAGIC == ubi->magic);
}

static bool api_vol_id_is_public(uint32_t vol_id)
{
	return (UBI_VOLUME_TABLE_VOL_ID != vol_id) &&
	       (UBI_VOL_ID_INVALID != vol_id);
}

static bool api_maintenance_op_is_known(enum ubi_maintenance_op operation)
{
	switch (operation) {
	case UBI_MAINTENANCE_RECLAIM:
	case UBI_MAINTENANCE_RELOCATE:
	case UBI_MAINTENANCE_REPAIR:
	case UBI_MAINTENANCE_DISCARD:
		return true;
	default:
		return false;
	}
}

static int api_lock(struct ubi_device *ubi)
{
	k_mutex_lock(&ubi->lock, K_FOREVER);

	/* The lock is recursive, so a callback calling back in would find the
	 * device half way through the operation that called it. */
	if (!ubi->in_callback)
		return 0;

	k_mutex_unlock(&ubi->lock);

	LOG_ERR("a callback called back into UBI");

	return -EDEADLK;
}

static int api_unlock(struct ubi_device *ubi, int ret)
{
	if (IS_ENABLED(CONFIG_UBI_SELF_CHECKS)) {
		const int checked = ubi_impl_state_self_check(ubi);

		if (0 != checked)
			ret = checked;
	}

	k_mutex_unlock(&ubi->lock);

	return ret;
}

/* Module interface function definitions ----------------------------------- */

size_t ubi_device_size(void)
{
	return sizeof(struct ubi_device);
}

int ubi_device_format(const struct ubi_config *config)
{
	const int ret = api_config_validate(config);

	if (0 != ret) {
		LOG_ERR("format needs a key handle and both callbacks");
		return ret;
	}

	return ubi_impl_device_format(config);
}

int ubi_device_init(struct ubi_device *ubi, const struct ubi_config *config)
{
	const bool attached = api_device_is_attached(ubi);

	if (NULL == ubi) {
		LOG_ERR("attach needs a device handle");
		return -EINVAL;
	}

	if (attached) {
		LOG_ERR("this handle is already attached");
		return -EBUSY;
	}

	const int ret = api_config_validate(config);

	if (0 != ret) {
		LOG_ERR("attach needs a key handle and both callbacks");
		return ret;
	}

	/* Nothing to lock yet: the mutex lives in the handle this call is
	 * about to build. */
	return ubi_impl_device_init(ubi, config);
}

int ubi_device_deinit(struct ubi_device *ubi)
{
	const bool attached = api_device_is_attached(ubi);

	if (!attached) {
		LOG_ERR("this handle is not attached");
		return -EINVAL;
	}

	/* A callback runs under the operation that called it, which would
	 * carry on with the handle gone. */
	if (ubi->in_callback) {
		LOG_ERR("a callback tried to detach its own device");
		return -EDEADLK;
	}

	/* Not locked: detaching a handle another thread is using is a defect
	 * the library cannot paper over. */
	ubi_impl_device_deinit(ubi);

	return 0;
}

int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info)
{
	const bool attached = api_device_is_attached(ubi);

	if (!attached || NULL == info) {
		LOG_ERR("device info needs an attached handle");
		return -EINVAL;
	}

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_state_describe(ubi, info);

	return api_unlock(ubi, ret);
}

int ubi_volume_create(struct ubi_device *ubi,
		      const struct ubi_volume_config *config, uint32_t *vol_id)
{
	const bool attached = api_device_is_attached(ubi);

	if (!attached || NULL == config || NULL == config->name ||
	    0 == config->leb_count || NULL == vol_id) {
		LOG_ERR("volume create: needs a handle, a name and a size");
		return -EINVAL;
	}

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_state_guard(ubi);

	if (0 != ret)
		return api_unlock(ubi, ret);

	ret = ubi_impl_volume_create(ubi, config, vol_id);

	return api_unlock(ubi, ret);
}

int ubi_volume_resize(struct ubi_device *ubi, uint32_t vol_id,
		      uint32_t leb_count)
{
	const bool attached = api_device_is_attached(ubi);
	const bool reachable = api_vol_id_is_public(vol_id);

	if (!attached || !reachable || 0 == leb_count) {
		LOG_ERR("volume resize: needs a handle, a volume and a size");
		return -EINVAL;
	}

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_state_guard(ubi);

	if (0 != ret)
		return api_unlock(ubi, ret);

	ret = ubi_impl_volume_resize(ubi, vol_id, leb_count);

	return api_unlock(ubi, ret);
}

int ubi_volume_remove(struct ubi_device *ubi, uint32_t vol_id)
{
	const bool attached = api_device_is_attached(ubi);
	const bool reachable = api_vol_id_is_public(vol_id);

	if (!attached || !reachable) {
		LOG_ERR("volume remove: needs a handle and a volume");
		return -EINVAL;
	}

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_state_guard(ubi);

	if (0 != ret)
		return api_unlock(ubi, ret);

	ret = ubi_impl_volume_remove(ubi, vol_id);

	return api_unlock(ubi, ret);
}

int ubi_volume_find(struct ubi_device *ubi, const char *name, uint32_t *vol_id)
{
	const bool attached = api_device_is_attached(ubi);

	if (!attached || NULL == name || NULL == vol_id) {
		LOG_ERR("volume find: needs a handle, a name and an answer");
		return -EINVAL;
	}

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_volume_find(ubi, name, vol_id);

	return api_unlock(ubi, ret);
}

int ubi_volume_get_info(struct ubi_device *ubi, uint32_t vol_id,
			struct ubi_volume_info *info)
{
	const bool attached = api_device_is_attached(ubi);
	const bool reachable = api_vol_id_is_public(vol_id);

	if (!attached || !reachable || NULL == info) {
		LOG_ERR("volume info: needs a handle, a volume and an answer");
		return -EINVAL;
	}

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_volume_get_info(ubi, vol_id, info);

	return api_unlock(ubi, ret);
}

int ubi_leb_map(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum)
{
	const bool attached = api_device_is_attached(ubi);
	const bool reachable = api_vol_id_is_public(vol_id);

	if (!attached || !reachable) {
		LOG_ERR("leb map: needs a handle and a volume");
		return -EINVAL;
	}

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_state_guard(ubi);

	if (0 != ret)
		return api_unlock(ubi, ret);

	ret = ubi_impl_leb_map(ubi, vol_id, lnum);

	return api_unlock(ubi, ret);
}

int ubi_leb_unmap(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum)
{
	const bool attached = api_device_is_attached(ubi);
	const bool reachable = api_vol_id_is_public(vol_id);

	if (!attached || !reachable) {
		LOG_ERR("leb unmap: needs a handle and a volume");
		return -EINVAL;
	}

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_state_guard(ubi);

	if (0 != ret)
		return api_unlock(ubi, ret);

	ret = ubi_impl_leb_unmap(ubi, vol_id, lnum);

	return api_unlock(ubi, ret);
}

int ubi_leb_erase(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum)
{
	const bool attached = api_device_is_attached(ubi);
	const bool reachable = api_vol_id_is_public(vol_id);

	if (!attached || !reachable) {
		LOG_ERR("leb erase: needs a handle and a volume");
		return -EINVAL;
	}

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_state_guard(ubi);

	if (0 != ret)
		return api_unlock(ubi, ret);

	ret = ubi_impl_leb_erase(ubi, vol_id, lnum);

	return api_unlock(ubi, ret);
}

int ubi_leb_read(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		 uint32_t offset, void *buffer, size_t length)
{
	const bool attached = api_device_is_attached(ubi);
	const bool reachable = api_vol_id_is_public(vol_id);

	if (!attached || !reachable || (NULL == buffer && 0 != length)) {
		LOG_ERR("leb read: needs a handle, a volume and a destination");
		return -EINVAL;
	}

	if (0 == length)
		return 0;

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	/* Served even without trust: the application has to be able to look
	 * at what it decided to distrust. */
	ret = ubi_impl_leb_read(ubi, vol_id, lnum, offset, buffer, length);

	return api_unlock(ubi, ret);
}

int ubi_leb_change(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		   const void *buffer, size_t length)
{
	const bool attached = api_device_is_attached(ubi);
	const bool reachable = api_vol_id_is_public(vol_id);

	if (!attached || !reachable || (NULL == buffer && 0 != length)) {
		LOG_ERR("leb change: needs a handle, a volume and the data");
		return -EINVAL;
	}

	if (0 == length)
		return 0;

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_state_guard(ubi);

	if (0 != ret)
		return api_unlock(ubi, ret);

	ret = ubi_impl_leb_change(ubi, vol_id, lnum, buffer, length);

	return api_unlock(ubi, ret);
}

int ubi_leb_write_at(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     uint32_t offset, const void *buffer, size_t length)
{
	const bool attached = api_device_is_attached(ubi);
	const bool reachable = api_vol_id_is_public(vol_id);

	if (!attached || !reachable || (NULL == buffer && 0 != length)) {
		LOG_ERR("leb write: needs a handle, a volume and the data");
		return -EINVAL;
	}

	if (0 == length)
		return 0;

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_state_guard(ubi);

	if (0 != ret)
		return api_unlock(ubi, ret);

	ret = ubi_impl_leb_write_at(ubi, vol_id, lnum, offset, buffer, length);

	return api_unlock(ubi, ret);
}

int ubi_leb_get_info(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     struct ubi_leb_info *info)
{
	const bool attached = api_device_is_attached(ubi);
	const bool reachable = api_vol_id_is_public(vol_id);

	if (!attached || !reachable || NULL == info) {
		LOG_ERR("leb info: needs a handle, a volume and an answer");
		return -EINVAL;
	}

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_leb_get_info(ubi, vol_id, lnum, info);

	return api_unlock(ubi, ret);
}

int ubi_maintenance(struct ubi_device *ubi, enum ubi_maintenance_op operation,
		    uint32_t budget, struct ubi_maintenance_result *result)
{
	const bool attached = api_device_is_attached(ubi);
	const bool known = api_maintenance_op_is_known(operation);

	if (!attached || !known || NULL == result) {
		LOG_ERR("maintenance: needs a handle, an operation and "
			"somewhere to report");
		return -EINVAL;
	}

	int ret = api_lock(ubi);

	if (0 != ret)
		return ret;

	ret = ubi_impl_state_guard(ubi);

	/* A refusal still says what is waiting. */
	if (0 != ret) {
		ubi_impl_maintenance_report(ubi, operation, result);
		return api_unlock(ubi, ret);
	}

	ret = ubi_impl_maintenance(ubi, operation, budget, result);

	return api_unlock(ubi, ret);
}
