/*
 * This file is released under the GPL.
 */

#ifndef _ERASETUP_H
#define _ERASETUP_H
#include <linux/types.h>
#include <stdint.h>

#define SUPERBLOCK_MAGIC 2126579579
#define SUPERBLOCK_CSUM_XOR 146538381
#define MIN_ERA_VERSION 1
#define MAX_ERA_VERSION 1

#define MD_BLOCK_SIZE 4096

#define SPACE_MAP_ROOT_SIZE 128
#define UUID_LEN 16

struct era_writeset {
	uint32_t nr_bits;
	uint64_t root;
} __attribute__ ((packed));

struct era_superblock {
	uint32_t csum;
	uint32_t flags;
	uint64_t blocknr;

	__u8 uuid[UUID_LEN];
	uint64_t magic;
	uint32_t version;

	__u8 metadata_space_map_root[SPACE_MAP_ROOT_SIZE];

	uint32_t data_block_size;
	uint32_t metadata_block_size;
	uint32_t nr_blocks;

	uint32_t current_era;
	struct era_writeset current_writeset;

	uint64_t writeset_tree_root;
	uint64_t era_array_root;

	uint64_t metadata_snap;
} __attribute__ ((packed));

#endif
