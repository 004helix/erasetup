/*
 * This file is released under the GPL.
 */

#ifndef __ERA_BTREE_H__
#define __ERA_BTREE_H__

#define BTREE_CSUM_XOR 121107
#define ARRAY_CSUM_XOR 595846735

enum node_flags {
	INTERNAL_NODE = 1,
	LEAF_NODE = 1 << 1
};

struct node_header {
	__le32 csum;
	__le32 flags;
	__le64 blocknr;

	__le32 nr_entries;
	__le32 max_entries;
	__le32 value_size;
	__le32 padding;
} __attribute__ ((packed));

struct btree_node {
	struct node_header header;
	__le64 keys[0];
} __attribute__ ((packed));

int era_array_walk(struct md *md);

#endif
