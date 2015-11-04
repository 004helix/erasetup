/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <stdio.h>

#include "era.h"
#include "era_dm.h"

#include <libdevmapper.h>

static int _era_dm_table(int task, int wait,
                         const char *name, const char *uuid,
                         uint64_t size, const char *target, const char *table,
                         struct era_dm_info *info)
{
	struct dm_task *dmt;
	uint32_t cookie = 0;
	int rc;

	if (!(dmt = dm_task_create(task)))
		return -1;

	if (!dm_task_set_name(dmt, name))
		goto out;

	if (uuid && !dm_task_set_uuid(dmt, uuid))
		goto out;

	if (!dm_task_add_target(dmt, 0, size, target, table))
		goto out;

	if (wait && !dm_task_set_cookie(dmt, &cookie, 0))
		goto out;

	rc = dm_task_run(dmt);

	if (wait)
		(void) dm_udev_wait(cookie);

	if (rc && info)
	{
		struct dm_info dmi;

		if (!dm_task_get_info(dmt, &dmi))
			goto out;

		info->open_count = dmi.open_count;
		info->major = dmi.major;
		info->minor = dmi.minor;
	}

	dm_task_destroy(dmt);

	return rc ? 0 : -1;
out:
	dm_task_destroy(dmt);
	return -1;
}

static int _era_dm_simple(int task, int wait, const char *name)
{
	struct dm_task *dmt;
	uint32_t cookie = 0;
	int rc;

	if (!(dmt = dm_task_create(task)))
		return -1;

	if (!dm_task_set_name(dmt, name))
		goto out;

	if (wait && !dm_task_set_cookie(dmt, &cookie, 0))
		goto out;

	rc = dm_task_run(dmt);

	if (wait)
		(void) dm_udev_wait(cookie);

	dm_task_destroy(dmt);
	return rc ? 0 : -1;
out:
	dm_task_destroy(dmt);
	return -1;
}

void era_dm_init(void)
{
	dm_set_uuid_prefix(UUID_PREFIX);
}

int era_dm_create(const char *name, const char *uuid,
                  uint64_t size, const char *target, const char *table,
                  struct era_dm_info *info)
{
	return _era_dm_table(DM_DEVICE_CREATE, 1,
	                     name, uuid, size, target, table, info);
}

int era_dm_load(const char *name,
                uint64_t size, const char *target, const char *table,
                struct era_dm_info *info)
{
	return _era_dm_table(DM_DEVICE_RELOAD, 0,
	                     name, NULL, size, target, table, info);
}

int era_dm_suspend(const char *name)
{
	return _era_dm_simple(DM_DEVICE_SUSPEND, 0, name);
}

int era_dm_resume(const char *name)
{
	return _era_dm_simple(DM_DEVICE_RESUME, 1, name);
}

int era_dm_remove(const char *name)
{
	return _era_dm_simple(DM_DEVICE_REMOVE, 1, name);
}

int era_dm_clear(const char *name)
{
	return _era_dm_simple(DM_DEVICE_CLEAR, 0, name);
}
