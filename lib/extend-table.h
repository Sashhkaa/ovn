/*
 * Copyright (c) 2017 DtDream Technology Co.,Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef EXTEND_TABLE_H
#define EXTEND_TABLE_H 1

#define EXT_TABLE_ID_INVALID 0

#include "openvswitch/hmap.h"
#include "openvswitch/list.h"
#include "openvswitch/uuid.h"

struct id_pool;

/* Used to manage expansion tables associated with Flow table,
 * such as the Group Table or Meter Table. */
struct ovn_extend_table {
    char *name; /* Used to identify this table in a user friendly way,
                 * e.g., for logging. */
    uint32_t n_ids;
    struct id_pool *table_ids; /* Used to allocate ids in either desired or
                                * existing (or both).  If the same "name"
                                * exists in both desired and existing tables,
                                * they must share the same ID.  The "peer"
                                * pointer would tell if the ID is still used by
                                * the same item in the peer table. */
    struct hmap desired;
    struct hmap lflow_to_desired; /* Index for looking up desired table
                                   * items from given lflow uuid, with
                                   * ovn_extend_table_lflow_to_desired nodes.
                                   */
    struct hmap existing;
};

struct ovn_extend_table_lflow_to_desired {
    struct hmap_node hmap_node; /* In ovn_extend_table.lflow_to_desired. */
    struct uuid lflow_uuid;
    struct ovs_list desired; /* List of desired items used by the lflow. */
};

struct ovn_extend_table_info {
    struct hmap_node hmap_node;
    char *name;         /* Name for the table entity. */
    uint32_t table_id;
    struct ovn_extend_table_info *peer; /* The extend tables exist as pairs,
                                           one for desired items and one for
                                           existing items. "peer" maintains the
                                           link between a pair of items in
                                           these tables. If "peer" is NULL, it
                                           means the counterpart is not created
                                           yet or deleted already. */
    struct hmap references; /* The lflows that are using this item, with
                             * ovn_extend_table_lflow_ref nodes. Only useful
                             * for items in ovn_extend_table.desired. */

    /* The fields below are only populated for group entries created via
     * ovn_extend_table_assign_group_id(), to support incremental
     * INSERT_BUCKET/REMOVE_BUCKET updates instead of whole-group replace.
     * 'group_header' is NULL for entries created via the regular
     * ovn_extend_table_assign_id() (whole-content-hash) path; that is the
     * signal for whether an entry participates in bucket-level diffing. */
    char *group_header;          /* e.g. "type=select,selection_method=...". */
    struct hmap desired_buckets; /* struct ovn_extend_table_bucket, by key. */
    struct hmap existing_buckets;
};

/* One bucket (e.g. one LB backend) within a group's desired_buckets or
 * existing_buckets. */
struct ovn_extend_table_bucket {
    struct hmap_node hmap_node; /* Keyed by hash of 'key'. */
    char *key;                  /* Stable identity, e.g. backend "ip:port". */
    uint32_t bucket_id;
    char *content;               /* e.g. "weight:100,actions=...". */
    struct ovn_extend_table_bucket *peer; /* Same role as
                                             ovn_extend_table_info.peer,
                                             one level down: links a
                                             desired_buckets entry to its
                                             existing_buckets counterpart
                                             (or NULL if not installed yet /
                                             already removed). */
};

/* Input to ovn_extend_table_assign_group_id(): one desired bucket. */
struct ovn_extend_table_bucket_spec {
    const char *key;
    const char *content;
};

/* Maintains the link between a lflow and an ovn_extend_table_info item in
 * ovn_extend_table.desired, indexed by both
 * ovn_extend_table_lflow_to_desired.desired and
 * ovn_extend_table_info.references.
 *
 * The struct is allocated whenever a new reference happens.
 * It destroyed when a lflow is deleted (for all the desired table_info
 * used by it), or when the lflow_to_desired table is being cleared.
 * */
struct ovn_extend_table_lflow_ref {
    struct hmap_node hmap_node; /* In ovn_extend_table_info.references. */
    struct ovs_list list_node; /* In ovn_extend_table_lflow_to_desired.desired.
                                */
    struct uuid lflow_uuid;
    struct ovn_extend_table_info *desired;
};

void ovn_extend_table_init(struct ovn_extend_table *, const char *table_name,
                           uint32_t n_ids);
void ovn_extend_table_reinit(struct ovn_extend_table *, uint32_t n_ids);

void ovn_extend_table_destroy(struct ovn_extend_table *);

struct ovn_extend_table_info *ovn_extend_table_lookup(
    struct hmap *, const struct ovn_extend_table_info *);

void ovn_extend_table_clear(struct ovn_extend_table *, bool);

void ovn_extend_table_remove_existing(struct ovn_extend_table *,
                                      struct ovn_extend_table_info *);

void ovn_extend_table_remove_desired(struct ovn_extend_table *,
                                     const struct uuid *lflow_uuid);

/* Copy the contents of desired to existing. */
void ovn_extend_table_sync(struct ovn_extend_table *);

uint32_t ovn_extend_table_assign_id(struct ovn_extend_table *,
                                    const char *name,
                                    struct uuid lflow_uuid);

/* Like ovn_extend_table_assign_id(), but keyed by a caller-provided stable
 * 'group_key' (independent of bucket content, e.g. an LB's stage-hint)
 * instead of the full group content. The group's table_id therefore stays
 * stable when 'buckets' changes across calls with the same 'group_key'.
 *
 * Replaces the info's desired bucket set with 'buckets' on every call, so
 * that ofctrl.c can diff it against what's already installed
 * (existing_buckets) and emit incremental INSERT_BUCKET/REMOVE_BUCKET
 * instead of a whole-group replace. */
uint32_t ovn_extend_table_assign_group_id(
    struct ovn_extend_table *table, const char *group_key,
    const char *group_header, const struct ovn_extend_table_bucket_spec *,
    size_t n_buckets, struct uuid lflow_uuid);

struct ovn_extend_table_info *
ovn_extend_table_desired_lookup_by_name(struct ovn_extend_table * table,
                                        const char *name);

struct ovn_extend_table_bucket *
ovn_extend_table_bucket_find(struct hmap *buckets, const char *key);

/* Iterates 'DESIRED' through all of the 'ovn_extend_table_info's in
 * 'TABLE'->desired that are not in 'TABLE'->existing.  (The loop body
 * presumably adds them.) */
#define EXTEND_TABLE_FOR_EACH_UNINSTALLED(DESIRED, TABLE) \
    HMAP_FOR_EACH (DESIRED, hmap_node, &(TABLE)->desired) \
        if (!ovn_extend_table_lookup(&(TABLE)->existing, DESIRED))

/* Iterates 'EXISTING' through all of the 'ovn_extend_table_info's in
 * 'TABLE'->existing that are not in 'TABLE'->desired.  (The loop body
 * presumably removes them.) */
#define EXTEND_TABLE_FOR_EACH_INSTALLED(EXISTING, TABLE)               \
    HMAP_FOR_EACH_SAFE (EXISTING, hmap_node, &(TABLE)->existing)        \
        if (!ovn_extend_table_lookup(&(TABLE)->desired, EXISTING))

#endif /* lib/extend-table.h */
