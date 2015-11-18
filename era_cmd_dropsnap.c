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
	if (dev == NULL)
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

	dev->next = *devs;
	*devs = dev;

	return 0;
}

int era_dropsnap(int argc, char **argv)
{
	char *path;
	struct md *sn;
	struct devices *devs, *curr;
	struct devices *orig, *snap, *cow;
	struct era_snapshot_superblock *ssb;
	unsigned real_major, real_minor;
	char dmuuid[DM_UUID_LEN];
	char uuid[UUID_LEN];
	unsigned era;
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

	path = argv[0];

	/*
	 * open snapshot device
	 */

	sn = md_open(path, 0);
	if (sn == NULL)
		return -1;

	/*
	 * read snapshot superblock
	 */

	ssb = md_block(sn, 0, 0, SNAP_SUPERBLOCK_CSUM_XOR);
	if (ssb == NULL)
	{
		md_close(sn);
		return -1;
	}

	memcpy(uuid, ssb->uuid, UUID_LEN);
	era = le32toh(ssb->snapshot_era);

	md_close(sn);

	/*
	 * list devices
	 */

	devs = NULL;

	if (era_dm_list(devices_cb, &devs))
		return -1;

	if (devs == NULL)
	{
		error(0, "no devices found");
		goto out;
	}

	/*
	 * search snapshot device
	 */

	snap = NULL;

	snprintf(dmuuid, sizeof(dmuuid), "ERA-SNAP-%s", uuid2str(uuid));

	for (curr = devs; curr != NULL; curr = curr->next)
	{
		if (!strcmp(curr->uuid, dmuuid))
		{
			snap = curr;
			break;
		}
	}

	if (snap == NULL)
	{
		error(0, "can't find device era-snap-%s", uuid2str(uuid));
		goto out;
	}

	if (strcmp(snap->target, TARGET_SNAPSHOT))
	{
		error(0, "unsupported target type: %s", snap->target);
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
	 * count snapshots
	 */

	for (curr = devs; curr != NULL; curr = curr->next)
	{
		unsigned maj, min;

		if (strcmp(curr->target, TARGET_SNAPSHOT))
			continue;

		if (sscanf(curr->table, "%u:%u", &maj, &min) != 2)
			continue;

		if (maj == real_major && min == real_minor)
			cnt++;
	}

	printf("%d\n", cnt);

	rc = 0;
out:
	curr = devs;
	while (curr != NULL)
	{
		void *p = curr;
		curr = curr->next;
		free(p);
	}

	return rc;
}
