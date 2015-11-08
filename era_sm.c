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

static inline int test_and_set_bit(unsigned long nr, unsigned long *bitmap)
{
	unsigned long offset = nr / BITS_PER_LONG;
	unsigned long bit = nr & (BITS_PER_LONG - 1);
	int rc = (bitmap[offset] >> bit) & 1;
	bitmap[offset] |= 1UL << bit;
	return rc;
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

static int bitmap_cb(void *arg, uint64_t blocknr, void *block)
{
	unsigned long *bitmap = arg;
	if (test_and_set_bit(blocknr, bitmap))
	{
		error(0, "block %llu already in use",
		         (long long unsigned)blocknr);
		return -1;
	}
	return 0;
}

static int bitset_cb(void *arg, unsigned size, void *dummy, void *data)
{
	unsigned *total = arg;
	*total += size;
	return 0;
}

static int era_array_cb(void *arg, unsigned size, void *dummy, void *data)
{
	unsigned *total = arg;
	*total += size;
	return 0;
}

struct writeset_state {
	unsigned long *bitmap;
	unsigned nr_blocks;
	struct md *md;
};

static int writeset_cb(void *arg, unsigned size, void *keys, void *values)
{
	struct writeset_state *state = arg;
	struct era_writeset *ws;
	uint64_t *eras;
	unsigned i;

	if (size == 0)
		return 0;

	ws = malloc(sizeof(*ws) * size);
	if (ws == NULL)
	{
		error(ENOMEM, NULL);
		return -1;
	}

	eras = malloc(sizeof(*eras) * size);
	if (eras == NULL)
	{
		error(ENOMEM, NULL);
		free(ws);
		return -1;
	}

	memcpy(ws, values, sizeof(*ws) * size);
	memcpy(eras, keys, sizeof(*eras) * size);

	for (i = 0; i < size; i++)
	{
		unsigned era = (unsigned)le64toh(eras[i]);
		unsigned bits = le32toh(ws[i].nr_bits);
		uint64_t root = le64toh(ws[i].root);
		unsigned total = 0;

		if (bits != state->nr_blocks)
		{
			error(0, "writeset.nr_bits for era %u mismatch: "
			         "expected %u, but got %u",
			         era, state->nr_blocks, bits);
			goto out;
		}

		md_flush(state->md);

		if (era_bitset_walk(state->md, root,
		                    bitset_cb, &total,
		                    bitmap_cb, state->bitmap) == -1)
			goto out;

		if (total != (state->nr_blocks + 63) / 64)
		{
			error(0, "writeset for era %u elements mismatch: "
			         "expected %u, but got %u",
			         era, (state->nr_blocks + 63) / 64, total);
			goto out;
		}
	}

	free(eras);
	free(ws);
	return -1;

out:
	free(eras);
	free(ws);
	return -1;
}

/*
 * check and count all blocks used by all btrees in era superblock
 * drop metadata snapshot, build and save new space-map
 * write new era superblock
 */

int era_spacemap_rebuild(struct md *md)
{
	unsigned long *bitmap;
	struct writeset_state wst;
	struct era_superblock *sb;
	unsigned current_writeset_bits;
	uint64_t current_writeset_root;
	uint64_t writeset_tree_root;
	uint64_t era_array_root;
	unsigned nr_blocks;
	unsigned total;

	/*
	 * prepare spacemap bitmap
	 */

	bitmap = malloc(sizeof(long) * LONGS(md->blocks));
	if (bitmap == NULL)
	{
		error(ENOMEM, NULL);
		return -1;
	}

	memset(bitmap, 0, sizeof(long) * LONGS(md->blocks));

	/*
	 * read btree roots from superblock
	 */

	md_flush(md);

	sb = md_block(md, MD_CACHED, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL)
		goto out;

	nr_blocks = le32toh(sb->nr_blocks);
	era_array_root = le32toh(sb->era_array_root);
	writeset_tree_root = le32toh(sb->writeset_tree_root);
	current_writeset_root = le64toh(sb->current_writeset.root);
	current_writeset_bits = le32toh(sb->current_writeset.nr_bits);

	/*
	 * check and set used blocks by current_writeset
	 */

	if (current_writeset_root != 0)
	{
		if (current_writeset_bits != nr_blocks)
		{
			error(0, "current_writeset.nr_bits mismatch: "
			         "expected %u, but got %u",
			         nr_blocks, current_writeset_bits);
			goto out;
		}

		md_flush(md);

		total = 0;

		if (era_bitset_walk(md, current_writeset_root,
		                    bitset_cb, &total,
		                    bitmap_cb, bitmap) == -1)
			goto out;

		if (total != (nr_blocks + 63) / 64)
		{
			error(0, "current_writeset elements mismatch: "
			         "expected %u, but got %u",
			         (nr_blocks + 63) / 64, total);
			goto out;
		}
	}

	/*
	 * check and set used blocks by
	 * writesets and all bitsets in it
	 */

	md_flush(md);

	wst = (struct writeset_state) {
		.bitmap = bitmap,
		.nr_blocks = nr_blocks,
		.md = md,
	};

	if (era_writesets_walk(md, writeset_tree_root,
	                       writeset_cb, &wst,
	                       bitmap_cb, bitmap) == -1)
		goto out;

	/*
	 * check and set used blocks by era_array
	 */

	md_flush(md);

	total = 0;

	if (era_array_walk(md, era_array_root,
	                   era_array_cb, &total,
	                   bitmap_cb, bitmap) == -1)
		goto out;

	if (total != nr_blocks)
	{
		error(0, "era_array elements mismatch: "
		         "expacted %u, but got %u",
		         nr_blocks, total);
		goto out;
	}

	/*
	 * mark superblock as used
	 */

	if (bitmap_cb(bitmap, 0, NULL))
		goto out;

	free(bitmap);
	return 0;
out:
	free(bitmap);
	return -1;
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
