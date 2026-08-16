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

#ifndef NEIGHBOR_H
#define NEIGHBOR_H 1

#include <sys/types.h>
#include <netinet/in.h>
#include <net/if.h>
#include <stdint.h>

#include "hmapx.h"
#include "lib/sset.h"
#include "openvswitch/hmap.h"
#include "lib/ovn-util.h"

#include "vec.h"

/* XXX: AF_BRIDGE doesn't seem to be defined on some systems, e.g., OSX. */
#ifndef AF_BRIDGE
#define AF_BRIDGE AF_UNSPEC
#endif

struct tracked_lport;

enum neighbor_family {
    NEIGH_AF_INET = AF_INET,
    NEIGH_AF_INET6 = AF_INET6,
    NEIGH_AF_BRIDGE = AF_BRIDGE,
};

struct neighbor_ctx_in {
    /* Contains 'struct local_datapath'. */
    const struct hmap *local_datapaths;
    /* Index for Port Binding by Datapath. */
    struct ovsdb_idl_index *sbrec_pb_by_dp;
    /* Index for Advertised_Mac_Binding by Datapath. */
    struct ovsdb_idl_index *sbrec_amb_by_dp;
    /* Index for Port Binding by name. */
    struct ovsdb_idl_index *sbrec_pb_by_name;
    /* Index for Port Binding by datapath and tunnel key. */
    struct ovsdb_idl_index *sbrec_pb_by_key;
    /* Index for FDB by dp_key. */
    struct ovsdb_idl_index *sbrec_fdb_by_dp_key;
    const struct sbrec_chassis *chassis;
    struct evpn_local_ip_map *evpn_local_ip_map;
};

struct neighbor_ctx_out {
    /* Contains struct neighbor_interface_monitor pointers. */
    struct vector *monitored_interfaces;
    /* Contains set of port binding names that are currently advertised. */
    struct sset *advertised_pbs;
    /* Contains 'struct local_datapath' pointers for datapaths with FDB
     * advertisement enabled. */
    struct hmapx *fdb_datapaths;
    /* Contains struct neighbor_ovn_maintain_entry for VNIs that need
     * maintained interfaces.  May be NULL if caller does not need this. */
    struct vector *maintain_evpn;
};

enum neighbor_interface_type {
    NEIGH_IFACE_BRIDGE,
    NEIGH_IFACE_VXLAN,
    NEIGH_IFACE_LOOPBACK,
};

enum neighbor_mainatain_mode {
    NEIGH_MAINTAIN_NONE,
    NEIGH_MAINTAIN_MVD,
};

/* UDP destination port used for the OVN maintained VXLAN devices if
 * "dynamic-routing-maintain-evpn-vxlan-port" is not set.  It must differ
 * from the port used by the OVS datapath VXLAN device (4789), because
 * those devices are only used to exchange EVPN state with the routing
 * daemon and never carry traffic. */
#define EVPN_DEFAULT_FAKE_VXLAN_PORT 4790

/* IP/MAC to configure on the bridge interface so BGP can use it as next-hop.
 * Filled from the LRP named by dynamic-routing-evpn-lrp. */
struct neighbor_bridge_iface_config {
    bool has_addr;
    struct eth_addr lladdr;
};

struct neighbor_ovn_maintain_entry {
    enum neigh_redistribute_mode redistribute_mode;
    enum neighbor_mainatain_mode mainatain_mode;
    char vxlan_if_name[IFNAMSIZ + 1];
    char vrf_if_name[IFNAMSIZ + 1];
    char br_if_name[IFNAMSIZ + 1];
    char lo_if_name[IFNAMSIZ + 1];
    struct neighbor_bridge_iface_config br_config;
    struct in6_addr local_ip;
    uint32_t vni;
    uint16_t fake_vxlan_port;
};

struct neighbor_interface_monitor {
    enum neighbor_family family;
    char if_name[IFNAMSIZ + 1];
    enum neighbor_interface_type type;
    uint32_t vni;

    /* Contains struct advertise_neighbor_entry - the entries that OVN
     * advertises on this interface. */
    struct hmap announced_neighbors;
};

struct advertise_neighbor_entry {
    struct hmap_node node;

    struct eth_addr lladdr;
    struct in6_addr addr;   /* In case of 'dst' entries non-zero;
                             * all zero otherwise. */
};

uint32_t advertise_neigh_hash(const struct eth_addr *,
                              const struct in6_addr *);
struct advertise_neighbor_entry *advertise_neigh_find(
    const struct hmap *neighbors, struct eth_addr mac,
    const struct in6_addr *ip);
void neighbor_run(struct neighbor_ctx_in *, struct neighbor_ctx_out *);
void neighbor_cleanup(struct vector *monitored_interfaces);
bool neighbor_is_relevant_port_updated(
    struct ovsdb_idl_index *sbrec_pb_by_name,
    const struct sbrec_chassis *chassis, struct sset *advertised_pbs,
    const struct tracked_lport *lport);

#endif /* NEIGHBOR_H */
