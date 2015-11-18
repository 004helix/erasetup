/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <string.h>
#include <endian.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "era.h"
#include "era_dm.h"
#include "era_md.h"
#include "era_snapshot.h"
#include "era_cmd_dropsnap.h"

struct devices {
	uint64_t size;
	char name[DM_NAME_LEN];
	char uuid[DM_UUID_LEN];
	char table[256];
	char target[DM_MAX_TYPE_NAME];
	struct era_dm_info info;
	struct devices *next;
};

static int devices_cb(void *arg, const char *name)
{
	uint64_t start, length;
	struct devices **devs = arg;
	struct devices *dev;

	if (strlen(name) >= sizeof(dev->name))
		return 0;

	dev = malloc(sizeof(*dev));
	if (!dev)
	{
		error(ENOMEM, NULL);
		return -1;
	}

	memset(dev, 0, sizeof(*dev));
	strcpy(dev->name, name);

	if (era_dm_info(dev->name, NULL, &dev->info, 0, NULL,
	                sizeof(dev->uuid), dev->uuid))
	{
		free(dev);
		return 0;
	}

	if (!dev->info.exists ||
	    dev->info.target_count != 1 ||
	    strncmp(dev->uuid, UUID_PREFIX, strlen(UUID_PREFIX)))
	{
		free(dev);
		return 0;
	}

	if (era_dm_first_table(name, NULL, &start, &length,
	                       sizeof(dev->target), dev->target,
	                       sizeof(dev->table), dev->table))
	{
		free(dev);
		return -1;
	}

	dev->size = length;
	dev->next = *devs;
	*devs = dev;

	return 0;
}

int era_dropsnap(int argc, char **argv)
{
	struct md *sn;
	struct devices *devs, *curr;
	struct devices *orig, *snap, *cow;
	struct era_snapshot_superblock *ssb;
	unsigned real_major, real_minor;
	char dmuuid[DM_UUID_LEN];
	char uuid[UUID_LEN];
	int cnt = 0;
	int rc = -1;

	switch (argc)
	{
	case 0:
		error(0, "snapshot device argument expected");
		usage(stderr, 1);
	case 1:
		break;
	default:
		error(0, "unknown argument: %s", argv[1]);
		usage(stderr, 1);
	}

	/*
	 * open snapshot device
	 */

	sn = md_open(argv[0], 0);
	if (!sn)
		return -1;

	/*
	 * read snapshot superblock
	 */

	ssb = md_block(sn, 0, 0, SNAP_SUPERBLOCK_CSUM_XOR);
	if (!ssb)
	{
		md_close(sn);
		return -1;
	}

	memcpy(uuid, ssb->uuid, UUID_LEN);

	md_close(sn);

	/*
	 * list devices
	 */

	devs = NULL;

	if (era_dm_list(devices_cb, &devs))
		goto out;

	if (!devs)
	{
		error(0, "no devices found");
		return -1;
	}

	/*
	 * search snapshot
	 */

	snap = NULL;

	snprintf(dmuuid, sizeof(dmuuid), "ERA-SNAP-%s", uuid2str(uuid));

	for (curr = devs; curr; curr = curr->next)
	{
		if (strcmp(curr->target, TARGET_SNAPSHOT))
			continue;

		if (!strcmp(curr->uuid, dmuuid))
		{
			snap = curr;
			break;
		}
	}

	if (!snap)
	{
		error(0, "can't find era-snap-%s", uuid2str(uuid));
		goto out;
	}

	if (snap->info.open_count > 0)
	{
		error(0, "snapshot is in use");
		goto out;
	}

	/*
	 * parse snapshot table
	 */

	if (sscanf(snap->table, "%u:%u", &real_major, &real_minor) != 2)
	{
		error(0, "can't parse snapshot table: %s", snap->table);
		goto out;
	}

	/*
	 * search cow
	 */

	cow = NULL;

	snprintf(dmuuid, sizeof(dmuuid), "ERA-SNAP-%s-cow", uuid2str(uuid));

	for (curr = devs; curr; curr = curr->next)
	{
		if (strcmp(curr->target, TARGET_LINEAR))
			continue;

		if (!strcmp(curr->uuid, dmuuid))
		{
			cow = curr;
			break;
		}
	}

	if (!cow)
	{
		error(0, "can't find era-snap-%s-cow", uuid2str(uuid));
		goto out;
	}

	/*
	 * search origin
	 */

	orig = NULL;

	for (curr = devs; curr; curr = curr->next)
	{
		unsigned maj, min;

		if (strcmp(curr->target, TARGET_ORIGIN))
			continue;

		if (sscanf(curr->table, "%u:%u", &maj, &min) != 2)
			continue;

		if (maj == real_major && min == real_minor)
		{
			orig = curr;
			break;
		}
	}

	if (!orig)
	{
		error(0, "can't find origin device");
		goto out;
	}

	/*
	 * count snapshots
	 */

	for (curr = devs; curr; curr = curr->next)
	{
		unsigned maj, min;

		if (strcmp(curr->target, TARGET_SNAPSHOT))
			continue;

		if (sscanf(curr->table, "%u:%u", &maj, &min) != 2)
			continue;

		if (maj == real_major && min == real_minor)
			cnt++;
	}

	/*
	 * suspend origin
	 */

	if (era_dm_suspend(orig->name))
		goto out;

	/*
	 * remove snapshot
	 */

	if (era_dm_remove(snap->name))
	{
		era_dm_resume(orig->name);
		goto out;
	}

	/*
	 * replace snapshot-origin with linear
	 */

	if (cnt == 1)
	{
		char table[64];

		sprintf(table, "%u:%u 0", real_major, real_minor);

		if (era_dm_load(orig->name, 0, orig->size,
		                TARGET_LINEAR, table, NULL))
		{
			era_dm_resume(orig->name);
			goto out;
		}
	}

	/*
	 * resume origin
	 */

	if (era_dm_resume(orig->name))
		goto out;

	/*
	 * remove cow
	 */

	if (era_dm_remove(cow->name))
		goto out;

	rc = 0;
out:
	curr = devs;
	while (curr)
	{
		void *p = curr;
		curr = curr->next;
		free(p);
	}

	return rc;
}
