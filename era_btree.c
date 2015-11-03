/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <endian.h>

#include <unistd.h>

#include "era.h"
#include "era_md.h"
#include "era_dump.h"
#include "era_btree.h"

/*
 * visit btree array node
 *
 * leaf_type:
 *   LEAF_ARRAY:  leaf contains era_array elemens
 *   LEAF_BITSET: leaf contains bitset elements
 */
static int era_btree_walk_array_node(struct md *md, struct dump *dump,
                                     uint64_t nr, int leaf_type)
{
	struct array_node *node;
	uint64_t blocknr;
	unsigned nr_entries;
	unsigned max_entries;
	unsigned value_size;
	unsigned i;

	node = md_block(md, 0, nr, ARRAY_CSUM_XOR);
	if (node == NULL)
		return -1;

	blocknr = le64toh(node->header.blocknr);
	if (blocknr != nr)
	{
		fprintf(stderr, "bad array node: block number incorrect "
		        "(want: %llu, on disk: %llu)\n",
		        (long long unsigned)nr,
		        (long long unsigned)blocknr);
		return -1;
	}

	value_size = le32toh(node->header.value_size);
	if (value_size == 0 || value_size >= MD_BLOCK_SIZE)
	{
		fprintf(stderr, "bad array node: incorrect value size "
		        "(%u)\n", value_size);
		return -1;
	}

	max_entries = le32toh(node->header.max_entries);
	if (max_entries > (MD_BLOCK_SIZE - sizeof(*node)) / value_size)
	{
		fprintf(stderr, "bad array node: max_entries too large "
		        "(%u)\n", max_entries);
		return -1;
	}

	nr_entries = le32toh(node->header.nr_entries);
	if (nr_entries > max_entries)
	{
		fprintf(stderr, "bad array node: nr_entries > max_entries "
		        "(%u > %u)\n", nr_entries, max_entries);
		return -1;
	}

	if (dump == NULL)
		return nr_entries;

	switch (leaf_type)
	{
	case LEAF_ARRAY:
		if (value_size != ERA_ENTRY_SIZE)
		{
			fprintf(stderr, "bad array node: incorrect value size "
			        "for array leaf (%u)\n", value_size);
			return -1;
		}
		if (dump_append_array(dump, node->values32, nr_entries) == -1)
			return -1;
		return nr_entries;

	case LEAF_BITSET:
		if (value_size != BITSET_ENTRY_SIZE)
		{
			fprintf(stderr, "bad array node: incorrect value size "
			        "for bitset leaf (%u)\n", value_size);
			return -1;
		}
		for (i = 0; i < nr_entries; i++)
		{
			uint64_t entry = le64toh(node->values64[i]);
			if (dump_append_bitset(dump, entry) == -1)
				return -1;
		}
		return nr_entries;

	default:
		fprintf(stderr, "unknown leaf type\n");
		return -1;
	}
}

// visit btree node
static int era_btree_walk_btree_node(struct md *md, struct dump *dump,
                                     uint64_t nr, int leaf_type)
{
	struct btree_node *node;
	uint64_t blocknr, next;
	unsigned i, flags, value_size;
	unsigned nr_entries, max_entries;
	int rc, total = 0;

	node = md_block(md, MD_CACHED, nr, BTREE_CSUM_XOR);
	if (node == NULL)
		return -1;

	blocknr = le64toh(node->header.blocknr);
	if (blocknr != nr)
	{
		fprintf(stderr, "bad btree node: block number incorrect "
		        "(want: %llu, on disk: %llu)\n",
		        (long long unsigned)nr,
		        (long long unsigned)blocknr);
		return -1;
	}

	flags = le32toh(node->header.flags);
	if (flags & INTERNAL_NODE && flags & LEAF_NODE)
	{
		fprintf(stderr, "bad btree node: both internnal and leaf "
		        "bits are set\n");
		return -1;
	}

	value_size = le32toh(node->header.value_size);
	if (value_size != sizeof(uint64_t))
	{
		fprintf(stderr, "bad btree node: value_size != %lu\n",
		        sizeof(uint64_t));
		return -1;
	}

	max_entries = le32toh(node->header.max_entries);
	if (max_entries >
	    (MD_BLOCK_SIZE - sizeof(*node)) / (sizeof(uint64_t) * 2))
	{
		fprintf(stderr, "bad btree node: max_entries too large "
		        "(%u)\n", max_entries);
		return -1;
	}
	if (max_entries % 3)
	{
		fprintf(stderr, "bad btree node: max entries is not divisible "
		        "by 3 (%u)\n", max_entries);
		return -1;
	}

	nr_entries = le32toh(node->header.nr_entries);
	if (nr_entries > max_entries)
	{
		fprintf(stderr, "bad btree node: nr_entries > max_entries "
		        "(%u > %u)\n", nr_entries, max_entries);
		return -1;
	}

	for (total = 0, i = 0; i < nr_entries; i++)
	{
		/*
		 * node address can be changed on
		 * each request due to md cache resize.
		 */
		node = md_block(md, MD_CACHED, nr, BTREE_CSUM_XOR);
		if (node == NULL)
			return -1;

		next = le64toh(node->keys[max_entries + i]);

		if (flags & INTERNAL_NODE)
			rc = era_btree_walk_btree_node(md, dump, next, leaf_type);
		else
			rc = era_btree_walk_array_node(md, dump, next, leaf_type);

		if (rc == -1)
			return -1;

		total += rc;
	}

	return total;
}

// dump era array
int era_array_dump(struct md *md, struct dump *dump)
{
	struct era_superblock *sb;
	uint64_t root;

	sb = md_block(md, MD_CACHED, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL)
		return -1;

	root = le64toh(sb->era_array_root);

	if (era_btree_walk_btree_node(md, dump, root, LEAF_ARRAY) == -1)
		return -1;

	if (dump->cur_ents < dump->max_ents)
	{
		fprintf(stderr, "not enough era_array entries "
		                "(want: %u, found: %u)",
		                dump->max_ents, dump->cur_ents);
		return -1;
	}

	return 0;
}

// dump era bitset
int era_bitset_dump(struct md *md, struct dump *dump)
{
	struct era_superblock *sb;
	uint64_t root;

	sb = md_block(md, MD_CACHED, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL)
		return -1;

	root = le64toh(sb->current_writeset.root);

	if (era_btree_walk_btree_node(md, dump, root, LEAF_BITSET) == -1)
		return -1;

	if (dump->cur_bs_ents < dump->max_bs_ents)
	{
		fprintf(stderr, "not enough era bitset entries "
		                "(want: %u, found: %i)",
		                dump->max_bs_ents,
		                dump->cur_bs_ents);
		return -1;
	}

	return 0;
}
