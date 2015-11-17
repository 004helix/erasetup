/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <endian.h>
#include <string.h>
#include <errno.h>

#include "crc32c.h"
#include "era.h"
#include "era_dm.h"
#include "era_md.h"
#include "era_blk.h"
#include "era_snapshot.h"
#include "era_cmd_takesnap.h"

static void *get_snapshot_uuid(struct md *sn, const char *device)
{
	struct era_snapshot_superblock *ssb;
	static char uuid[UUID_LEN];
	int fd;

	ssb = md_block(sn, MD_NOCRC, 0, 0);
	if (ssb == NULL)
		return NULL;

	if (le32toh(ssb->magic) == SNAP_SUPERBLOCK_MAGIC)
	{
		uint32_t csum;

		csum = crc_update(0xffffffff, &ssb->flags,
		                  MD_BLOCK_SIZE - sizeof(uint32_t));
		csum ^= SNAP_SUPERBLOCK_CSUM_XOR;

		if (le32toh(ssb->csum) == csum &&
		    le32toh(ssb->version) == SNAP_VERSION)
		{
			memcpy(uuid, ssb->uuid, UUID_LEN);
			return uuid;
		}
	}

	if (!force && memcmp(ssb, empty_block, MD_BLOCK_SIZE))
	{
		error(0, "existing data found on %s", device);
		return NULL;
	}

	fd = open(RANDOM_DEVICE, O_RDONLY);
	if (fd == -1)
	{
		error(errno, "can't open %s", RANDOM_DEVICE);
		return NULL;
	}

	if (read(fd, uuid, UUID_LEN) != UUID_LEN)
	{
		error(errno, "can't read %s", RANDOM_DEVICE);
		close(fd);
		return NULL;
	}

	close(fd);

	return uuid;
}

