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

#include <config.h>
#include <string.h>

#include "extend-table.h"
#include "hash.h"
#include "id-pool.h"
#include "lib/uuid.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(extend_table);

static void
ovn_extend_table_delete_desired(struct ovn_extend_table *table,
                                struct ovn_extend_table_lflow_to_desired *l);

void
ovn_extend_table_init(struct ovn_extend_table *table, const char *table_name,
                      uint32_t n_ids)
{
    /* Table id 0 is invalid, set id-pool base to 1. */
    uint32_t meter_base = 1;

    n_ids = MIN(n_ids, UINT32_MAX - meter_base);

    *table = (struct ovn_extend_table) {
        .name = xstrdup(table_name),
        .n_ids = n_ids,
        .table_ids = id_pool_create(meter_base, n_ids),
        .desired = HMAP_INITIALIZER(&table->desired),
        .lflow_to_desired = HMAP_INITIALIZER(&table->lflow_to_desired),
        .existing = HMAP_INITIALIZER(&table->existing),
    };
}

void
ovn_extend_table_reinit(struct ovn_extend_table *table, uint32_t n_ids)
{
    /* Table id 0 is invalid, set id-pool base to 1. */
    uint32_t meter_base = 1;

    n_ids = MIN(n_ids, UINT32_MAX - meter_base);

    if (n_ids != table->n_ids) {
        ovn_extend_table_clear(table, true);
        id_pool_destroy(table->table_ids);
        table->table_ids = id_pool_create(meter_base, n_ids);
        table->n_ids = n_ids;
    }
}

static struct ovn_extend_table_info *
ovn_extend_table_info_alloc(const char *name, uint32_t id,
                            struct ovn_extend_table_info *peer,
                            uint32_t hash)
{
    struct ovn_extend_table_info *e = xmalloc(sizeof *e);
    e->name = xstrdup(name);
    e->table_id = id;
    e->peer = peer;
    if (peer) {
        peer->peer = e;
    }
    e->hmap_node.hash = hash;
    hmap_init(&e->references);
    e->group_header = NULL;
    hmap_init(&e->desired_buckets);
    hmap_init(&e->existing_buckets);
    return e;
}

/* Frees every bucket in 'buckets' (either a desired_buckets or
 * existing_buckets hmap belonging to one info object), unlinking each
 * bucket's cross-side peer (in the *other* info object of the pair) so it
 * doesn't retain a dangling pointer. Only safe to use when the whole
 * info object owning 'buckets' is being torn down (or its peer is being
 * torn down in the same pass) — ovn_extend_table_assign_group_id() has its
 * own, more selective replacement logic (ovn_extend_table_desired_buckets_
 * replace()) because it only replaces desired_buckets in place while
 * existing_buckets survives. */
static void
ovn_extend_table_buckets_clear(struct hmap *buckets)
{
    struct ovn_extend_table_bucket *b;
    HMAP_FOR_EACH_SAFE (b, hmap_node, buckets) {
        hmap_remove(buckets, &b->hmap_node);
        if (b->peer) {
            b->peer->peer = NULL;
        }
        free(b->key);
        free(b->content);
        free(b);
    }
}

/* Replaces 'info->desired_buckets' with 'new_desired'. Entries from the old
 * desired_buckets that were carried over into 'new_desired' (same backend
 * key) already had their existing_buckets peer link redirected to the new
 * entry by ovn_extend_table_bucket_alloc(); entries that were dropped still
 * point their peer back at themselves, so unlink those before freeing to
 * avoid leaving a dangling pointer on the existing_buckets side. */
static void
ovn_extend_table_desired_buckets_replace(struct ovn_extend_table_info *info,
                                         struct hmap *new_desired)
{
    struct ovn_extend_table_bucket *old;
    HMAP_FOR_EACH_SAFE (old, hmap_node, &info->desired_buckets) {
        hmap_remove(&info->desired_buckets, &old->hmap_node);
        if (old->peer && old->peer->peer == old) {
            old->peer->peer = NULL;
        }
        free(old->key);
        free(old->content);
        free(old);
    }
    hmap_destroy(&info->desired_buckets);
    info->desired_buckets = *new_desired;
}

