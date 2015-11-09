/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <stdio.h>

#include "era.h"
#include "era_dm.h"

#include <libdevmapper.h>

void era_dm_init(void)
{
	dm_lib_init();
	dm_set_uuid_prefix(UUID_PREFIX);
}

void era_dm_exit(void)
{
	dm_lib_release();
	dm_lib_exit();
}

static int _era_dm_table(int task, int wait,
                         const char *name, const char *uuid,
                         uint64_t start, uint64_t size,
                         const char *target, const char *table,
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

	if (target != NULL &&
	    !dm_task_add_target(dmt, start, size, target, table))
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

		info->target_count = dmi.target_count;
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

int era_dm_create_empty(const char *name, const char *uuid,
                        struct era_dm_info *info)
{
	return _era_dm_table(DM_DEVICE_CREATE, 0, name, uuid,
	                     0, 0, NULL, NULL, info);
}

int era_dm_create(const char *name, const char *uuid,
                  uint64_t start, uint64_t size,
                  const char *target, const char *table,
                  struct era_dm_info *info)
{
	return _era_dm_table(DM_DEVICE_CREATE, 1, name, uuid,
	                     start, size, target, table, info);
}

int era_dm_load(const char *name,
                uint64_t start, uint64_t size,
                const char *target, const char *table,
                struct era_dm_info *info)
{
	return _era_dm_table(DM_DEVICE_RELOAD, 0, name, NULL,
	                     start, size, target, table, info);
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

int era_dm_list(void)
{
	struct dm_task *dmt;
	struct dm_names *names;
	unsigned next = 0;

	if (!(dmt = dm_task_create(DM_DEVICE_LIST)))
		return -1;

	if (!dm_task_run(dmt))
		goto out;

	if (!(names = dm_task_get_names(dmt)))
		goto out;

	if (names->dev)
	{
		do {
			names = (struct dm_names *)((char *)names + next);
			printf("%s\n", names->name);
			next = names->next;
		} while(next);
	}

	dm_task_destroy(dmt);
	return 0;
out:
	dm_task_destroy(dmt);
	return -1;
}
