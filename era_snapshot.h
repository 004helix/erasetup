/*
 * This file is released under the GPL.
 */

#ifndef __ERA_SNAPSHOT_H__
#define __ERA_SNAPSHOT_H__

struct era_snapshot_node {
	__le32 csum;
	__le32 flags;
	__le64 blocknr;

	__le32 era[0];
} __attribute__ ((packed));

#endif
