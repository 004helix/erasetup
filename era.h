/*
 * This file is released under the GPL.
 */

#ifndef _ERASETUP_H
#define _ERASETUP_H
#include <linux/types.h>
#include <stdint.h>
#include <stdio.h>

#define SECTOR_SIZE 512

#define UUID_PREFIX     "ERA-"
#define TARGET_ERA      "era"
#define TARGET_LINEAR   "linear"
#define TARGET_SNAPSHOT "snapshot"
#define TARGET_ORIGIN   "snapshot-origin"

extern void *empty_block;
extern int verbose;
extern int force;

char *uuid2str(const void *uuid);
void usage(FILE *out, int code);
void error(int err, const char *fmt, ...)
	__attribute__ ((format (printf, 2, 3)));

#define SUPERBLOCK_MAGIC 2126579579
#define SUPERBLOCK_CSUM_XOR 146538381
#define MIN_ERA_VERSION 1
#define MAX_ERA_VERSION 1

#define SPACE_MAP_ROOT_SIZE 128
#define UUID_LEN 16

struct era_writeset {
	__le32 nr_bits;
	__le64 root;
} __attribute__ ((packed));

struct era_superblock {
	__le32 csum;
	__le32 flags;
	__le64 blocknr;

	__u8 uuid[UUID_LEN];
	__le64 magic;
	__le32 version;

	__u8 metadata_space_map_root[SPACE_MAP_ROOT_SIZE];

	__le32 data_block_size;
	__le32 metadata_block_size;
	__le32 nr_blocks;

	__le32 current_era;
	struct era_writeset current_writeset;

	__le64 writeset_tree_root;
	__le64 era_array_root;

	__le64 metadata_snap;
} __attribute__ ((packed));

int era_sb_check(struct era_superblock *sb);

#endif
