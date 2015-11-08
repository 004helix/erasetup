/*
 * This file is released under the GPL.
 */

#ifndef __ERA_BTREE_H__
#define __ERA_BTREE_H__

/*
 * leaf types
 */
enum leaf_type {
	LEAF_ARRAY = 1,
	LEAF_BITSET = 2,
	LEAF_WRITESET = 3
};

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
	__u8 values[0];
} __attribute__ ((packed));

typedef int (*datacb_t) (void *arg, unsigned size, void *keys, void *vals);
typedef int (*blockcb_t) (void *arg, uint64_t blocknr, void *block);

int era_array_walk(struct md *md, uint64_t root,
                   datacb_t datacb, void *dataarg,
                   blockcb_t blockcb, void *blockarg);

int era_bitset_walk(struct md *md, uint64_t root,
                    datacb_t datacb, void *dataarg,
                    blockcb_t blockcb, void *blockarg);

int era_writesets_walk(struct md *md, uint64_t root,
                       datacb_t datacb, void *dataarg,
                       blockcb_t blockcb, void *blockarg);

#endif
