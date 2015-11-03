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

void era_dm_init(void);
int era_dm_create(const char *name, const char *uuid,
                  uint64_t size, const char *target, const char *table);

#endif
