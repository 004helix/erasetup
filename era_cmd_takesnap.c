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

struct device {
	struct era_dm_info info;
	char target[DM_MAX_TYPE_NAME];
	char name[DM_NAME_LEN];
	char uuid[DM_UUID_LEN];
	char status[256];
	char table[256];
	uint64_t size;
};

static void *get_snapshot_uuid(struct md *sn, const char *device)
{
	struct era_snapshot_superblock *ssb;
	static char uuid[UUID_LEN];
	int fd;

	ssb = md_block(sn, MD_NOCRC, 0, 0);
	if (!ssb)
		return NULL;

	if (le32toh(ssb->magic) == SNAP_SUPERBLOCK_MAGIC)
	{
		uint32_t csum;

		csum = crc_update(crc_init(), &ssb->flags,
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
	void *uuid;
	char *snap_path;
	struct md *md, *sn;
	struct era_superblock *sb;
	struct era_snapshot_superblock *ssb;
	uint32_t csum;
	uint64_t snap_offset;
	unsigned nr_blocks, snap_blocks;
	unsigned replace_with_linear;
	unsigned drop_metadata_snap;
	unsigned long *bitmap;
	unsigned long long meta_snap;
	unsigned long long meta_used;
	unsigned long long meta_total;
	unsigned meta_major, meta_minor;
	unsigned orig_major, orig_minor;
	unsigned real_major, real_minor;
	unsigned chunk, meta_chunk;
	unsigned current_era;
	struct device *era, *orig;
	struct device *snap, *cow;
	size_t len;
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

	era = malloc(sizeof(*era));
	cow = malloc(sizeof(*cow));
	snap = malloc(sizeof(*snap));
	orig = malloc(sizeof(*orig));

	if (!era || !cow || !snap || !orig)
	{
		error(ENOMEM, NULL);
		goto out;
	}

	snprintf(era->name, sizeof(era->name), "%s", argv[0]);
	snap_path = argv[1];

	sn = NULL;
	md = NULL;

	replace_with_linear = 0;
	drop_metadata_snap = 0;

	/*
	 * open metadata device
	 */

	if (era_dm_info(era->name, NULL, &era->info,
	                0, NULL, sizeof(era->uuid), era->uuid))
		goto out;

	if (!era->info.exists)
	{
		error(0, "device %s does not exists", era->name);
		goto out;
	}

	if (era->info.target_count != 1)
	{
		error(0, "invalid device %s", era->name);
		goto out;
	}

	if (era_dm_first_table(era->name, NULL, NULL, &era->size,
	                       sizeof(era->target), era->target,
	                       sizeof(era->table), era->table))
		goto out;

	if (strcmp(era->target, TARGET_ERA))
	{
		error(0, "unsupported target type: %s", era->target);
		goto out;
	}

	if (sscanf(era->table, "%u:%u %u:%u %u",
	           &meta_major, &meta_minor,
	           &orig_major, &orig_minor,
	           &chunk) != 5)
	{
		error(0, "can't parse device table: %s", era->table);
		goto out;
	}

	fd = blkopen2(meta_major, meta_minor, 0, NULL);
	if (fd == -1)
		goto out;

	md = md_open(NULL, fd);
	if (!md)
		goto out;

	printv(1, "era: era %s\n", era->table);

	/*
	 * check era device status
	 */

	if (era_dm_first_status(era->name, NULL, NULL, NULL,
	                        0, NULL, sizeof(era->status), era->status))
		goto out;

	len = strlen(era->status);

	if (len == 0)
	{
		error(0, "empty device status: %s", era->name);
		goto out;
	}

	if (era->status[len - 1] != '-')
	{
		error(0, "another snapshot in progress: %s", era->name);
		goto out;
	}

	if (sscanf(era->status, "%u %llu/%llu %u -", &meta_chunk,
	           &meta_used, &meta_total, &current_era) != 4)
	{
		error(0, "can't parse era status: %s", era->status);
		goto out;
	}

	if ((meta_chunk << SECTOR_SHIFT) != MD_BLOCK_SIZE)
	{
		error(0, "unexpected metadata block size: %u", meta_chunk);
		goto out;
	}

	printv(1, "era: %s\n", era->status);

	/*
	 * open snapshot device
	 */

	sn = md_open(snap_path, 1);
	if (!sn)
		goto out;

	uuid = get_snapshot_uuid(sn, snap_path);
	if (!uuid)
		goto out;

	printv(1, "snapshot: uuid %s\n", uuid2str(uuid));

	/*
	 * calculate era array size
	 */

	nr_blocks = (unsigned)((era->size + chunk - 1) / chunk);
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

	snprintf(snap->name, sizeof(snap->name),
	         "era-snap-%s", uuid2str(uuid));

	snprintf(snap->uuid, sizeof(snap->uuid),
	         "ERA-SNAP-%s", uuid2str(uuid));

	if (era_dm_create_empty(snap->name, snap->uuid, NULL))
		goto out;

	snprintf(cow->name, sizeof(cow->name),
	         "era-snap-%s-cow", uuid2str(uuid));

	snprintf(cow->uuid, sizeof(cow->uuid),
	         "ERA-SNAP-%s-cow", uuid2str(uuid));

	snprintf(cow->table, sizeof(cow->table),
	         "%u:%u %llu", sn->major, sn->minor,
	         (long long unsigned)snap_offset);

	strcpy(cow->target, TARGET_LINEAR);

	cow->size = sn->sectors - snap_offset;

	if (era_dm_create(cow->name, cow->uuid, 0, cow->size,
	                  cow->target, cow->table, &cow->info))
	{
		era_dm_remove(snap->name);
		goto out;
	}

	printv(1, "snapshot: cow %s\n", cow->name);
	printv(1, "snapshot: name %s\n", snap->name);

	/*
	 * check and replace origin device with the "snapshot-origin" target
	 */

	snprintf(orig->uuid, sizeof(orig->uuid), "%s-orig", era->uuid);

	if (era_dm_info(NULL, orig->uuid, &orig->info,
	                sizeof(orig->name), orig->name, 0, NULL))
		goto out_snap;

	if (!orig->info.exists)
	{
		error(0, "origin device does not exists: %s", orig->uuid);
		goto out_snap;
	}

	if (orig->info.target_count != 1 ||
	    orig->info.major != orig_major ||
	    orig->info.minor != orig_minor)
	{
		error(0, "invalid origin device: %s", orig->name);
		goto out_snap;
	}

	if (era_dm_first_table(NULL, orig->uuid, NULL, &orig->size,
	                       sizeof(orig->target), orig->target,
	                       sizeof(orig->table), orig->table))
		goto out_snap;

	printv(1, "origin: %s %s\n", orig->target, orig->table);

	if (!strcmp(orig->target, TARGET_LINEAR))
	{
		long long unsigned zero = 3;

		if (sscanf(orig->table, "%u:%u %llu",
		           &real_major, &real_minor, &zero) != 3)
		{
			error(0, "can't parse origin table: %s", orig->table);
			goto out_snap;
		}

		if (zero != 0)
		{
			error(0, "invalid origin table: %s", orig->table);
			goto out_snap;
		}

		strcpy(orig->target, TARGET_ORIGIN);
		snprintf(orig->table, sizeof(orig->table), "%u:%u",
		         real_major, real_minor);

		printv(1, "origin: suspend\n");

		if (era_dm_suspend(orig->name))
			goto out_snap;

		printv(1, "origin: %s %s\n", orig->target, orig->table);

		if (era_dm_load(orig->name, 0, orig->size,
		                orig->target, orig->table, NULL))
		{
			era_dm_resume(orig->name);
			goto out_snap;
		}

		replace_with_linear++;

		printv(1, "origin: resume\n");

		if (era_dm_resume(orig->name))
			goto out_snap;
	}

	if (strcmp(orig->target, TARGET_ORIGIN))
	{
		error(0, "unsupported origin target: %s", orig->target);
		goto out_snap;
	}

	if (sscanf(orig->table, "%u:%u", &real_major, &real_minor) != 2)
	{
		error(0, "can't parse origin table: %s", orig->table);
		goto out_snap;
	}

	/*
	 * send take_metadata_snap to era
	 */

	printv(1, "era: take metadata snapshot\n");

	if (era_dm_message0(era->name, "take_metadata_snap"))
		goto out_snap;

	drop_metadata_snap++;

	if (era_dm_first_status(NULL, era->uuid, NULL, NULL, 0, NULL,
	                        sizeof(era->status), era->status))
		goto out_snap_drop;

	len = strlen(era->status);

	if (len == 0)
	{
		error(0, "empty status: %s", era->name);
		goto out_snap_drop;
	}

	if (sscanf(era->status, "%u %llu/%llu %u %llu", &meta_chunk,
	           &meta_used, &meta_total, &current_era, &meta_snap) != 5)
	{
		error(0, "can't parse era status: %s", era->status);
		goto out_snap_drop;
	}

	if (meta_snap == 0)
	{
		error(0, "invalid era metadata snapshot offset: %llu",
		      meta_snap);
		goto out_snap_drop;
	}

	printv(1, "era: %s\n", era->status);

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

	if (era_dm_message0(era->name, "drop_metadata_snap"))
		goto out_snap_drop;

	drop_metadata_snap = 0;

	/*
	 * suspend era and origin devices
	 */

	printv(1, "era: suspend\n");

	if (era_dm_suspend(era->name))
		goto out_resume;

	printv(1, "origin: suspend\n");

	if (era_dm_suspend(orig->name))
		goto out_resume;

	/*
	 * read and check superblock
	 */

	sb = md_block(md, MD_CACHED, 0, SUPERBLOCK_CSUM_XOR);
	if (!sb || era_sb_check(sb))
		goto out_resume;

	if (le32toh(sb->current_era) != current_era)
	{
		error(0, "unexpected current era after suspend: "
		         "expected %u, but got %u",
		         current_era, le32toh(sb->current_era));
		goto out_resume;
	}

	/*
	 * copy bitmap for current era
	 */

	printv(1, "snapshot: copy bitmap for era %u\n", current_era);

	bitmap = era_snapshot_getbitmap(md, current_era, 0, nr_blocks);
	if (!bitmap)
		goto out_resume;

	/*
	 * load table into snapshot device
	 */

	strcpy(snap->target, TARGET_SNAPSHOT);

	snprintf(snap->table, sizeof(snap->table), "%u:%u %u:%u %s %u",
	         real_major, real_minor, cow->info.major, cow->info.minor,
	         SNAPSHOT_PERSISTENT, SNAPSHOT_CHUNK);

	printv(1, "snapshot: %s %s\n", snap->target, snap->table);

	if (md_write(sn, snap_blocks + 1, empty_block))
	{
		free(bitmap);
		goto out_resume;
	}

	if (era_dm_load(snap->name, 0, era->size,
	                snap->target, snap->table, &snap->info))
	{
		free(bitmap);
		goto out_resume;
	}

	printv(1, "snapshot: resume\n");

	if (era_dm_resume(snap->name))
	{
		free(bitmap);
		goto out_resume;
	}

	/*
	 * resume era device
	 */

	printv(1, "origin: resume\n");

	if (era_dm_resume(orig->name))
	{
		free(bitmap);
		goto out_resume;
	}

	printv(1, "era: resume\n");

	if (era_dm_resume(era->name))
	{
		free(bitmap);
		goto out_resume;
	}

	/*
	 * digest bitmap
	 */

	printv(1, "snapshot: digest bitmap for era %u\n", current_era);

	if (era_snapshot_digest(sn, current_era, bitmap, nr_blocks))
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
	ssb->snapshot_era = htole32(current_era);

	csum = crc_update(crc_init(), &ssb->flags,
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
	era_dm_resume(orig->name);
	era_dm_resume(era->name);

out_snap_drop:
	if (drop_metadata_snap)
		era_dm_message0(era->name, "drop_metadata_snap");

out_snap:
	era_dm_remove(snap->name);
	era_dm_remove(cow->name);

	if (replace_with_linear)
	{
		strcpy(orig->target, TARGET_LINEAR);

		snprintf(orig->table, sizeof(orig->table),
		         "%u:%u 0", real_major, real_minor);

		if (era_dm_suspend(orig->name))
			goto out;

		era_dm_load(orig->name, 0, orig->size,
		            orig->target, orig->table, NULL);
		era_dm_resume(orig->name);
	}

out:
	if (snap)
		free(snap);
	if (orig)
		free(orig);
	if (era)
		free(era);
	if (cow)
		free(cow);
	if (md)
		md_close(md);
	if (sn)
		md_close(sn);
	return -1;
}
