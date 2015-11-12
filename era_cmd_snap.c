/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "era.h"
#include "era_dm.h"
#include "era_md.h"
#include "era_blk.h"
#include "era_snapshot.h"
#include "era_cmd_snap.h"

int era_takesnap(int argc, char **argv)
{
	char *name, *snap, *cow;
	struct era_dm_info info;
	struct md *md, *sn;
	uint64_t start, length;
	uint64_t nr_blocks, era_blocks;
	uint64_t snap_offset;
	unsigned replace_with_linear;
	unsigned long long meta_snap;
	unsigned long long meta_used;
	unsigned long long meta_total;
	unsigned meta_major, meta_minor;
	unsigned data_major, data_minor;
	unsigned real_major, real_minor;
	unsigned cow_major, cow_minor;
	unsigned chunk, meta_chunk, era;
	size_t status_len;
	char snapuuid[256];
	char cowname[256];
	char cowuuid[256];
	char status[128];
	char target[128];
	char table[128];
	char uuid[256];
	char orig[256];
	int fd;

	switch (argc)
	{
	case 0:
		error(0, "device name argument expected");
		usage(stderr, 1);
	case 1:
		error(0, "snapshot name argument expected");
		usage(stderr, 1);
	case 2:
		error(0, "cow device argument expected");
		usage(stderr, 1);
	case 3:
		break;
	default:
		error(0, "unknown argument: %s", argv[3]);
		usage(stderr, 1);
	}

	name = argv[0];
	snap = argv[1];
	cow = argv[2];

	sn = NULL;
	md = NULL;

	replace_with_linear = 0;

	/*
	 * open metadata device
	 */

	if (era_dm_info(name, NULL, &info,
	                0, NULL, sizeof(uuid) - 16, uuid))
		goto out;

	if (!info.exists)
	{
		error(0, "device %s does not exists", name);
		goto out;
	}

	if (info.target_count != 1)
	{
		error(0, "invalid device %s", name);
		goto out;
	}

	if (era_dm_first_table(NULL, uuid, &start, &length,
	                       sizeof(target), target,
	                       sizeof(table), table))
		goto out;

	if (strcmp(target, TARGET_ERA))
	{
		error(0, "unsupported target type \"%s\"", target);
		goto out;
	}

	if (sscanf(table, "%u:%u %u:%u %u",
	           &meta_major, &meta_minor,
	           &data_major, &data_minor,
	           &chunk) != 5)
	{
		error(0, "can't parse device table: \"%s\"", table);
		goto out;
	}

	fd = blkopen2(meta_major, meta_minor, 1, NULL);
	if (fd == -1)
		goto out;

	md = md_open(NULL, fd);
	if (md == NULL)
		goto out;

	/*
	 * open cow device
	 */

	sn = md_open(cow, 1);
	if (sn == NULL)
		goto out;

	/*
	 * check era device status
	 */

	if (era_dm_first_status(NULL, uuid, &start, &length,
	                        sizeof(target), target,
	                        sizeof(status), status))
		goto out;

	status_len = strlen(status);

	if (status_len == 0)
	{
		error(0, "empty device status: \"%s\"", name);
		goto out;
	}

	if (status[status_len - 1] != '-')
	{
		error(0, "another snapshot in progress: %s", name);
		goto out;
	}

	if (sscanf(status, "%u %llu/%llu %u -", &meta_chunk,
	           &meta_used, &meta_total, &era) != 4)
	{
		error(0, "can't parse device status: \"%s\"", name);
		goto out;
	}

	if (meta_chunk * SECTOR_SIZE != MD_BLOCK_SIZE)
	{
		error(0, "unexpected metadata block size: %u", meta_chunk);
		goto out;
	}

	/*
	 * calculate era array size
	 */

	nr_blocks = (length + chunk - 1) / chunk;
	era_blocks = (nr_blocks + ERAS_PER_BLOCK - 1) / ERAS_PER_BLOCK;
	snap_offset = era_blocks * meta_chunk;

	/*
	 * create snapshot and cow devices
	 */

	if (sn->sectors <= snap_offset)
	{
		error(0, "snapshot cow too small");
		goto out;
	}

	snprintf(snapuuid, sizeof(snapuuid), "%s-snap-%s", uuid, snap);

	if (era_dm_create_empty(snap, snapuuid, NULL))
		goto out;

	snprintf(cowuuid, sizeof(cowuuid), "%s-snap-%s-cow", uuid, snap);
	snprintf(cowname, sizeof(cowname), "%s-cow", snap);
	sprintf(table, "%u:%u %llu", sn->major, sn->minor,
	        (long long unsigned)snap_offset);

	if (era_dm_create(cowname, cowuuid,
	                  0, sn->sectors - snap_offset,
	                  TARGET_LINEAR, table, &info))
	{
		era_dm_remove(snap);
		goto out;
	}

	cow_major = info.major;
	cow_minor = info.minor;

	/*
	 * check and replace origin device with the "snapshot-origin" target
	 */

	strcat(uuid, "-orig");

	if (era_dm_info(NULL, uuid, &info,
	                sizeof(orig), orig, 0, NULL))
		goto out_snap;

	if (!info.exists)
	{
		error(0, "data device does not exists: %s", uuid);
		goto out_snap;
	}

	if (info.target_count != 1)
	{
		error(0, "invalid origin device: %s", orig);
		goto out_snap;
	}

	if (era_dm_first_table(NULL, uuid, &start, &length,
	                       sizeof(target), target,
	                       sizeof(table), table))
		goto out_snap;

	if (!strcmp(target, TARGET_LINEAR))
	{
		long long unsigned zero = 1;

		if (sscanf(table, "%u:%u %llu", &real_major, &real_minor,
		           &zero) != 3)
		{
			error(0, "can't parse linear table: %s", table);
			goto out_snap;
		}

		if (zero != 0)
		{
			error(0, "invalid origin device: %s", orig);
			goto out_snap;
		}

		strcpy(target, TARGET_ORIGIN);
		sprintf(table, "%u:%u", real_major, real_minor);

		if (era_dm_suspend(orig))
			goto out_snap;

		if (era_dm_load(orig, start, length, target, table, NULL))
		{
			era_dm_resume(orig);
			goto out_snap;
		}

		replace_with_linear++;

		if (era_dm_resume(orig))
			goto out_snap;
	}

	if (strcmp(target, TARGET_ORIGIN))
	{
		error(0, "unsupported target type: %s", target);
		goto out_snap;
	}

	/*
	 * send take_metadata_snap to era
	 */

	if (era_dm_message0(name, "take_metadata_snap"))
		goto out_snap;

	if (era_dm_first_status(name, NULL, &start, &length,
	                        0, NULL, sizeof(status), status))
		goto out_snap_meta;

	status_len = strlen(status);

	if (status_len == 0)
	{
		error(0, "empty device status: \"%s\"", name);
		goto out_snap_meta;
	}

	if (sscanf(status, "%u %llu/%llu %u %llu", &meta_chunk,
	           &meta_used, &meta_total, &era, &meta_snap) != 5)
	{
		error(0, "can't parse device status: \"%s\"", name);
		goto out_snap_meta;
	}

	if (meta_snap == 0)
	{
		error(0, "invalid era metadata snapshot offset: %llu",
		      meta_snap);
		goto out_snap_meta;
	}

	/*
	 * copy era_array and all archived writesets to snapshot
	 */

	if (era_snapshot_copy(md, sn, (uint64_t)meta_snap,
	                      COPY_ARRAY | COPY_WRITESETS))
		goto out_snap_meta;

	printf("%llu\n", (long long unsigned)meta_snap);
	printf("%llu\n", (long long unsigned)nr_blocks);
	printf("%llu\n", (long long unsigned)era_blocks);

	md_close(md);
	md_close(sn);
	return 0;

out_snap_meta:
	era_dm_message0(name, "drop_metadata_snap");

out_snap:
	era_dm_remove(snap);
	era_dm_remove(cowname);

	if (replace_with_linear)
	{
		sprintf(table, "%u:%u 0", real_major, real_minor);

		if (era_dm_suspend(orig))
			goto out;

		era_dm_load(orig, start, length, TARGET_LINEAR, table, NULL);
		era_dm_resume(orig);
	}

out:
	if (md != NULL)
		md_close(md);
	if (sn != NULL)
		md_close(sn);
	return -1;
}
