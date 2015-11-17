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

static char *hsize(uint64_t s)
{
	static char buffer[32];
	static const char *units[] = {
		"KiB",
		"MiB",
		"GiB",
		"TiB",
		"PiB",
		"EiB",
		NULL
	};

	int i;
	uint64_t k = s / 1024;
	double d = (double) k;

	for (i = 0; (d > 1024.0) && units[i]; i++)
		d /= 1024.0;

	snprintf(buffer, sizeof(buffer), "%.02f %s", d, units[i]);

	return buffer;
}

static char *percent(uint64_t val, uint64_t total)
{
	static char buffer[32];

	if (total == 0)
	{
		strcpy(buffer, "?%");
		return buffer;
	}

	if (val >= total)
	{
		strcpy(buffer, "100%");
		return buffer;
	}

	double p = (double) val;
	p /= (double) total;
	p *= 100;

	if (p < 10.0)
		sprintf(buffer, "%.02f%%", p);
	else
		sprintf(buffer, "%.01f%%", p);

	return buffer;
}

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

	memset(dev, 0, sizeof(*dev));
	strcpy(dev->name, name);
	dev->next = *devs;
	*devs = dev;

	return 0;
}

static int get_snapshot_era(struct devices *devs, const char *uuid,
                            unsigned *era)
{
	struct md *sn;
	struct devices *curr, *cow;
	struct era_snapshot_superblock *ssb;
	char cow_dmuuid[DM_UUID_LEN];
	unsigned long long offset;
	unsigned major, minor;
	uint64_t sectors;
	int fd;

	snprintf(cow_dmuuid, sizeof(cow_dmuuid), "ERA-SNAP-%s-cow", uuid);

	cow = NULL;

	for (curr = devs; curr->next != NULL; curr = curr->next)
	{
		if (!strcmp(curr->uuid, cow_dmuuid))
		{
			cow = curr;
			break;
		}
	}

	if (cow == NULL)
	{
		error(0, "can't find cow-device for uuid %s", uuid);
		return -1;
	}

	if (strcmp(cow->target, TARGET_LINEAR))
	{
		error(0, "unexpected cow target type: %s", cow->target);
		return -1;
	}

	if (sscanf(cow->table, "%u:%u %llu", &major, &minor, &offset) != 3)
	{
		error(0, "can't parse cow table: %s", cow->table);
		return -1;
	}

	fd = blkopen2(major, minor, 0, &sectors);
	if (fd == -1)
		return -1;

	sn = md_open(NULL, fd);
	if (sn == NULL)
		return -1;

	ssb = md_block(sn, 0, 0, SNAP_SUPERBLOCK_CSUM_XOR);
	if (ssb == NULL)
	{
		md_close(sn);
		return -1;
	}

	if (strcmp(uuid, uuid2str(ssb->uuid)))
	{
		error(0, "wrong superblock uuid: expected %s, "
		      "but got %s", uuid, uuid2str(ssb->uuid));
		md_close(sn);
		return -1;
	}

	*era = le32toh(ssb->snapshot_era);

	md_close(sn);

	return 0;
}

int era_status(int argc, char **argv)
{
	size_t prefix_len = strlen(UUID_PREFIX);
	struct devices *devs, *curr;
	char *device;
	int found = 0;
	int rc = -1;

	switch (argc)
	{
	case 0:
		device = NULL;
		break;
	case 1:
		device = argv[0];
		break;
	default:
		error(0, "unknown argument: %s", argv[1]);
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
		unsigned meta_chunk, chunk, era;
		unsigned long long meta_used;
		unsigned long long meta_total;
		char orig_dmuuid[DM_UUID_LEN];
		struct devices *c, *orig;
		unsigned real_major;
		unsigned real_minor;
		unsigned maj1, min1;
		unsigned maj2, min2;

		if (strcmp(curr->target, TARGET_ERA))
			continue;

		if (device != NULL && strcmp(curr->name, device))
			continue;

		if (sscanf(curr->status, "%u %llu/%llu %u %s", &meta_chunk,
		           &meta_used, &meta_total, &era, meta_snap) != 5)
		{
			error(0, "unsupported era device: %s\n", curr->name);
			continue;
		}

		if (sscanf(curr->table, "%u:%u %u:%u %u", &maj1, &min1,
		           &maj2, &min2, &chunk) != 5)
		{
			error(0, "unsupported era device: %s\n", curr->name);
			continue;
		}

		found++;

		printf("name:          %s\n", curr->name);
		printf("current era:   %u\n", era);
		printf("device size:   %s\n",
		       hsize(curr->sectors << SECTOR_SHIFT));
		printf("chunk size:    %s\n",
		       hsize(chunk << SECTOR_SHIFT));
		printf("metadata size: %s\n",
		       hsize(meta_total * MD_BLOCK_SIZE));
		printf("metadata used: %s (%s)\n",
		       hsize(meta_used * MD_BLOCK_SIZE),
		       percent(meta_used, meta_total));
		printf("uuid:          %s\n", curr->uuid);

		printf("\n");

		strcpy(orig_dmuuid, curr->uuid);
		strcat(orig_dmuuid, "-orig");
		orig = NULL;

		for (c = devs; c->next != NULL; c = c->next)
		{
			if (!strcmp(c->uuid, orig_dmuuid))
			{
				orig = c;
				break;
			}
		}

		if (orig == NULL || strcmp(orig->target, TARGET_ORIGIN))
			continue;

		if (sscanf(orig->table, "%u:%u",
		           &real_major, &real_minor) != 2)
			continue;

		for (c = devs; c->next != NULL; c = c->next)
		{
			unsigned snap_chunk, era;
			unsigned long long used;
			unsigned long long total;
			unsigned long long meta;
			char persistent[4];
			char *uuid;

			if (strcmp(c->target, TARGET_SNAPSHOT))
				continue;

			if (strncmp("era-snap-", c->name, 9))
				continue;

			uuid = c->name + 9;

			if (sscanf(c->table, "%u:%u %u:%u %s %u",
			           &maj1, &min1, &maj2, &min2,
			           persistent, &snap_chunk) != 6)
				continue;

			if (real_major != maj1 || real_minor != min1)
				continue;

			printf("  snapshot:    %s\n", uuid);

			if (sscanf(c->status, "%llu/%llu %llu",
			           &used, &total, &meta) != 3)
			{
				printf("  status:      %s\n\n", c->status);
				continue;
			}
			else
				printf("  status:      Active\n");

			printf("  size:        %s\n",
			       hsize(total << SECTOR_SHIFT));
			printf("  used:        %s (%s)\n",
			       hsize(used << SECTOR_SHIFT),
			       percent(used, total));

			if (get_snapshot_era(devs, uuid, &era) == 0)
				printf("  era:         %u\n", era);

			printf("\n");
		}
	}

	if (device != NULL && !found)
	{
		error(0, "device not found: %s", device);
		goto out;
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
