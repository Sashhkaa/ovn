/*
 * Copyright (c) 2025 Canonical, Ltd.
 * Copyright (c) 2025, STACKIT GmbH & Co. KG
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

#ifndef NETLINK_UTILS_H
#define NETLINK_UTILS_H 1

#include <stdint.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <net/if.h>

#define NETNL_REQ_BUFFER_SIZE 128

#define TABLE_ID_VALID(table_id) (table_id != RT_TABLE_UNSPEC &&              \
                                  table_id != RT_TABLE_COMPAT &&              \
                                  table_id != RT_TABLE_LOCAL &&               \
                                  table_id != RT_TABLE_MAX)

int32_t nl_ifindex_get(const char *ifname);
int nl_create_device(const char *ifname, const char *kind);
int nl_create_vrf(const char *ifname, uint32_t table_id);
int nl_delete_device(const char *ifname);
int nl_set_iface_mac(const char *ifname,
                     const struct eth_addr *mac);
int nl_set_master(const char *slave, const char *master);

#endif /* netlink-utils.h */