static void
ovn_extend_table_info_destroy(struct ovn_extend_table_info *e)
{
    free(e->name);
    struct ovn_extend_table_lflow_ref *r;
    HMAP_FOR_EACH_SAFE (r, hmap_node, &e->references) {
        hmap_remove(&e->references, &r->hmap_node);
        ovs_list_remove(&r->list_node);
        free(r);
    }
    hmap_destroy(&e->references);

    free(e->group_header);
    ovn_extend_table_buckets_clear(&e->desired_buckets);
    hmap_destroy(&e->desired_buckets);
    ovn_extend_table_buckets_clear(&e->existing_buckets);
    hmap_destroy(&e->existing_buckets);

    free(e);
}

/* Finds and returns a group_info in 'existing' whose key is identical
 * to 'target''s key, or NULL if there is none. */
struct ovn_extend_table_info *
ovn_extend_table_lookup(struct hmap *exisiting,
                        const struct ovn_extend_table_info *target)
{
    struct ovn_extend_table_info *e;

    HMAP_FOR_EACH_WITH_HASH (e, hmap_node, target->hmap_node.hash,
                             exisiting) {
        if (e->table_id == target->table_id) {
            return e;
        }
   }
    return NULL;
}

static struct ovn_extend_table_lflow_to_desired *
ovn_extend_table_find_desired_by_lflow(struct ovn_extend_table *table,
                                       const struct uuid *lflow_uuid)
{
    struct ovn_extend_table_lflow_to_desired *l;
    HMAP_FOR_EACH_WITH_HASH (l, hmap_node, uuid_hash(lflow_uuid),
                             &table->lflow_to_desired) {
        if (uuid_equals(&l->lflow_uuid, lflow_uuid)) {
            return l;
        }
    }
    return NULL;
}

/* Add a reference to the list of items that <lflow_uuid> uses.
 * If the <lflow_uuid> entry doesn't exist in lflow_to_desired mapping, add
 * the <lflow_uuid> entry first. */
static void
ovn_extend_table_add_desired_to_lflow(struct ovn_extend_table *table,
                                      const struct uuid *lflow_uuid,
                                      struct ovn_extend_table_lflow_ref *r)
{
    struct ovn_extend_table_lflow_to_desired *l =
        ovn_extend_table_find_desired_by_lflow(table, lflow_uuid);
    if (!l) {
        l = xmalloc(sizeof *l);
        l->lflow_uuid = *lflow_uuid;
        ovs_list_init(&l->desired);
        hmap_insert(&table->lflow_to_desired, &l->hmap_node,
                    uuid_hash(lflow_uuid));
        VLOG_DBG("%s: table %s: add new lflow_to_desired entry "UUID_FMT,
                 __func__, table->name, UUID_ARGS(lflow_uuid));
    }

    ovs_list_insert(&l->desired, &r->list_node);
    VLOG_DBG("%s: table %s: lflow "UUID_FMT" use new item %s, id %"PRIu32,
             __func__, table->name, UUID_ARGS(lflow_uuid), r->desired->name,
             r->desired->table_id);
}

static struct ovn_extend_table_lflow_ref *
ovn_extend_info_find_lflow_ref(struct ovn_extend_table_info *e,
                               const struct uuid *lflow_uuid)
{
    struct ovn_extend_table_lflow_ref *r;
    HMAP_FOR_EACH_WITH_HASH (r, hmap_node, uuid_hash(lflow_uuid),
                             &e->references) {
        if (uuid_equals(&r->lflow_uuid, lflow_uuid)) {
            return r;
        }
    }
    return NULL;
}

/* Create the cross reference between <e> and <lflow_uuid> */
static void
ovn_extend_info_add_lflow_ref(struct ovn_extend_table *table,
                              struct ovn_extend_table_info *e,
                              const struct uuid *lflow_uuid)
{
    struct ovn_extend_table_lflow_ref *r =
        ovn_extend_info_find_lflow_ref(e, lflow_uuid);
    if (!r) {
        r = xmalloc(sizeof *r);
        r->lflow_uuid = *lflow_uuid;
        r->desired = e;
        hmap_insert(&e->references, &r->hmap_node, uuid_hash(lflow_uuid));

        ovn_extend_table_add_desired_to_lflow(table, lflow_uuid, r);
    }
}

static void
ovn_extend_info_del_lflow_ref(struct ovn_extend_table *table,
                              struct ovn_extend_table_lflow_ref *r)
{
    VLOG_DBG("%s: table %s: name %s, lflow "UUID_FMT" n %"PRIuSIZE, __func__,
             table->name, r->desired->name, UUID_ARGS(&r->lflow_uuid),
             hmap_count(&r->desired->references));
    hmap_remove(&r->desired->references, &r->hmap_node);
    ovs_list_remove(&r->list_node);
    free(r);
}

