/*
 * This file is released under the GPL.
 */

#ifndef __ERA_DM_H__
#define __ERA_DM_H__

#define UUID_PREFIX     "ERA-"
#define TARGET_ERA      "era"
#define TARGET_LINEAR   "linear"
#define TARGET_SNAPSHOT "snapshot"
#define TARGET_ORIGIN   "snapshot-origin"

#include <linux/dm-ioctl.h>

/* truncated dm_info */
struct era_dm_info {
	int exists;
	int suspended;
	uint32_t major;
	uint32_t minor;
	int32_t open_count;
	int32_t target_count;
};

void era_dm_init(void);
void era_dm_exit(void);

int era_dm_suspend(const char *name);
int era_dm_resume(const char *name);
int era_dm_remove(const char *name);
int era_dm_clear(const char *name);

int era_dm_message0(const char *name, const char *message);

int era_dm_create(const char *name, const char *uuid,
                  uint64_t start, uint64_t size,
                  const char *target, const char *table,
                  struct era_dm_info *info);

int era_dm_load(const char *name,
                uint64_t start, uint64_t size,
                const char *target, const char *table,
                struct era_dm_info *info);

int era_dm_create_empty(const char *name, const char *uuid,
                        struct era_dm_info *info);

int era_dm_info(const char *name,
                const char *uuid,
                struct era_dm_info *info,
                size_t name_size, char *name_ptr,
                size_t uuid_size, char *uuid_ptr);

int era_dm_first_table(const char *name,
                       const char *uuid,
                       uint64_t *start, uint64_t *length,
                       size_t target_size, char *target_ptr,
                       size_t params_size, char *params_ptr);

int era_dm_first_status(const char *name,
                        const char *uuid,
                        uint64_t *start, uint64_t *length,
                        size_t target_size, char *target_ptr,
                        size_t params_size, char *params_ptr);

int era_dm_list(int (*cb)(void *arg, const char *name), void *cbarg);

#endif
