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
}

void era_dm_exit(void)
{
	dm_lib_release();
	dm_lib_exit();
}

static int _dm_create(int task, int wait,
                      const char *name, const char *uuid,
                      uint64_t start, uint64_t length,
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
	    !dm_task_add_target(dmt, start, length, target, table))
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
		info->exists = dmi.exists;
		info->major = dmi.major;
		info->minor = dmi.minor;
	}

	dm_task_destroy(dmt);

	return rc ? 0 : -1;
out:
	dm_task_destroy(dmt);
	return -1;
}

static int _dm_simple(int task, int wait, const char *name)
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
	return _dm_create(DM_DEVICE_CREATE, 0, name, uuid,
	                  0, 0, NULL, NULL, info);
}

int era_dm_create(const char *name, const char *uuid,
                  uint64_t start, uint64_t length,
                  const char *target, const char *table,
                  struct era_dm_info *info)
{
	return _dm_create(DM_DEVICE_CREATE, 1, name, uuid,
	                  start, length, target, table, info);
}

int era_dm_load(const char *name,
                uint64_t start, uint64_t length,
                const char *target, const char *table,
                struct era_dm_info *info)
{
	return _dm_create(DM_DEVICE_RELOAD, 0, name, NULL,
	                  start, length, target, table, info);
}

int era_dm_suspend(const char *name)
{
	return _dm_simple(DM_DEVICE_SUSPEND, 0, name);
}

int era_dm_resume(const char *name)
{
	return _dm_simple(DM_DEVICE_RESUME, 1, name);
}

int era_dm_remove(const char *name)
{
	return _dm_simple(DM_DEVICE_REMOVE, 1, name);
}

int era_dm_clear(const char *name)
{
	return _dm_simple(DM_DEVICE_CLEAR, 0, name);
}

int era_dm_info(const char *name,
                const char *uuid,
                struct era_dm_info *info,
                size_t name_size, char *name_ptr,
                size_t uuid_size, char *uuid_ptr)
{
	struct dm_task *dmt;
	struct dm_info dmi;
	int rc = -1;

	if (!(dmt = dm_task_create(DM_DEVICE_INFO)))
		return -1;

	if (name && !dm_task_set_name(dmt, name))
		goto out;

	if (uuid && !dm_task_set_uuid(dmt, uuid))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	if (!dm_task_get_info(dmt, &dmi))
		goto out;

	if (info)
	{
		info->target_count = dmi.target_count;
		info->open_count = dmi.open_count;
		info->exists = dmi.exists;
		info->major = dmi.major;
		info->minor = dmi.minor;
	}

	if (dmi.exists && name_size > 0 && name_ptr)
	{
		const char *dm_name;
		size_t dm_name_len;

		dm_name = dm_task_get_name(dmt);
		if (dm_name == NULL)
			goto out;

		dm_name_len = strlen(dm_name);
		if (dm_name_len >= name_size)
		{
			rc = dm_name_len + 1;
			goto out;
		}

		strcpy(name_ptr, dm_name);
	}

	if (dmi.exists && uuid_size > 0 && uuid_ptr)
	{
		const char *dm_uuid;
		size_t dm_uuid_len;

		dm_uuid = dm_task_get_uuid(dmt);
		if (dm_uuid == NULL)
			goto out;

		dm_uuid_len = strlen(dm_uuid);
		if (dm_uuid_len >= uuid_size)
		{
			dm_task_destroy(dmt);
			return dm_uuid_len + 1;
		}

		strcpy(uuid_ptr, dm_uuid);
	}

	rc = 0;
out:
	dm_task_destroy(dmt);
	return rc;
}

static int _first_status(int task,
                         const char *name,
                         const char *uuid,
                         uint64_t *start, uint64_t *length,
                         size_t target_size, char *target_ptr,
                         size_t params_size, char *params_ptr)
{
	struct dm_task *dmt;
	struct dm_info dmi;
	char *tgt, *prm;
	int rc = -1;

	if (!(dmt = dm_task_create(task)))
		return -1;

	if (name && !dm_task_set_name(dmt, name))
		goto out;

	if (uuid && !dm_task_set_uuid(dmt, uuid))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	if (!dm_task_get_info(dmt, &dmi))
		goto out;

	if (!dmi.exists)
	{
		error(0, "target %s does not exists",
		      name ? name : (uuid ? uuid : "<NULL>"));
		goto out;
	}

	(void)dm_get_next_target(dmt, NULL, start, length, &tgt, &prm);

	if (target_size > 0 && target_ptr)
	{
		if (strlen(tgt) >= target_size)
			goto out;

		strcpy(target_ptr, tgt);
	}

	if (params_size > 0 && params_ptr)
	{
		if (strlen(prm) >= params_size)
			goto out;

		strcpy(params_ptr, prm);
	}

	rc = 0;
out:
	dm_task_destroy(dmt);
	return rc;
}

int era_dm_first_table(const char *name,
                       const char *uuid,
                       uint64_t *start, uint64_t *length,
                       size_t target_size, char *target_ptr,
                       size_t params_size, char *params_ptr)
{
	return _first_status(DM_DEVICE_TABLE, name, uuid, start, length,
	                     target_size, target_ptr, params_size, params_ptr);
}

int era_dm_first_status(const char *name,
                        const char *uuid,
                        uint64_t *start, uint64_t *length,
                        size_t target_size, char *target_ptr,
                        size_t params_size, char *params_ptr)
{
	return _first_status(DM_DEVICE_STATUS, name, uuid, start, length,
	                     target_size, target_ptr, params_size, params_ptr);
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