/* Clear either desired or existing in ovn_extend_table. */
void
ovn_extend_table_clear(struct ovn_extend_table *table, bool existing)
{
    struct ovn_extend_table_info *g;
    struct hmap *target = existing ? &table->existing : &table->desired;

    /* Clear lflow_to_desired index, if the target is desired table. */
    if (!existing) {
        struct ovn_extend_table_lflow_to_desired *l;
        HMAP_FOR_EACH_SAFE (l, hmap_node, &table->lflow_to_desired) {
            ovn_extend_table_delete_desired(table, l);
        }
    }

    /* Clear the target table. */
    HMAP_FOR_EACH_SAFE (g, hmap_node, target) {
        hmap_remove(target, &g->hmap_node);
        if (g->peer) {
            g->peer->peer = NULL;
        } else {
            /* Unset the id because the peer is deleted already. */
            id_pool_free_id(table->table_ids, g->table_id);
        }
        ovn_extend_table_info_destroy(g);
    }
}

void
ovn_extend_table_destroy(struct ovn_extend_table *table)
{
    ovn_extend_table_clear(table, false);
    hmap_destroy(&table->desired);
    hmap_destroy(&table->lflow_to_desired);
    ovn_extend_table_clear(table, true);
    hmap_destroy(&table->existing);
    id_pool_destroy(table->table_ids);
    free(table->name);
}

/* Remove an entry from existing table */
void
ovn_extend_table_remove_existing(struct ovn_extend_table *table,
                                 struct ovn_extend_table_info *existing)
{
    /* Remove 'existing' from 'table->existing' */
    hmap_remove(&table->existing, &existing->hmap_node);

    if (existing->peer) {
        existing->peer->peer = NULL;
    } else {
        /* Dealloc the ID. */
        id_pool_free_id(table->table_ids, existing->table_id);
    }
    ovn_extend_table_info_destroy(existing);
}

static void
ovn_extend_table_delete_desired(struct ovn_extend_table *table,
                                struct ovn_extend_table_lflow_to_desired *l)
{
    hmap_remove(&table->lflow_to_desired, &l->hmap_node);
    struct ovn_extend_table_lflow_ref *r;
    LIST_FOR_EACH_SAFE (r, list_node, &l->desired) {
        struct ovn_extend_table_info *e = r->desired;
        ovn_extend_info_del_lflow_ref(table, r);
        if (hmap_is_empty(&e->references)) {
            VLOG_DBG("%s: table %s: %s, "UUID_FMT, __func__,
                     table->name, e->name, UUID_ARGS(&l->lflow_uuid));
            hmap_remove(&table->desired, &e->hmap_node);
            if (e->peer) {
                e->peer->peer = NULL;
            } else {
                id_pool_free_id(table->table_ids, e->table_id);
            }
            ovn_extend_table_info_destroy(e);
        }
    }
    free(l);
}

/* Remove entries in desired table that are created by the lflow_uuid */
void
ovn_extend_table_remove_desired(struct ovn_extend_table *table,
                                const struct uuid *lflow_uuid)
{
    struct ovn_extend_table_lflow_to_desired *l =
        ovn_extend_table_find_desired_by_lflow(table, lflow_uuid);

    if (!l) {
        return;
    }

    ovn_extend_table_delete_desired(table, l);
}

static struct ovn_extend_table_bucket *
ovn_extend_table_bucket_lookup(struct hmap *buckets, const char *key,
                               uint32_t hash)
{
    struct ovn_extend_table_bucket *b;
    HMAP_FOR_EACH_WITH_HASH (b, hmap_node, hash, buckets) {
        if (!strcmp(b->key, key)) {
            return b;
        }
    }
    return NULL;
}

static struct ovn_extend_table_bucket *
ovn_extend_table_bucket_alloc(const char *key, uint32_t id,
                              const char *content,
                              struct ovn_extend_table_bucket *peer,
                              uint32_t hash)
{
    struct ovn_extend_table_bucket *b = xmalloc(sizeof *b);
    b->key = xstrdup(key);
    b->bucket_id = id;
    b->content = xstrdup(content);
    b->peer = peer;
    if (peer) {
        peer->peer = b;
    }
    b->hmap_node.hash = hash;
    return b;
}

/* Reconciles 'desired's bucket set into 'existing's, mirroring what the
 * caller (ofctrl.c) has just told the switch via INSERT_BUCKET/
 * REMOVE_BUCKET. Only called for entries created through
 * ovn_extend_table_assign_group_id() ('desired->group_header' set). */
