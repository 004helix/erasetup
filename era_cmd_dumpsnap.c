/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <endian.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <unistd.h>

#include "era.h"
#include "era_dm.h"
#include "era_md.h"
#include "era_blk.h"
#include "era_btree.h"
#include "era_snapshot.h"
#include "era_cmd_dumpsnap.h"

/*
 * dumpsnap command
 */

int era_dumpsnap(int argc, char **argv)
{
	char dmname[DM_NAME_LEN];
	char dmuuid[DM_UUID_LEN];
	struct md *sn;
	struct era_snapshot_superblock *ssb;
	struct era_dm_info info;
	char uuid[UUID_LEN];
	uint64_t era_size, length;
	unsigned nr, count, last;
	unsigned i, snap_blocks;
	unsigned j, nr_blocks;
	unsigned chunk;
	unsigned era;

	if (argc == 0)
	{
		error(0, "snapshot device argument expected");
		usage(stderr, 1);
	}

	if (argc > 1)
	{
		error(0, "unknown argument: %s", argv[1]);
		usage(stderr, 1);
	}

	/*
	 * open ahnd check snapshot superblock
	 */

	sn = md_open(argv[0], 0);
	if (!sn)
		return -1;

	ssb = md_block(sn, 0, 0, SNAP_SUPERBLOCK_CSUM_XOR);
	if (!ssb || era_ssb_check(ssb))
		goto out;

	memcpy(uuid, ssb->uuid, UUID_LEN);

	era_size = le64toh(ssb->era_size);
	nr_blocks = le32toh(ssb->nr_blocks);
	chunk = le32toh(ssb->data_block_size);
	era = le32toh(ssb->snapshot_era);

	if (((era_size + chunk - 1) / chunk) != nr_blocks)
	{
		error(0, "invalid snapshot superblock");
		goto out;
	}

	snap_blocks = (nr_blocks + ERAS_PER_BLOCK - 1) / ERAS_PER_BLOCK;

	/*
	 * check snapshot device
	 */

	snprintf(dmuuid, sizeof(dmuuid), "ERA-SNAP-%s", uuid2str(uuid));

	if (era_dm_info(NULL, dmuuid, &info, sizeof(dmname), dmname, 0, NULL))
		goto out;

	if (!info.exists)
	{
		error(0, "snapshot inactive");
		goto out;
	}

	if (info.target_count != 1)
	{
		error(0, "invalid snapshot");
		goto out;
	}

	if (era_dm_first_table(NULL, dmuuid, NULL, &length,
	                       0, NULL, 0, NULL))
		goto out;

	if (length != era_size)
	{
		error(0, "invalid snapshot");
		goto out;
	}

	/*
	 * dump snapshot blocks
	 */

	printf("<snapshot block_size=\"%u\" blocks=\"%u\" era=\"%u\"\n"
	       "          dev=\"/dev/mapper/%s\">\n",
	       chunk, nr_blocks, era, dmname);

	count = 0;
	nr = 0;

	for (i = 0; i < snap_blocks; i++)
	{
		struct era_snapshot_node *node;

		node = md_block(sn, 0, i + 1, SNAP_ARRAY_CSUM_XOR);
		if (!node)
			goto out;

		if (le64toh(node->blocknr) != i + 1)
		{
			error(0, "bad block number: expected %u, but got %u",
			      i + 1, (unsigned)le64toh(node->blocknr));
			goto out;
		}

		for (j = 0; j < ERAS_PER_BLOCK; j++, nr++)
		{
			unsigned era;

			if (nr >= nr_blocks)
				break;

			era = le32toh(node->era[j]);

			if (count == 0)
			{
				last = era;
				count++;
				continue;
			}

			if (last == era)
			{
				count++;
				continue;
			}

			if (count == 1)
			{
				printf("  <block block=\"%u\" era=\"%u\"/>\n",
				       nr - 1, last);
			}
			else {
				printf("  <range begin=\"%u\" end=\"%u\" "
				       "era=\"%u\"/>\n",
				       nr - count, nr - 1, last);
			}

			last = era;
			count = 1;
		}
	}

	switch (count)
	{
	case 0:
		break;
	case 1:
		printf("  <block block=\"%u\" era=\"%u\"/>\n", nr - 1, last);
		break;
	default:
		printf("  <range begin=\"%u\" end=\"%u\" era=\"%u\"/>\n",
		       nr - count, nr - 1, last);
	}

	printf("</snapshot>\n");

	md_close(sn);
	return 0;

out:
	md_close(sn);
	return -1;
}
