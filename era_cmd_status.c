/*
 * This file is released under the GPL.
  */

#define _GNU_SOURCE

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#include "era.h"
#include "era_dm.h"
#include "era_md.h"
#include "era_blk.h"
#include "era_snapshot.h"

struct devices {
	char target[DM_MAX_TYPE_NAME];
	char name[DM_NAME_LEN];
	char uuid[DM_UUID_LEN];
	char status[128];
	char table[256];
	uint64_t sectors;
	struct devices *next;
};

static int devlist_cb(void *arg, const char *name)
{
	struct devices **devs = arg;
	struct devices *dev;

	/* too long */
	if (strlen(name) >= sizeof(dev->name))
		return 0;

	dev = malloc(sizeof(*dev));
	if (dev == NULL)
	{
		error(ENOMEM, NULL);
		return -1;
	}

	strcpy(dev->name, name);
	dev->uuid[0] = '\0';
	dev->target[0] = '\0';
	dev->status[0] = '\0';
	dev->table[0] = '\0';
	dev->next = *devs;
	*devs = dev;

	return 0;
}

int era_status(int argc, char **argv)
{
	size_t prefix_len = strlen(UUID_PREFIX);
	struct devices *devs, *curr;
	int rc = -1;

	if (argc != 0)
	{
		error(0, "unknown argument: %s", argv[0]);
		usage(stderr, 1);
	}

	devs = NULL;

	if (era_dm_list(devlist_cb, &devs))
		return -1;

	if (devs == NULL)
	{
		printv(1, "no devices found\n");
		return 0;
	}

	for (curr = devs; curr->next != NULL; curr = curr->next)
	{
		struct era_dm_info info;
		uint64_t start, length;

		if (era_dm_info(curr->name, NULL, &info, 0, NULL,
		                sizeof(curr->uuid), curr->uuid))
			goto out;

		if (!info.exists || info.suspended || info.target_count != 1)
			continue;

		if (strncmp(curr->uuid, UUID_PREFIX, prefix_len))
			continue;

		if (era_dm_first_table(curr->name, NULL, &start, &length,
		                       sizeof(curr->target), curr->target,
		                       sizeof(curr->table), curr->table))
			goto out;

		if (strcmp(curr->target, TARGET_ERA) &&
		    strcmp(curr->target, TARGET_SNAPSHOT))
			continue;

		if (era_dm_first_status(curr->name, NULL, &start,
		                        &curr->sectors, 0, NULL,
		                        sizeof(curr->status), curr->status))
			goto out;
	}

	for (curr = devs; curr->next != NULL; curr = curr->next)
	{
		char meta_snap[16];
		unsigned meta_chunk, era;
		unsigned long long meta_used;
		unsigned long long meta_total;

		if (strcmp(curr->target, TARGET_ERA))
			continue;

		if (sscanf(curr->status, "%u %llu/%llu %u %s", &meta_chunk,
		           &meta_used, &meta_total, &era, meta_snap) != 5)
		{
			error(0, "unsupported era device: %s\n", curr->name);
			continue;
		}

		printf("--- device -----------------------------------------"
		       "-------------\n");

		printv(0, "name:          %s\n", curr->name);
		printv(0, "uuid:          %s\n", curr->uuid);
		printv(0, "current era:   %u\n", era);
		printv(0, "device size:   %llu\n",
		       (long long unsigned)(curr->sectors << SECTOR_SHIFT));
	}

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
