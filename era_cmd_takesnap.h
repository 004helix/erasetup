/*
 * This file is released under the GPL.
 */

#ifndef __ERA_CMD_TAKESNAP_H__
#define __ERA_CMD_TAKESNAP_H__

#define SNAPSHOT_NAME_FORMAT "era-snap-%s"
#define SNAPSHOT_UUID_FORMAT "ERA-SNAP-%s"
#define SNAPSHOT_PERSISTENT "N"
#define SNAPSHOT_CHUNK 8

#define RANDOM_DEVICE "/dev/urandom"

int era_takesnap(int argc, char **argv);

#endif
