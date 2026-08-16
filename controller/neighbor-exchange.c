/* Copyright (c) 2025, Red Hat, Inc.
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

#include <errno.h>
#include <linux/neighbour.h>
#include <net/if.h>

#include "host-if-monitor.h"
#include "lib/sset.h"
#include "neighbor.h"
#include "neighbor-exchange.h"
#include "neighbor-exchange-netlink.h"
#include "route-exchange-netlink.h"
#include "openvswitch/poll-loop.h"
#include "openvswitch/vlog.h"
#include "ovn-util.h"
#include "packets.h"
#include "vec.h"
#include "unixctl.h"

VLOG_DEFINE_THIS_MODULE(neighbor_exchange);

static uint32_t evpn_remote_vtep_hash(const struct in6_addr *ip,
                                      uint16_t port, uint32_t vni);
static void evpn_remote_vtep_add(struct hmap *remote_vteps, struct in6_addr ip,
                                 uint16_t port, uint32_t vni);
static struct evpn_remote_vtep *evpn_remote_vtep_find(
    const struct hmap *remote_vteps, const struct in6_addr *ip,
    uint16_t port, uint32_t vni);
static void evpn_static_entry_add(struct hmap *static_entries,
                                  struct eth_addr mac, struct in6_addr ip,
                                  uint32_t vni, uint32_t nh_id);
static struct evpn_static_entry *evpn_static_entry_find(
    const struct hmap *static_entries, struct eth_addr mac,
    struct in6_addr ip, uint32_t vni, uint32_t nh_id);
static uint32_t evpn_static_entry_hash(const struct eth_addr *mac,
                                       const struct in6_addr *ip,
                                       uint32_t vni, uint32_t nh_id);

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);

/* Last neighbor_exchange netlink operation. */
static int neighbor_exchange_nl_status;

#define CLEAR_NEIGHBOR_EXCHANGE_NL_STATUS() \
    do {                                    \
        neighbor_exchange_nl_status = 0;    \
    } while (0)

#define SET_NEIGHBOR_EXCHANGE_NL_STATUS(error)     \
    do {                                           \
        if (!neighbor_exchange_nl_status) {        \
            neighbor_exchange_nl_status = (error); \
            if (error) {                           \
                poll_immediate_wake();             \
            }                                      \
        }                                          \
    } while (0)

/* Tracks the EVPN interface stack of a single VNI.
 *
 * Presence in 'maintained_evpn_entries' means "OVN may have created kernel
 * devices for this VNI and is therefore responsible for tearing them down".
 * An entry is inserted before the first netlink call is issued, so that a
 * stack that was only partially created is still cleaned up later. */
struct maintained_evpn_entry {
    struct hmap_node node;
    /* The configuration the devices were last (re)created with, i.e. what
     * the kernel is expected to hold.  It is updated as soon as the
     * previous devices are gone, even if creating the new ones then fails,
     * so that a partial failure doesn't leave a permanent difference
     * against the desired configuration. */
    struct neighbor_ovn_maintain_entry entry;
    /* False if the last attempt to apply 'entry' didn't fully succeed.  The
     * next run then repairs the stack by re-running the idempotent create
     * path instead of tearing it down and rebuilding it. */
    bool synced;
};

static struct hmap maintained_evpn_entries =
    HMAP_INITIALIZER(&maintained_evpn_entries);

static struct maintained_evpn_entry *
maintained_evpn_entry_find(const struct hmap *entries, uint32_t vni)
{
    struct maintained_evpn_entry *me;
    HMAP_FOR_EACH_WITH_HASH (me, node, hash_int(vni, 0), entries) {
        if (me->entry.vni == vni) {
            return me;
        }
    }

    return NULL;
}

/* Deletes the interfaces for 'entry'. Returns true if all of them were
 * deleted (or were already absent), false if any netlink call failed. */
