/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef VDO_INT_MAP_H
#define VDO_INT_MAP_H

#include <linux/compiler.h>
#include <linux/types.h>

#include "hash-map.h"

/**
 * DOC: int_map
 *
 * An int_map associates pointers (void *) with integer keys (u64). NULL pointer values are
 * not supported.
 *
 * The map is implemented as hash table, which should provide constant-time insert, query, and
 * remove operations, although the insert may occasionally grow the table, which is linear in the
 * number of entries in the map. The table will grow as needed to hold new entries, but will not
 * shrink as entries are removed.
 */

#endif /* VDO_INT_MAP_H */