static void
ovn_extend_table_sync_buckets(struct ovn_extend_table_info *desired,
                              struct ovn_extend_table_info *existing)
{
    /* Drop existing buckets that are no longer desired. */
    struct ovn_extend_table_bucket *b;
    HMAP_FOR_EACH_SAFE (b, hmap_node, &existing->existing_buckets) {
        uint32_t hash = hash_string(b->key, 0);
        if (!ovn_extend_table_bucket_lookup(&desired->desired_buckets,
                                            b->key, hash)) {
            hmap_remove(&existing->existing_buckets, &b->hmap_node);
            if (b->peer) {
                b->peer->peer = NULL;
            }
            free(b->key);
            free(b->content);
            free(b);
        }
    }

    /* Add or refresh existing buckets to match what's desired. */
    struct ovn_extend_table_bucket *d;
    HMAP_FOR_EACH (d, hmap_node, &desired->desired_buckets) {
        uint32_t hash = hash_string(d->key, 0);
        struct ovn_extend_table_bucket *e =
            ovn_extend_table_bucket_lookup(&existing->existing_buckets,
                                           d->key, hash);
        if (e) {
            /* Refresh content in case e.g. weight/health status changed. */
            free(e->content);
            e->content = xstrdup(d->content);
            e->peer = d;
            d->peer = e;
        } else {
            struct ovn_extend_table_bucket *new_e =
                ovn_extend_table_bucket_alloc(d->key, d->bucket_id,
                                              d->content, d, hash);
            hmap_insert(&existing->existing_buckets, &new_e->hmap_node, hash);
        }
    }
}

void
ovn_extend_table_sync(struct ovn_extend_table *table)
{
    struct ovn_extend_table_info *desired;

    /* Copy the contents of desired to existing. */
    HMAP_FOR_EACH_SAFE (desired, hmap_node, &table->desired) {
        struct ovn_extend_table_info *existing =
            ovn_extend_table_lookup(&table->existing, desired);
        if (!existing) {
            existing = ovn_extend_table_info_alloc(desired->name,
                                                    desired->table_id,
                                                    desired,
                                                    desired->hmap_node.hash);
            hmap_insert(&table->existing, &existing->hmap_node,
                        existing->hmap_node.hash);
        }

        if (desired->group_header) {
            free(existing->group_header);
            existing->group_header = xstrdup(desired->group_header);
            ovn_extend_table_sync_buckets(desired, existing);
        }
    }
}

/* Assign a new table ID for the table information from the ID pool.
 * If it already exists, return the old ID. */
uint32_t
ovn_extend_table_assign_id(struct ovn_extend_table *table, const char *name,
                           struct uuid lflow_uuid)
{
    uint32_t table_id = 0, hash;
    struct ovn_extend_table_info *table_info, *existing_info;

    hash = hash_string(name, 0);

    /* Check whether we have non installed but allocated group_id. */
    HMAP_FOR_EACH_WITH_HASH (table_info, hmap_node, hash, &table->desired) {
        if (!strcmp(table_info->name, name)) {
            VLOG_DBG("ovn_extend_table_assign_id: table %s: "
                     "reuse old id %"PRIu32" for %s, used by lflow "UUID_FMT,
                     table->name, table_info->table_id, table_info->name,
                     UUID_ARGS(&lflow_uuid));
            ovn_extend_info_add_lflow_ref(table, table_info, &lflow_uuid);
            return table_info->table_id;
        }
    }

    /* Check whether we already have an installed entry for this
     * combination. */
    existing_info = NULL;
    HMAP_FOR_EACH_WITH_HASH (table_info, hmap_node, hash, &table->existing) {
        if (!strcmp(table_info->name, name)) {
            existing_info = table_info;
            table_id = existing_info->table_id;
            break;
        }
    }

    if (!existing_info) {
        /* Reserve a new id. */
        if (!id_pool_alloc_id(table->table_ids, &table_id)) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);

            VLOG_ERR_RL(&rl, "table %s: out of table ids.", table->name);
            return EXT_TABLE_ID_INVALID;
        }
    }

    table_info = ovn_extend_table_info_alloc(name, table_id, existing_info,
                                             hash);

    hmap_insert(&table->desired,
                &table_info->hmap_node, table_info->hmap_node.hash);

    ovn_extend_info_add_lflow_ref(table, table_info, &lflow_uuid);

    return table_id;
}

