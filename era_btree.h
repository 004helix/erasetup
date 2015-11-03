/*
 * This file is released under the GPL.
 */

#ifndef __ERA_BTREE_H__
#define __ERA_BTREE_H__

#define LEAF_ARRAY 0
#define LEAF_BITSET 1

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

struct array_header {
	__le32 csum;
	__le32 max_entries;
	__le32 nr_entries;
	__le32 value_size;
	__le64 blocknr;
} __attribute__ ((packed));

struct array_node {
	struct array_header header;
	union {
		__le32 values32[0];
		__le64 values64[0];
	};
} __attribute__ ((packed));

int era_array_dump(struct md *md, struct dump *dump);
int era_bitset_dump(struct md *md, struct dump *dump);

#endif