static bool
evpn_delete_devices(struct neighbor_ovn_maintain_entry *entry)
{
    int error;
    bool ok = true;

    error = ne_nl_delete_iface(entry->br_if_name);
    if (error && error != ENODEV) {
        VLOG_WARN_RL(&rl, "Unable to delete bridge interface %s: %s",
                     entry->br_if_name, ovs_strerror(error));
        SET_NEIGHBOR_EXCHANGE_NL_STATUS(error);
        ok = false;
    }

    error = ne_nl_delete_iface(entry->vxlan_if_name);
    if (error && error != ENODEV) {
        VLOG_WARN_RL(&rl, "Unable to delete VXLAN interface %s: %s",
                     entry->vxlan_if_name, ovs_strerror(error));
        SET_NEIGHBOR_EXCHANGE_NL_STATUS(error);
        ok = false;
    }

    error = ne_nl_delete_iface(entry->vrf_if_name);
    if (error && error != ENODEV) {
        VLOG_WARN_RL(&rl, "Unable to delete vrf interface %s: %s",
                     entry->vrf_if_name, ovs_strerror(error));
        SET_NEIGHBOR_EXCHANGE_NL_STATUS(error);
        ok = false;
    }

    error = ne_nl_delete_iface(entry->lo_if_name);
    if (error && error != ENODEV) {
        VLOG_WARN_RL(&rl, "Unable to delete dummy interface %s: %s",
                     entry->lo_if_name, ovs_strerror(error));
        SET_NEIGHBOR_EXCHANGE_NL_STATUS(error);
        ok = false;
    }

    /* Refresh the host-if-monitor ifindex cache for the interfaces we
     * monitor for neighbor sync, so a lookup later in this same engine
     * iteration doesn't use a stale ifindex. */
    host_if_monitor_invalidate(entry->br_if_name);
    host_if_monitor_invalidate(entry->vxlan_if_name);
    host_if_monitor_invalidate(entry->lo_if_name);

    return ok;
}

static bool
set_bridge_evpn_device_addr(struct neighbor_ovn_maintain_entry *entry)
{
    int err;
    if (entry->br_config.has_addr) {
        err = ne_nl_set_iface_mac_addr(entry->br_if_name,
                                       &entry->br_config.lladdr);
        if (err) {
            VLOG_WARN_RL(&rl, "Unable set mac address on interface %s: %s",
                         entry->br_if_name, ovs_strerror(err));
            SET_NEIGHBOR_EXCHANGE_NL_STATUS(err);
            return false;
        }
    }

    return true;
}

/* Creates the interfaces for 'entry'. Returns true if the whole stack was
 * created successfully (or already existed), false if any netlink call
 * failed. */
