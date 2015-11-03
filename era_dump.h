/*
 * This file is released under the GPL.
 */

#ifndef __ERA_DUMP_H__
#define __ERA_DUMP_H__

/* era array */
#define ERA_ENTRY_SIZE       4  /* bytes */

/* era bitset */
#define BITSET_ENTRY_SIZE    8  /* bytes */
#define BITS_PER_ARRAY_ENTRY 64 /* bits  */

struct dump {
	int fd;               /* dump-file fd                    */
	unsigned cur_ents;    /* dump-file current entries count */
	unsigned max_ents;    /* dump-file maximum entries       */
	unsigned cur_bs_ents; /* current bitset entries count    */
	unsigned max_bs_ents; /* maximum bitset entries          */
	unsigned __padding;
	uint64_t bitset[0];   /* bitset                          */
};

struct dump *dump_open(const char *file, unsigned total_blocks);
int dump_append_array(struct dump *dump, void *data, unsigned size);
int dump_append_bitset(struct dump *dump, uint64_t entry);
void dump_close(struct dump *dump);

#endif
