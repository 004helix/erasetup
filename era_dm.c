/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include "era.h"
#include "era_dm.h"

#include <libdevmapper.h>

void era_dm_init(void)
{
	dm_set_uuid_prefix(UUID_PREFIX);
}

int era_dm_create(const char *name, const char *uuid,
                  uint64_t size, const char *target, const char *table)
{
	struct dm_task *dmt;
	uint32_t cookie = 0;
	int rc;

	if (!(dmt = dm_task_create(DM_DEVICE_CREATE)))
		return -1;

	if (!dm_task_set_name(dmt, name))
		goto out;

	if (!dm_task_set_uuid(dmt, uuid))
		goto out;

	if (!dm_task_add_target(dmt, 0, size, TARGET_ERA, table))
		goto out;

	if (!dm_task_set_cookie(dmt, &cookie, 0))
		goto out;

	rc = dm_task_run(dmt);

	(void)dm_udev_wait(cookie);

	dm_task_destroy(dmt);

	return rc ? 0 : -1;
out:
	dm_task_destroy(dmt);

	return -1;
}