static bool
evpn_create_devices(struct neighbor_ovn_maintain_entry *entry)
{
    static const char *link_dev = "vxlan_sys_4789";
    int32_t link_ifindex = ne_nl_ifindex_get(link_dev);
    int err;
    bool ok = true;

    if (!link_ifindex) {
        err = ENODEV;
        VLOG_WARN_RL(&rl,
                     "Unable to find OVS system vxlan interface");
        SET_NEIGHBOR_EXCHANGE_NL_STATUS(err);
        ok = false;
    }

    if (entry->br_config.has_addr
        && nrm_mode_IP_is_set(entry->redistribute_mode)) {
        err = ne_nl_create_vrf(entry->vrf_if_name, entry->vni);
        if (err && err != EEXIST) {
            VLOG_WARN_RL(&rl,
                         "Unable to create VRF %s for datapath %s",
                         entry->vrf_if_name, ovs_strerror(err));
            SET_NEIGHBOR_EXCHANGE_NL_STATUS(err);
            ok = false;
        }
    }

    err = ne_nl_create_vxlan(entry->vxlan_if_name, entry->vni,
                             &entry->local_ip, entry->fake_vxlan_port,
                             link_ifindex);
    if (err && err != EEXIST) {
        VLOG_WARN_RL(&rl,
                     "Unable to create VXLAN interface %s for "
                     "VNI %"PRIu32": %s",
                     entry->vxlan_if_name, entry->vni, ovs_strerror(err));
        SET_NEIGHBOR_EXCHANGE_NL_STATUS(err);
        ok = false;
    }

    err = ne_nl_create_bridge(entry->br_if_name);
    if (err && err != EEXIST) {
        VLOG_WARN_RL(&rl,
                     "Unable to create bridge interface %s for "
                     "VNI %"PRIu32": %s",
                     entry->br_if_name, entry->vni, ovs_strerror(err));
        SET_NEIGHBOR_EXCHANGE_NL_STATUS(err);
        ok = false;
    }

    if (!set_bridge_evpn_device_addr(entry)) {
        ok = false;
    }

    if (entry->br_config.has_addr
        && nrm_mode_IP_is_set(entry->redistribute_mode)) {
        err = ne_nl_set_master(entry->br_if_name, entry->vrf_if_name);
        if (err) {
            VLOG_WARN_RL(&rl, "Unable to enslave %s to bridge %s: %s",
                         entry->br_if_name, entry->vrf_if_name,
                         ovs_strerror(err));
            SET_NEIGHBOR_EXCHANGE_NL_STATUS(err);
            ok = false;
        }
    }

    if (nrm_mode_FDB_is_set(entry->redistribute_mode)) {
        err = ne_nl_create_lo(entry->lo_if_name);
        if (err && err != EEXIST) {
            VLOG_WARN_RL(&rl,
                         "Unable to create dummy interface %s for "
                         "VNI %"PRIu32": %s",
                         entry->lo_if_name, entry->vni, ovs_strerror(err));
            SET_NEIGHBOR_EXCHANGE_NL_STATUS(err);
            ok = false;
        }
        err = ne_nl_set_master(entry->lo_if_name, entry->br_if_name);
        if (err) {
            VLOG_WARN_RL(&rl, "Unable to enslave %s to bridge %s: %s",
                         entry->lo_if_name, entry->br_if_name,
                         ovs_strerror(err));
            SET_NEIGHBOR_EXCHANGE_NL_STATUS(err);
            ok = false;
        }
    }

    err = ne_nl_set_master(entry->vxlan_if_name, entry->br_if_name);
    if (err) {
        VLOG_WARN_RL(&rl, "Unable to enslave %s to bridge %s: %s",
                     entry->vxlan_if_name, entry->br_if_name,
                     ovs_strerror(err));
        SET_NEIGHBOR_EXCHANGE_NL_STATUS(err);
        ok = false;
    }

    /* Refresh the host-if-monitor ifindex cache so a lookup later in this
     * same engine iteration picks up the newly (re)created interfaces
     * instead of a stale ifindex left over from before they were
     * deleted/recreated. */
    host_if_monitor_invalidate(entry->br_if_name);
    host_if_monitor_invalidate(entry->vxlan_if_name);
    host_if_monitor_invalidate(entry->lo_if_name);

    return ok;
}

/* Returns true if a change between 'old_entry' and 'entry' requires
 * deleting and recreating the whole VRF/bridge/VXLAN/lo interface stack,
 * rather than updating it in place.  This is the case when:
 *
 * - The VXLAN device's local IP or UDP destination port changed: Netlink
 *   offers no way to change either on an existing VXLAN interface.
 *
 * - The bridge gained or lost its L3 address ('has_addr'): this changes
 *   whether a VRF is needed at all.
 *
 * - The redistribution mode changed (FDB and/or IP bits): this changes
 *   whether the loopback advertise interface is needed at all.
 *
 * Recreating the whole stack in these cases is simpler and less
 * error-prone than trying to patch each interface individually. */
