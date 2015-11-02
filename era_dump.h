/*
 * This file is released under the GPL.
 */

#ifndef __ERA_DUMP_H__
#define __ERA_DUMP_H__

struct dump {
	int fd;
	int size;
};

struct dump *dump_open(const char *file);
int dump_append(struct dump *dump, void *data, unsigned size);
void dump_close(struct dump *dump);

#endif
