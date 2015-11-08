/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>
#include <errno.h>

#include "era.h"
#include "era_md.h"
#include "era_sm.h"
#include "era_btree.h"

#define BITS_PER_LONG ((int)sizeof(long) * 8)
#define LONGS(bits) (((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static inline int test_bit(unsigned long nr, unsigned long *bitmap)
{
	unsigned long offset = nr / BITS_PER_LONG;
	unsigned long bit = nr & (BITS_PER_LONG - 1);
	return (bitmap[offset] >> bit) & 1;
}

static inline void set_bit(unsigned long nr, unsigned long *bitmap)
{
	unsigned long offset = nr / BITS_PER_LONG;
	unsigned long bit = nr & (BITS_PER_LONG - 1);
	bitmap[offset] |= 1UL << bit;
}

/*
 * rebuild spacemap
 */
int era_spacemap_rebuild(struct md *md)
{
	unsigned long *bitmap;

	bitmap = malloc(sizeof(long) * LONGS(md->blocks));
	if (bitmap == NULL)
	{
		error(ENOMEM, NULL);
		return -1;
	}

	return 0;
}

int era_spacemap_walk(struct md *md, uint64_t root, unsigned char *refcnt)
{
	struct disk_metadata_index *idx;
	unsigned i, j, k, total;
	unsigned total_ie;

	idx = md_block(md, MD_CACHED, root, INDEX_CSUM_XOR);
	if (idx == NULL)
		return -1;

	if (root != le64toh(idx->blocknr))
	{
		error(0, "bad index node: block number incorrect: "
		         "expected %llu, but got: %llu",
		         (long long unsigned)root,
		         (long long unsigned)le64toh(idx->blocknr));
		return -1;
	}

	memset(refcnt, 0, md->blocks);

	total = 0;
	total_ie = (md->blocks + ENTRIES_PER_BLOCK - 1) / ENTRIES_PER_BLOCK;
	for (i = 0; i < total_ie; i++)
	{
		struct disk_index_entry *ie = &idx->index[i];
		struct disk_bitmap_header *node;
		unsigned char *bytes;

		node = md_block(md, 0, le64toh(ie->blocknr), BITMAP_CSUM_XOR);

		if (ie->blocknr != node->blocknr)
		{
			error(0, "bad index node: block number "
			         "incorrect: expected %llu, "
			         "but got: %llu",
			         (long long unsigned)le64toh(ie->blocknr),
			         (long long unsigned)le64toh(node->blocknr));
			return -1;
		}

		//printf("nfb: %u\n", le32toh(ie->none_free_before));
		//printf("bnr: %llu\n", (long long unsigned)(le64toh(node->blocknr)));

		bytes = (unsigned char *)node +
		        sizeof(struct disk_bitmap_header);
		for (j = 0; j < BYTES_PER_BLOCK; j++)
		{
			unsigned char byte = bytes[j];
			for (k = 0; k < ENTRIES_PER_BYTE; k++)
			{
				unsigned hi = (byte & 1);
				unsigned lo = (byte & 2) >> 1;
				refcnt[total++] = (hi << 1) | lo;
				if (total == md->blocks)
					goto done;
				byte >>= 2;
			}
		}
	}

done:
	return 0;
}