static bool
should_destroy_interfaces(const struct neighbor_ovn_maintain_entry *entry,
                          const struct neighbor_ovn_maintain_entry *old_entry)
{
    return !ipv6_addr_equals(&old_entry->local_ip, &entry->local_ip) ||
           old_entry->fake_vxlan_port != entry->fake_vxlan_port ||
           old_entry->br_config.has_addr != entry->br_config.has_addr ||
           old_entry->redistribute_mode != entry->redistribute_mode ||
           (entry->br_config.has_addr
           && !eth_addr_equals(old_entry->br_config.lladdr,
                               entry->br_config.lladdr));
}

/* Brings the kernel devices of a VNI in line with the desired configuration
 * 'entry', recording in 'me' what was actually applied.  'has_old' tells
 * whether 'me->entry' describes devices we created in a previous run. */
static void
maintain_evpn_devices(const struct neighbor_ovn_maintain_entry *entry,
                      struct maintained_evpn_entry *me, bool has_old)
{
    if (has_old && should_destroy_interfaces(entry, &me->entry)) {
        if (!evpn_delete_devices(&me->entry)) {
            /* The old devices are (partly) still around, so 'me->entry'
             * keeps describing them and the next run retries the
             * teardown. */
            me->synced = false;
            return;
        }
    } else if (me->synced) {
        /* Nothing changed and the stack was fully applied. */
        return;
    }

    /* Either there were no devices yet, or the old ones are now gone: from
     * here on 'entry' is what the kernel is expected to hold, even if the
     * creation below only partially succeeds.  Otherwise the difference
     * against 'entry' would never go away and we would tear the stack down
     * and rebuild it on every single run. */
    me->entry = *entry;
    me->synced = evpn_create_devices(&me->entry);
}

static void
neighbor_exchange_maintain_evpn_run(const struct vector *maintain_evpn)
{
    struct hmap old_maintained_evpn_entries =
        HMAP_INITIALIZER(&old_maintained_evpn_entries);
    hmap_swap(&maintained_evpn_entries, &old_maintained_evpn_entries);

    struct neighbor_ovn_maintain_entry *entry;
    VECTOR_FOR_EACH_PTR (maintain_evpn, entry) {
        struct maintained_evpn_entry *me =
            maintained_evpn_entry_find(&old_maintained_evpn_entries,
                                        entry->vni);
        bool has_old = me != NULL;

        if (has_old) {
            hmap_remove(&old_maintained_evpn_entries, &me->node);
        } else {
            me = xzalloc(sizeof *me);
        }

        /* Start tracking the VNI before issuing any netlink call to be
         * responsible for deleting/updating its devices */
        hmap_insert(&maintained_evpn_entries, &me->node,
                    hash_int(entry->vni, 0));

        maintain_evpn_devices(entry, me, has_old);
    }

    struct maintained_evpn_entry *stale_me;
    HMAP_FOR_EACH_POP (stale_me, node, &old_maintained_evpn_entries) {
        if (evpn_delete_devices(&stale_me->entry)) {
            free(stale_me);
        } else {
            /* Deletion failed: keep tracking it so it's retried as stale
             * again on the next run (it's no longer in 'maintain_evpn').
             * Some devices may already be gone, so if the VNI comes back
             * before the teardown completes, the stack has to be repaired
             * rather than assumed to be in shape. */
            stale_me->synced = false;
            hmap_insert(&maintained_evpn_entries, &stale_me->node,
                        hash_int(stale_me->entry.vni, 0));
        }
    }

    hmap_destroy(&old_maintained_evpn_entries);
}

void
neighbor_exchange_maintain_evpn_cleanup_all(void)
{
    struct maintained_evpn_entry *me;
    HMAP_FOR_EACH (me, node, &maintained_evpn_entries) {
        evpn_delete_devices(&me->entry);
    }
}

void
neighbor_exchange_maintain_evpn_destroy(void)
{
    struct maintained_evpn_entry *me;
    HMAP_FOR_EACH_POP (me, node, &maintained_evpn_entries) {
        free(me);
    }
    hmap_destroy(&maintained_evpn_entries);
}

