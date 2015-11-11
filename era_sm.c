/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>
#include <errno.h>

#include "crc32c.h"
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

static inline int test_and_set_bit(unsigned long nr, unsigned long *bitmap)
{
	unsigned long offset = nr / BITS_PER_LONG;
	unsigned long bit = nr & (BITS_PER_LONG - 1);
	int rc = (bitmap[offset] >> bit) & 1;
	bitmap[offset] |= 1UL << bit;
	return rc;
}

static unsigned long first_unset_bit(unsigned long size, unsigned long *bitmap)
{
	unsigned long longs = LONGS(size);
	unsigned long offset;
	for (offset = 0; offset < longs; offset++)
	{
		int nr = ffsl(~bitmap[offset]);
		if (nr > 0)
		{
			unsigned long rc = BITS_PER_LONG * offset + nr;
			if (rc > size)
				return 0;
			else
				return rc;
		}
	}
	return 0;
}

/*
 * rebuild spacemap
 */

static int bitmap_cb(void *arg, uint64_t blocknr, void *block)
{
	unsigned long *bitmap = arg;
	if (test_and_set_bit((unsigned long)blocknr, bitmap))
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
	unsigned nr_blocks;
	unsigned long *bitmap;
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
	return 0;

out:
	free(eras);
	free(ws);
	return -1;
}

/*
 * create and save new spacemap using bitmap;
 * only one ref count per block supported
 */

static int era_spacemap_write(struct md *md, unsigned long *bitmap,
                              struct disk_sm_root *smr)
{
	struct disk_metadata_index *index;
	struct btree_node *ref_count;
	uint64_t index_root;
	uint64_t ref_count_root;
	uint64_t nr_allocated;
	uint32_t csum;
	unsigned max_entries;
	uint64_t bm_blocks;
	unsigned i;

	/*
	 * allocate memory for the index and ref count blocks
	 */

	ref_count = malloc(MD_BLOCK_SIZE);
	if (ref_count == NULL)
	{
		error(ENOMEM, NULL);
		return -1;
	}

	index = malloc(MD_BLOCK_SIZE);
	if (index == NULL)
	{
		error(ENOMEM, NULL);
		free(ref_count);
		return -1;
	}

	memset(ref_count, 0, MD_BLOCK_SIZE);
	memset(index, 0, MD_BLOCK_SIZE);

	/*
	 * create index block
	 */

	bm_blocks = (md->blocks + ENTRIES_PER_BLOCK - 1) / ENTRIES_PER_BLOCK;
	if (bm_blocks > MAX_METADATA_BITMAPS)
	{
		error(0, "metadata is too large");
		goto out;
	}

	index_root = first_unset_bit(md->blocks, bitmap);
	if (index_root == 0)
	{
		error(0, "there is no free space in metadata "
		         "for the spacemap index block");
		goto out;
	}
	set_bit(--index_root, bitmap);

	index->blocknr = htole64(index_root);

	/*
	 * create ref count block (always contains 0 entries)
	 */

	max_entries = (MD_BLOCK_SIZE - sizeof(*ref_count)) /
	              (sizeof(uint64_t) + sizeof(uint32_t));
	while (max_entries % 3)
		max_entries--;

	ref_count->header.flags = htole32(LEAF_NODE);
	ref_count->header.nr_entries = 0;
	ref_count->header.max_entries = htole32(max_entries);
	ref_count->header.value_size = htole32(sizeof(uint32_t));

	ref_count_root = first_unset_bit(md->blocks, bitmap);
	if (ref_count_root == 0)
	{
		error(0, "there is no free space in metadata "
		         "for the spacemap ref count root block");
		goto out;
	}
	set_bit(--ref_count_root, bitmap);

	ref_count->header.blocknr = htole64(ref_count_root);

	/*
	 * allocate bitmap blocks
	 */

	for (i = 0; i < bm_blocks; i++)
	{
		uint64_t root = first_unset_bit(md->blocks, bitmap);
		if (root == 0)
		{
			error(0, "there is no free space in metadata "
			         "for the spacemap bitmap block");
			goto out;
		}
		set_bit(--root, bitmap);

		index->index[i].blocknr = htole64(root);
	}

	/*
	 * write bitmap blocks
	 */

	nr_allocated = 0;

