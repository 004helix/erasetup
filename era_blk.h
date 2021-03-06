/*
 * This file is released under the GPL.
 */

#ifndef __ERA_BLK_H__
#define __ERA_BLK_H__

#define SECTOR_SIZE 512
#define SECTOR_SHIFT 9

int blkopen(const char *device, int rw,
            unsigned *major, unsigned *minor, uint64_t *sectors);

int blkopen2(unsigned major, unsigned minor, int rw, uint64_t *sectors);

#endif