void
neighbor_exchange_run(const struct neighbor_exchange_ctx_in *n_ctx_in,
                      struct neighbor_exchange_ctx_out *n_ctx_out)
{
    /* Reset once for the whole run: errors hit while maintaining the EVPN
     * devices must survive into neighbor_exchange_status_run(), otherwise
     * the engine is never woken up to retry a failed or partial apply. */
    CLEAR_NEIGHBOR_EXCHANGE_NL_STATUS();

    neighbor_exchange_maintain_evpn_run(n_ctx_in->maintain_evpn);

    struct neighbor_interface_monitor *nim;

    struct sset if_names = SSET_INITIALIZER(&if_names);
    VECTOR_FOR_EACH (n_ctx_in->monitored_interfaces, nim) {
        sset_add(&if_names, nim->if_name);
    }
    host_if_monitor_update_watches(&if_names);
    sset_destroy(&if_names);

    VECTOR_FOR_EACH (n_ctx_in->monitored_interfaces, nim) {
        int32_t if_index = host_if_monitor_ifname_toindex(nim->if_name);

        if (!if_index) {
            continue;
        }

        struct vector received_neighbors =
            VECTOR_EMPTY_INITIALIZER(struct ne_nl_received_neigh);
        SET_NEIGHBOR_EXCHANGE_NL_STATUS(
            ne_nl_sync_neigh(nim->family, if_index, &nim->announced_neighbors,
                             &received_neighbors)
        );

        struct ne_nl_received_neigh *ne;
        switch (nim->type) {
        case NEIGH_IFACE_BRIDGE:
            VECTOR_FOR_EACH_PTR (&received_neighbors, ne) {
                if (ne_is_valid_static_arp(ne)) {
                    if (!evpn_static_entry_find(n_ctx_out->static_arps,
                                                ne->lladdr, ne->addr,
                                                nim->vni, 0)) {
                        evpn_static_entry_add(n_ctx_out->static_arps,
                                              ne->lladdr, ne->addr,
                                              nim->vni, 0);
                    }
                }
            }
            break;
        case NEIGH_IFACE_VXLAN:
            VECTOR_FOR_EACH_PTR (&received_neighbors, ne) {
                if (ne_is_valid_remote_vtep(ne)) {
                    uint16_t port = ne->port ? ne->port : DEFAULT_VXLAN_PORT;
                    if (!evpn_remote_vtep_find(n_ctx_out->remote_vteps,
                                               &ne->addr, port, nim->vni)) {
                        evpn_remote_vtep_add(n_ctx_out->remote_vteps, ne->addr,
                                             port, nim->vni);
                    }
                }

                if (ne_is_valid_static_fdb(ne)) {
                    if (!evpn_static_entry_find(n_ctx_out->static_fdbs,
                                                ne->lladdr, ne->addr,
                                                nim->vni, ne->nh_id)) {
                        evpn_static_entry_add(n_ctx_out->static_fdbs,
                                              ne->lladdr, ne->addr,
                                              nim->vni, ne->nh_id);
                    }
                }
            }
            break;
        case NEIGH_IFACE_LOOPBACK:
            /* No learning from the loopback interface required. */
            break;
        }

        vector_push(n_ctx_out->neighbor_table_watches, &if_index);
        vector_destroy(&received_neighbors);
    }
}

int
neighbor_exchange_status_run(void)
{
    return neighbor_exchange_nl_status;
}

void
evpn_remote_vteps_clear(struct hmap *remote_vteps)
{
    struct evpn_remote_vtep *vtep;
    HMAP_FOR_EACH_POP (vtep, hmap_node, remote_vteps) {
        free(vtep);
    }
}