	for (i = 0; i < bm_blocks; i++)
	{
		struct disk_bitmap_header *hdr = md->buffer;
		unsigned char *bytes = md->buffer + sizeof(*hdr);
		uint64_t root = le64toh(index->index[i].blocknr);
		uint32_t nr_free;
		unsigned from, to;
		unsigned j, k;

		memset(md->buffer, 0, MD_BLOCK_SIZE);

		from = i * ENTRIES_PER_BLOCK;
		to = from + ENTRIES_PER_BLOCK > md->blocks ?
		     md->blocks : from + ENTRIES_PER_BLOCK;

		nr_free = to - from;

		for (j = from, k = 0; j < to; j++, k++)
		{
			if (test_bit(j, bitmap))
			{
				/*
				 * two bits per entry,
				 * high and low bits are swapped
				 */
				unsigned offset = k / ENTRIES_PER_BYTE;
				unsigned bit = ((k & 3) << 1) + 1;
				bytes[offset] |= 1 << bit;
				nr_allocated++;
				nr_free--;
			}
		}

		hdr->blocknr = htole64(root);

		csum = crc_update(0xffffffff, &hdr->not_used,
		                  MD_BLOCK_SIZE - sizeof(hdr->csum));
		hdr->csum = htole32(csum ^ BITMAP_CSUM_XOR);

		if (md_write(md, root, hdr) == -1)
			goto out;

		index->index[i].none_free_before = 0;
		index->index[i].nr_free = htole32(nr_free);
	}

	/*
	 * write ref count block
	 */

	csum = crc_update(0xffffffff, &ref_count->header.flags,
	                  MD_BLOCK_SIZE - sizeof(ref_count->header.csum));
	ref_count->header.csum = htole32(csum ^ BTREE_CSUM_XOR);

	memcpy(md->buffer, ref_count, MD_BLOCK_SIZE);
	if (md_write(md, ref_count_root, md->buffer))
		goto out;

	/*
	 * write index block
	 */

	csum = crc_update(0xffffffff, &index->padding,
	                  MD_BLOCK_SIZE - sizeof(index->csum));
	index->csum = htole32(csum ^ INDEX_CSUM_XOR);

	memcpy(md->buffer, index, MD_BLOCK_SIZE);
	if (md_write(md, index_root, md->buffer))
		goto out;

	/*
	 * save disk_sm_root
	 */

	smr->nr_blocks = htole64(md->blocks);
	smr->nr_allocated = htole64(nr_allocated);
	smr->bitmap_root = htole64(index_root);
	smr->ref_count_root = htole64(ref_count_root);

	/*
	 * done
	 */

	free(ref_count);
	free(index);
	return 0;
out:
	free(ref_count);
	free(index);
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
	struct disk_sm_root smr;
	struct writeset_state wst;
	struct era_superblock *sb;
	unsigned current_writeset_bits;
	uint64_t current_writeset_root;
	uint64_t writeset_tree_root;
	uint64_t era_array_root;
	unsigned nr_blocks;
	unsigned total;
	uint32_t csum;

	/*
	 * check metadata size
	 */

	if (md->blocks > MAX_METADATA_BITMAPS * ENTRIES_PER_BLOCK)
	{
		md->blocks = MAX_METADATA_BITMAPS * ENTRIES_PER_BLOCK;
		error(0, "Warning: metadata device is too big, "
		         "only first %u MiB will be used",
		         (unsigned)(md->blocks * MD_BLOCK_SIZE / 1048576));
	}

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

	sb = md_block(md, MD_CACHED, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL)
		goto out;

	nr_blocks = le32toh(sb->nr_blocks);
	era_array_root = le32toh(sb->era_array_root);
	writeset_tree_root = le32toh(sb->writeset_tree_root);
	current_writeset_root = le64toh(sb->current_writeset.root);
	current_writeset_bits = le32toh(sb->current_writeset.nr_bits);

	/*
	 * check and mark used blocks by current_writeset
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
	 * check and mark used blocks by
	 * writesets and all bitsets in it
	 */

	md_flush(md);

	wst = (struct writeset_state) {
		.nr_blocks = nr_blocks,
		.bitmap = bitmap,
		.md = md,
	};

	if (era_writesets_walk(md, writeset_tree_root,
	                       writeset_cb, &wst,
	                       bitmap_cb, bitmap) == -1)
		goto out;

	/*
	 * check and mark used blocks by era_array
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
		         "expected %u, but got %u",
		         nr_blocks, total);
		goto out;
	}

	/*
	 * mark superblock as used
	 */

	if (bitmap_cb(bitmap, 0, NULL))
		goto out;

	/*
	 * write spacemap
	 */

	if (era_spacemap_write(md, bitmap, &smr))
		goto out;

	/*
	 * read and modify superblock
	 */

	sb = md_block(md, 0, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL)
		goto out;

	// drop metadata snapshot (not supported for now)
	sb->metadata_snap = 0;

	// save spacemap root
	memset(sb->metadata_space_map_root, 0, SPACE_MAP_ROOT_SIZE);
	memcpy(sb->metadata_space_map_root, &smr, sizeof(smr));

	// calculate new checksum
	csum = crc_update(0xffffffff, &sb->flags,
	                  MD_BLOCK_SIZE - sizeof(sb->csum));
	sb->csum = htole32(csum ^ SUPERBLOCK_CSUM_XOR);

	// write modified superblock
	if (md_write(md, 0, sb))
		goto out;

	/*
	 * done
	 */

	free(bitmap);
	return 0;

out:
	free(bitmap);
	return -1;
}
