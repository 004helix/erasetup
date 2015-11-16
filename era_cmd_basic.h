/*
 * This file is released under the GPL.
 */

#ifndef __ERA_CMD_BASIC_H__
#define __ERA_CMD_BASIC_H__

#define MIN_CHUNK_SIZE 8   /* sectors: 4k */
#define DEF_CHUNK_SIZE 128 /* sectors: 64k */

int era_create(int argc, char **argv);
int era_open(int argc, char **argv);
int era_close(int argc, char **argv);

#endif
