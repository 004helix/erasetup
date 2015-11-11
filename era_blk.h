/*
 * This file is released under the GPL.
 */

#ifndef __ERA_BLK_H__
#define __ERA_BLK_H__

#define SECTOR_SIZE 512

int blkopen(const char *device, int rw,
            unsigned *major, unsigned *minor, uint64_t *sectors);

#endif