int era_takesnap(int argc, char **argv)
{
	struct md *md, *sn;
	char *name, *snap, *uuid;
	struct era_snapshot_superblock *ssb;
	struct era_superblock *sb;
	struct era_dm_info info;
	uint32_t csum;
	uint64_t start, length;
	uint64_t snap_offset;
	unsigned nr_blocks, snap_blocks;
	unsigned replace_with_linear;
	unsigned drop_metadata_snap;
	unsigned long *bitmap;
	unsigned long long meta_snap;
	unsigned long long meta_used;
	unsigned long long meta_total;
	unsigned cow_major, cow_minor;
	unsigned meta_major, meta_minor;
	unsigned orig_major, orig_minor;
	unsigned real_major, real_minor;
	unsigned chunk, meta_chunk, era;
	size_t status_len;
	char era_dmuuid[DM_UUID_LEN];
	char orig_dmuuid[DM_UUID_LEN];
	char snap_dmname[DM_NAME_LEN];
	char snap_dmuuid[DM_UUID_LEN];
	char cow_dmname[DM_NAME_LEN];
	char cow_dmuuid[DM_UUID_LEN];
	char target[DM_MAX_TYPE_NAME];
	char orig[DM_NAME_LEN];
	char status[256];
	char table[256];
	int fd;

	switch (argc)
	{
	case 0:
		error(0, "device name argument expected");
		usage(stderr, 1);
	case 1:
		error(0, "snapshot device argument expected");
		usage(stderr, 1);
	case 2:
		break;
	default:
		error(0, "unknown argument: %s", argv[2]);
		usage(stderr, 1);
	}

	name = argv[0];
	snap = argv[1];

	sn = NULL;
	md = NULL;

	replace_with_linear = 0;
	drop_metadata_snap = 0;

	/*
	 * open metadata device
	 */

	if (era_dm_info(name, NULL, &info,
	                0, NULL, sizeof(era_dmuuid) - 16, era_dmuuid))
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

	if (era_dm_first_table(NULL, era_dmuuid, &start, &length,
	                       sizeof(target), target,
	                       sizeof(table), table))
		goto out;

	if (strcmp(target, TARGET_ERA))
	{
		error(0, "unsupported target type: %s", target);
		goto out;
	}

	if (sscanf(table, "%u:%u %u:%u %u",
	           &meta_major, &meta_minor,
	           &orig_major, &orig_minor,
	           &chunk) != 5)
	{
		error(0, "can't parse device table: \"%s\"", table);
		goto out;
	}

	fd = blkopen2(meta_major, meta_minor, 0, NULL);
	if (fd == -1)
		goto out;

	md = md_open(NULL, fd);
	if (md == NULL)
		goto out;

	printv(1, "era: era %s\n", table);

	/*
	 * check era device status
	 */

	if (era_dm_first_status(NULL, era_dmuuid, &start, &length,
	                        sizeof(target), target,
	                        sizeof(status), status))
		goto out;

	status_len = strlen(status);

	if (status_len == 0)
	{
		error(0, "empty device status: %s", name);
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

	if ((meta_chunk << SECTOR_SHIFT) != MD_BLOCK_SIZE)
	{
		error(0, "unexpected metadata block size: %u", meta_chunk);
		goto out;
	}

	printv(1, "era: %s\n", status);

	/*
	 * open snapshot device
	 */

	sn = md_open(snap, 1);
	if (sn == NULL)
		goto out;

	uuid = get_snapshot_uuid(sn, snap);
	if (uuid == NULL)
		goto out;

	printv(1, "snapshot: uuid %s\n", uuid2str(uuid));

	/*
	 * calculate era array size
	 */

	nr_blocks = (unsigned)((length + chunk - 1) / chunk);
	snap_blocks = (nr_blocks + ERAS_PER_BLOCK - 1) / ERAS_PER_BLOCK;
	snap_offset = (1 + snap_blocks) * meta_chunk;

	printv(1, "snapshot: metadata %llu KiB\n",
	       (long long unsigned)((snap_offset << SECTOR_SHIFT) / 1024));

	/*
	 * create snapshot and cow devices
	 */

	if (snap_offset >= sn->sectors)
	{
		error(0, "snapshot device too small");
		goto out;
	}

	snprintf(snap_dmname, sizeof(snap_dmname),
	         "era-snap-%s", uuid2str(uuid));

	snprintf(snap_dmuuid, sizeof(snap_dmuuid),
	         "ERA-SNAP-%s", uuid2str(uuid));

	if (era_dm_create_empty(snap_dmname, snap_dmuuid, NULL))
		goto out;

	snprintf(cow_dmname, sizeof(cow_dmname),
	         "era-snap-%s-cow", uuid2str(uuid));

	snprintf(cow_dmuuid, sizeof(cow_dmuuid),
	         "ERA-SNAP-%s-cow", uuid2str(uuid));

	sprintf(table, "%u:%u %llu", sn->major, sn->minor,
	        (long long unsigned)snap_offset);

	if (era_dm_create(cow_dmname, cow_dmuuid,
	                  0, sn->sectors - snap_offset,
	                  TARGET_LINEAR, table, &info))
	{
		era_dm_remove(snap_dmname);
		goto out;
	}

	cow_major = info.major;
	cow_minor = info.minor;

	printv(1, "snapshot: cow %s\n", cow_dmname);
	printv(1, "snapshot: name %s\n", snap_dmname);

	/*
	 * check and replace origin device with the "snapshot-origin" target
	 */

	strcpy(orig_dmuuid, era_dmuuid);
	strcat(orig_dmuuid, "-orig");

	if (era_dm_info(NULL, orig_dmuuid, &info,
	                sizeof(orig), orig, 0, NULL))
		goto out_snap;

	if (!info.exists)
	{
		error(0, "data device does not exists: %s", orig_dmuuid);
		goto out_snap;
	}

	if (info.target_count != 1 ||
	    info.major != orig_major ||
	    info.minor != orig_minor)
	{
		error(0, "invalid origin device: %s", orig);
		goto out_snap;
	}

	if (era_dm_first_table(NULL, orig_dmuuid, &start, &length,
	                       sizeof(target), target,
	                       sizeof(table), table))
		goto out_snap;

	printv(1, "origin: %s %s\n", target, table);

	if (!strcmp(target, TARGET_LINEAR))
	{
		long long unsigned zero = 3;

		if (sscanf(table, "%u:%u %llu", &real_major, &real_minor,
		           &zero) != 3)
		{
			error(0, "can't parse linear table: \"%s\"", table);
			goto out_snap;
		}

		if (zero != 0)
		{
			error(0, "invalid origin device: %s", orig);
			goto out_snap;
		}

		strcpy(target, TARGET_ORIGIN);
		sprintf(table, "%u:%u", real_major, real_minor);

		printv(1, "origin: suspend\n");

		if (era_dm_suspend(orig))
			goto out_snap;

		printv(1, "origin: %s %s\n", target, table);

		if (era_dm_load(orig, 0, length, target, table, NULL))
		{
			era_dm_resume(orig);
			goto out_snap;
		}

		replace_with_linear++;

		printv(1, "origin: resume\n");

		if (era_dm_resume(orig))
			goto out_snap;
	}

	if (strcmp(target, TARGET_ORIGIN))
	{
		error(0, "unsupported target type: %s", target);
		goto out_snap;
	}

	if (sscanf(table, "%u:%u", &real_major, &real_minor) != 2)
	{
		error(0, "can't parse origin table: \"%s\"", table);
		goto out_snap;
	}

	/*
	 * send take_metadata_snap to era
	 */

	printv(1, "era: take metadata snapshot\n");

	if (era_dm_message0(name, "take_metadata_snap"))
		goto out_snap;

	drop_metadata_snap++;

	if (era_dm_first_status(NULL, era_dmuuid, &start, &length,
	                        0, NULL, sizeof(status), status))
		goto out_snap_drop;

	status_len = strlen(status);

	if (status_len == 0)
	{
		error(0, "empty device status: %s", name);
		goto out_snap_drop;
	}

	if (sscanf(status, "%u %llu/%llu %u %llu", &meta_chunk,
	           &meta_used, &meta_total, &era, &meta_snap) != 5)
	{
		error(0, "can't parse device status: \"%s\"", name);
		goto out_snap_drop;
	}

	if (meta_snap == 0)
	{
		error(0, "invalid era metadata snapshot offset: %llu",
		      meta_snap);
		goto out_snap_drop;
	}

	printv(1, "era: %s\n", status);

	/*
	 * copy era_array and all archived writesets to snapshot
	 */

	printv(1, "era: copy metadata snapshot\n");

	if (era_snapshot_copy(md, sn, (uint64_t)meta_snap, nr_blocks))
		goto out_snap_drop;

	/*
	 * drop metadata snapshot
	 */

	printv(1, "era: drop metadata snapshot\n");

	if (era_dm_message0(name, "drop_metadata_snap"))
		goto out_snap_drop;

	drop_metadata_snap = 0;

	/*
	 * suspend era and origin devices
	 */

	printv(1, "era: suspend\n");

	if (era_dm_suspend(name))
		goto out_resume;

	printv(1, "origin: suspend\n");

	if (era_dm_suspend(orig))
		goto out_resume;

	/*
	 * read and check superblock
	 */

	sb = md_block(md, MD_CACHED, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL || era_sb_check(sb))
		goto out_resume;

	if (le32toh(sb->current_era) != era)
	{
		error(0, "unexpected current era after suspend: "
		         "expected %u, but got %u",
		         era, le32toh(sb->current_era));
		goto out_resume;
	}

	/*
	 * copy bitmap for current era
	 */

	printv(1, "era: copy bitset for era %u\n", era);

	bitmap = era_snapshot_getbitmap(md, era, 0, nr_blocks);
	if (bitmap == NULL)
		goto out_resume;

	/*
	 * load table into snapshot device
	 */

	snprintf(table, sizeof(table), "%u:%u %u:%u %s %u",
	         real_major, real_minor, cow_major, cow_minor,
	         SNAPSHOT_PERSISTENT, SNAPSHOT_CHUNK);

	printv(1, "snapshot: %s %s\n", TARGET_SNAPSHOT, table);

	if (md_write(sn, snap_blocks + 1, empty_block))
		return -1;

	if (era_dm_load(snap_dmname, 0, length, TARGET_SNAPSHOT, table, NULL))
	{
		free(bitmap);
		goto out_resume;
	}

	printv(1, "snapshot: resume\n");

	if (era_dm_resume(snap_dmname))
	{
		free(bitmap);
		goto out_resume;
	}

	/*
	 * resume era device
	 */

	printv(1, "origin: resume\n");

	if (era_dm_resume(orig))
	{
		free(bitmap);
		goto out_resume;
	}

	printv(1, "era: resume\n");

	if (era_dm_resume(name))
	{
		free(bitmap);
		goto out_resume;
	}

	/*
	 * digest bitmap
	 */

	printv(1, "snapshot: digest bitmap for era %u\n", era);

	if (era_snapshot_digest(sn, era, bitmap, nr_blocks))
	{
		free(bitmap);
		goto out_snap;
	}

	free(bitmap);

	/*
	 * save snapshot superblock
	 */

	printv(1, "snapshot: write superblock\n");

	ssb = sn->buffer;

	memset(ssb, 0, MD_BLOCK_SIZE);
	memcpy(ssb->uuid, uuid, UUID_LEN);

	ssb->magic = htole64(SNAP_SUPERBLOCK_MAGIC);
	ssb->version = htole32(1);

	ssb->data_block_size = htole32(chunk);
	ssb->metadata_block_size = htole32(MD_BLOCK_SIZE >> SECTOR_SHIFT);
	ssb->nr_blocks = htole32(nr_blocks);
	ssb->snapshot_era = htole32(era);

	csum = crc_update(0xffffffff, &ssb->flags,
	                  MD_BLOCK_SIZE - sizeof(uint32_t));
	ssb->csum = htole32(csum ^ SNAP_SUPERBLOCK_CSUM_XOR);

	if (md_write(sn, 0, ssb))
		goto out_snap;

	/*
	 * done
	 */

	md_close(sn);
	md_close(md);
	return 0;

out_resume:
	era_dm_resume(orig);
	era_dm_resume(name);

out_snap_drop:
	if (drop_metadata_snap)
		era_dm_message0(name, "drop_metadata_snap");

out_snap:
	era_dm_remove(snap_dmname);
	era_dm_remove(cow_dmname);

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