void
evpn_remote_vtep_list(struct unixctl_conn *conn, int argc OVS_UNUSED,
                      const char *argv[] OVS_UNUSED, void *data_)
{
    struct hmap *remote_vteps = data_;
    struct ds ds = DS_EMPTY_INITIALIZER;

    struct evpn_remote_vtep *vtep;
    HMAP_FOR_EACH (vtep, hmap_node, remote_vteps) {
        ds_put_cstr(&ds, "IP: ");
        ipv6_format_mapped(&vtep->ip, &ds);
        ds_put_format(&ds, ", port: %"PRIu16", vni: %"PRIu32"\n",
                      vtep->port, vtep->vni);
    }

    unixctl_command_reply(conn, ds_cstr_ro(&ds));
    ds_destroy(&ds);
}

void
evpn_static_entries_clear(struct hmap *static_entries)
{
    struct evpn_static_entry *e;
    HMAP_FOR_EACH_POP (e, hmap_node, static_entries) {
        free(e);
    }
}

static void
evpn_remote_vtep_add(struct hmap *remote_vteps, struct in6_addr ip,
                     uint16_t port, uint32_t vni)
{
    struct evpn_remote_vtep *vtep = xmalloc(sizeof *vtep);
    *vtep = (struct evpn_remote_vtep) {
        .ip = ip,
        .port = port,
        .vni = vni,
    };

    hmap_insert(remote_vteps, &vtep->hmap_node,
                evpn_remote_vtep_hash(&ip, port, vni));
}

static struct evpn_remote_vtep *
evpn_remote_vtep_find(const struct hmap *remote_vteps,
                      const struct in6_addr *ip,
                      uint16_t port, uint32_t vni)
{
    uint32_t hash = evpn_remote_vtep_hash(ip, port, vni);

    struct evpn_remote_vtep *vtep;
    HMAP_FOR_EACH_WITH_HASH (vtep, hmap_node, hash, remote_vteps) {
        if (ipv6_addr_equals(&vtep->ip, ip) &&
            vtep->port == port && vtep->vni == vni) {
            return vtep;
        }
    }

    return NULL;
}

static uint32_t
evpn_remote_vtep_hash(const struct in6_addr *ip, uint16_t port,
                      uint32_t vni)
{
    uint32_t hash = 0;
    hash = hash_add_in6_addr(hash, ip);
    hash = hash_add(hash, port);
    hash = hash_add(hash, vni);

    return hash_finish(hash, 14);
}

static void
evpn_static_entry_add(struct hmap *static_entries, struct eth_addr mac,
                      struct in6_addr ip, uint32_t vni, uint32_t nh_id)
{
    struct evpn_static_entry *e = xmalloc(sizeof *e);
    *e = (struct evpn_static_entry) {
        .mac = mac,
        .ip = ip,
        .vni = vni,
        .nh_id = nh_id,
    };

    hmap_insert(static_entries, &e->hmap_node,
                evpn_static_entry_hash(&mac, &ip, vni, nh_id));
}

static struct evpn_static_entry *
evpn_static_entry_find(const struct hmap *static_entries, struct eth_addr mac,
                       struct in6_addr ip, uint32_t vni, uint32_t nh_id)
{
    uint32_t hash = evpn_static_entry_hash(&mac, &ip, vni, nh_id);

    struct evpn_static_entry *e;
    HMAP_FOR_EACH_WITH_HASH (e, hmap_node, hash, static_entries) {
        if (eth_addr_equals(e->mac, mac) &&
            ipv6_addr_equals(&e->ip, &ip) &&
            e->vni == vni &&
            e->nh_id == nh_id) {
            return e;
        }
    }

    return NULL;
}

static uint32_t
evpn_static_entry_hash(const struct eth_addr *mac, const struct in6_addr *ip,
                       uint32_t vni, uint32_t nh_id)
{
    uint32_t hash = 0;
    hash = hash_bytes(mac, sizeof *mac, hash);
    hash = hash_add_in6_addr(hash, ip);
    hash = hash_add(hash, vni);
    hash = hash_add(hash, nh_id);

    return hash_finish(hash, 30);
}
