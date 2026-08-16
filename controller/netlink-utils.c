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
#include <stdbool.h>

#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>

#include "lib/netlink.h"
#include "lib/netlink-socket.h"
#include "lib/packets.h"

#include "openvswitch/ofpbuf.h"

#include "netlink-utils.h"

int32_t
nl_ifindex_get(const char *ifname)
{
    return (int32_t) if_nametoindex(ifname);
}

int
nl_create_device(const char *ifname, const char *kind)
{
    struct ifinfomsg *ifinfo;
    struct ofpbuf request;
    ofpbuf_init(&request, 0);

    nl_msg_put_nlmsghdr(&request, 0, RTM_NEWLINK,
                        NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL);
    ifinfo = ofpbuf_put_zeros(&request, sizeof *ifinfo);
    nl_msg_put_string(&request, IFLA_IFNAME, ifname);
    ifinfo->ifi_change = ifinfo->ifi_flags = IFF_UP;

    size_t linkinfo_off = nl_msg_start_nested(&request, IFLA_LINKINFO);
    nl_msg_put_string(&request, IFLA_INFO_KIND, kind);
    nl_msg_end_nested(&request, linkinfo_off);

    int err = nl_transact(NETLINK_ROUTE, &request, NULL);
    ofpbuf_uninit(&request);
    return err;
}

int
nl_create_vrf(const char *ifname, uint32_t table_id)
{
    size_t linkinfo_off, infodata_off;
    struct ifinfomsg *ifinfo;
    int err;

    struct ofpbuf request;
    uint8_t request_stub[NETNL_REQ_BUFFER_SIZE];
    ofpbuf_use_stub(&request, request_stub, sizeof(request_stub));

    nl_msg_put_nlmsghdr(&request, 0, RTM_NEWLINK,
                        NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL);
    ifinfo = ofpbuf_put_zeros(&request, sizeof *ifinfo);
    nl_msg_put_string(&request, IFLA_IFNAME, ifname);

    ifinfo->ifi_change = ifinfo->ifi_flags = IFF_UP;
    linkinfo_off = nl_msg_start_nested(&request, IFLA_LINKINFO);
    nl_msg_put_string(&request, IFLA_INFO_KIND, "vrf");
    infodata_off = nl_msg_start_nested(&request, IFLA_INFO_DATA);
    nl_msg_put_u32(&request, IFLA_VRF_TABLE, table_id);
    nl_msg_end_nested(&request, infodata_off);
    nl_msg_end_nested(&request, linkinfo_off);

    err = nl_transact(NETLINK_ROUTE, &request, NULL);

    ofpbuf_uninit(&request);
    return err;
}

int
nl_delete_device(const char *ifname)
{
    struct ifinfomsg *ifinfo;
    int err;

    struct ofpbuf request;
    uint8_t request_stub[NETNL_REQ_BUFFER_SIZE];
    ofpbuf_use_stub(&request, request_stub, sizeof(request_stub));

    nl_msg_put_nlmsghdr(&request, 0, RTM_DELLINK, NLM_F_REQUEST | NLM_F_ACK);
    ifinfo = ofpbuf_put_zeros(&request, sizeof *ifinfo);
    nl_msg_put_string(&request, IFLA_IFNAME, ifname);
    err = nl_transact(NETLINK_ROUTE, &request, NULL);

    ofpbuf_uninit(&request);
    return err;
}