static bool
ovn_extend_table_bucket_id_used(struct hmap *buckets, uint32_t id)
{
    struct ovn_extend_table_bucket *b;
    HMAP_FOR_EACH (b, hmap_node, buckets) {
        if (b->bucket_id == id) {
            return true;
        }
    }
    return false;
}

/* OpenFlow 1.5 reserves 0xfffffffd..0xffffffff (OFPG15_BUCKET_FIRST/LAST/
 * ALL); stay well clear of that range. */
#define OVN_BUCKET_ID_MASK 0x7fffffff

/* Like ovn_extend_table_assign_id(), but keyed by 'group_key' alone (not
 * the bucket content), so the returned table_id stays stable while
 * 'buckets' changes across calls. Replaces the entry's desired bucket set
 * with 'buckets' every call; ofctrl.c diffs that against existing_buckets
 * to emit incremental INSERT_BUCKET/REMOVE_BUCKET group_mods. */
uint32_t
ovn_extend_table_assign_group_id(struct ovn_extend_table *table,
                                 const char *group_key,
                                 const char *group_header,
                                 const struct ovn_extend_table_bucket_spec *buckets,
                                 size_t n_buckets, struct uuid lflow_uuid)
{
    uint32_t table_id = ovn_extend_table_assign_id(table, group_key,
                                                    lflow_uuid);
    if (table_id == EXT_TABLE_ID_INVALID) {
        return table_id;
    }

    struct ovn_extend_table_info *info =
        ovn_extend_table_desired_lookup_by_name(table, group_key);
    ovs_assert(info);

    free(info->group_header);
    info->group_header = xstrdup(group_header);

    /* 'existing_buckets' (what's actually installed on the switch) lives on
     * 'info's existing-side peer object, not on 'info' itself — 'info' is
     * always the desired-side object here (just (re)assigned above by
     * ovn_extend_table_assign_id()). No peer yet means nothing has been
     * synced to the switch for this group_key so far. */
    struct hmap *existing_buckets =
        info->peer ? &info->peer->existing_buckets : NULL;

    struct hmap new_desired = HMAP_INITIALIZER(&new_desired);
    for (size_t i = 0; i < n_buckets; i++) {
        const struct ovn_extend_table_bucket_spec *spec = &buckets[i];
        uint32_t hash = hash_string(spec->key, 0);

        /* Reuse the id (and existing-side peer link, if any) already
         * associated with this backend key, whether it's still sitting in
         * the outgoing desired_buckets or the entry was recreated this run
         * and only survives on the existing (installed) side. Otherwise
         * derive an id deterministically from the key and resolve
         * collisions against ids already in use within this one group. */
        struct ovn_extend_table_bucket *old =
            ovn_extend_table_bucket_lookup(&info->desired_buckets,
                                           spec->key, hash);
        struct ovn_extend_table_bucket *existing =
            (!old && existing_buckets)
            ? ovn_extend_table_bucket_lookup(existing_buckets,
                                             spec->key, hash)
            : NULL;
        struct ovn_extend_table_bucket *peer =
            old ? old->peer : existing;

        uint32_t bucket_id;
        if (old) {
            bucket_id = old->bucket_id;
        } else if (existing) {
            bucket_id = existing->bucket_id;
        } else {
            bucket_id = hash & OVN_BUCKET_ID_MASK;
            while (ovn_extend_table_bucket_id_used(&new_desired, bucket_id)
                   || (existing_buckets &&
                       ovn_extend_table_bucket_id_used(existing_buckets,
                                                       bucket_id))) {
                bucket_id = (bucket_id + 1) & OVN_BUCKET_ID_MASK;
            }
        }

        struct ovn_extend_table_bucket *b = ovn_extend_table_bucket_alloc(
            spec->key, bucket_id, spec->content, peer, hash);
        hmap_insert(&new_desired, &b->hmap_node, hash);
    }

    ovn_extend_table_desired_buckets_replace(info, &new_desired);

    return table_id;
}

struct ovn_extend_table_info *
ovn_extend_table_desired_lookup_by_name(struct ovn_extend_table * table,
                                        const char *name)
{
    uint32_t hash = hash_string(name, 0);
    struct ovn_extend_table_info *m_desired;
    HMAP_FOR_EACH_WITH_HASH (m_desired, hmap_node, hash, &table->desired) {
        if (!strcmp(m_desired->name, name)) {
            return m_desired;
        }
    }
    return NULL;
}
