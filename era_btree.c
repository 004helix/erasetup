/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <endian.h>

#include "era.h"
#include "era_md.h"
#include "era_btree.h"

/*
 * visit btree array node
 *
 * leaf_type:
 *   LEAF_ARRAY:  leaf contains era_array elemens
 *   LEAF_BITSET: leaf contains bitset elements
 */
static int walk_array_node(struct md *md, uint64_t nr,
                           enum leaf_type type,
           int (*datacb)(unsigned size, void *keys, void *values),
           int (*blockcb)(unsigned nr, void *block))
{
	struct array_node *node;
	uint64_t blocknr;
	unsigned nr_entries;
	unsigned max_entries;
	unsigned value_size;
	unsigned expected;
	int rc = 0;

	node = md_block(md, 0, nr, ARRAY_CSUM_XOR);
	if (node == NULL)
		return -1;

	blocknr = le64toh(node->header.blocknr);
	if (blocknr != nr)
	{
		error(0, "bad array node: block number incorrect: "
		         "expected %llu, but got %llu",
		         (long long unsigned)nr,
		         (long long unsigned)blocknr);
		return -1;
	}

	switch (type)
	{
	case LEAF_ARRAY:
		expected = sizeof(uint32_t);
		break;
	case LEAF_BITSET:
		expected = sizeof(uint64_t);
		break;
	case LEAF_WRITESET:
		// can't get here, just putting in to pacify the compiler
		error(0, "unknown error");
		return -1;
	}

	value_size = le32toh(node->header.value_size);
	if (value_size != expected)
	{
		error(0, "bad btree node: value_size mismatch: "
		         "expected %u, but got %u",
		         expected, value_size);
		return -1;
	}

	max_entries = le32toh(node->header.max_entries);
	if (max_entries > (MD_BLOCK_SIZE - sizeof(*node)) / value_size)
	{
		error(0, "bad array node: max_entries too large: %u",
		         max_entries);
		return -1;
	}

	nr_entries = le32toh(node->header.nr_entries);
	if (nr_entries > max_entries)
	{
		error(0, "bad btree node: nr_entries (%u) > "
		         "max_entries (%u)", nr_entries, max_entries);
		return -1;
	}

	if (blockcb != NULL && blockcb(nr, node))
		return -1;

	if (nr_entries && datacb != NULL)
		rc = datacb(nr_entries, NULL, node->values);

	return rc ? -1 : 0;
}

/*
 * visit btree node
 */
static int walk_btree_node(struct md *md, uint64_t nr,
                           enum leaf_type type,
           int (*datacb)(unsigned size, void *keys, void *values),
           int (*blockcb)(unsigned nr, void *block))
{
	uint64_t blocknr;
	unsigned i, flags;
	unsigned value_size, expected;
	unsigned nr_entries, max_entries;
	struct btree_node *node;
	int rc = 0;

	node = md_block(md, MD_CACHED, nr, BTREE_CSUM_XOR);
	if (node == NULL)
		return -1;

	blocknr = le64toh(node->header.blocknr);
	if (blocknr != nr)
	{
		error(0, "bad btree node: block number incorrect: "
		         "expected %llu, but got: %llu",
		         (long long unsigned)nr,
		         (long long unsigned)blocknr);
		return -1;
	}

	flags = le32toh(node->header.flags);
	if (flags & INTERNAL_NODE && flags & LEAF_NODE)
	{
		error(0, "bad btree node: both internnal and leaf "
		         "bits are set");
		return -1;
	}

	value_size = le32toh(node->header.value_size);
	if (flags & INTERNAL_NODE)
	{
		expected = sizeof(uint64_t);
	}
	else
	{
		switch (type)
		{
		case LEAF_ARRAY:
			expected = sizeof(uint64_t);
			break;
		case LEAF_BITSET:
			expected = sizeof(uint64_t);
			break;
		case LEAF_WRITESET:
			expected = sizeof(struct era_writeset);
			break;
		}
	}

	if (value_size != expected)
	{
		error(0, "bad btree node: value_size mismatch: "
		         "expected %u, but got %u",
		         expected, value_size);
		return -1;
	}

	max_entries = le32toh(node->header.max_entries);
	if (max_entries >
	    (MD_BLOCK_SIZE - sizeof(*node)) / (sizeof(uint64_t) + value_size))
	{
		error(0, "bad btree node: max_entries too large: %u",
		         max_entries);
		return -1;
	}

	if (max_entries % 3)
	{
		error(0, "bad btree node: max entries is not divisible "
		         "by 3: %u", max_entries);
		return -1;
	}

	nr_entries = le32toh(node->header.nr_entries);
	if (nr_entries > max_entries)
	{
		error(0, "bad btree node: nr_entries (%u) > "
		         "max_entries (%u)", nr_entries, max_entries);
		return -1;
	}

	if (blockcb != NULL && blockcb(nr, node))
		return -1;

	if (flags & INTERNAL_NODE || type == LEAF_ARRAY || type == LEAF_BITSET)
	{
		uint64_t *values;
		int rc;

		for (i = 0; i < nr_entries; i++)
		{
			/*
			 * node address can be changed on
			 * each iteration due to md cache resize.
			 */
			node = md_block(md, MD_CACHED, nr, BTREE_CSUM_XOR);
			if (node == NULL)
				return -1;

			values = (uint64_t *)&node->keys[max_entries];

			if (flags & INTERNAL_NODE)
				rc = walk_btree_node(md, values[i], type,
				                     datacb, blockcb);
			else
				rc = walk_array_node(md, values[i], type,
				                     datacb, blockcb);

			if (rc == -1)
				return -1;
		}

		return 0;
	}

	/*
	 * only LEAF_WRITESET type can be here
	 */

	if (nr_entries && datacb != NULL)
		rc = datacb(nr_entries, node->keys, &node->keys[max_entries]);

	return rc ? -1 : 0;
}

// walk era array
int era_array_walk(struct md *md, uint64_t root,
                   int (*datacb)(unsigned size, void *keys, void *values),
                   int (*blockcb)(unsigned nr, void *block))
{
	if (walk_btree_node(md, root, LEAF_ARRAY, datacb, blockcb) == -1)
		return -1;

	if (datacb != NULL && datacb(0, NULL, NULL))
		return -1;

	return 0;
}

// walk era bitset
int era_bitset_walk(struct md *md, uint64_t root,
                    int (*datacb)(unsigned size, void *keys, void *values),
                    int (*blockcb)(unsigned nr, void *block))
{
	if (walk_btree_node(md, root, LEAF_BITSET, datacb, blockcb) == -1)
		return -1;

	if (datacb != NULL && datacb(0, NULL, NULL))
		return -1;

	return 0;
}

// walk era writeset
int era_writeset_walk(struct md *md, uint64_t root,
                      int (*datacb)(unsigned size, void *keys, void *values),
                      int (*blockcb)(unsigned nr, void *block))
{
	if (walk_btree_node(md, root, LEAF_WRITESET, datacb, blockcb) == -1)
		return -1;

	if (datacb != NULL && datacb(0, NULL, NULL))
		return -1;

	return 0;
}
