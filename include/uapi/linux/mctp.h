/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Management Component Transport Protocol (MCTP)
 *
 * Copyright (c) 2021 Code Construct
 * Copyright (c) 2021 Google
 */

#ifndef __UAPI_MCTP_H
#define __UAPI_MCTP_H

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/netdevice.h>

typedef __u8			mctp_eid_t;

struct mctp_addr {
	mctp_eid_t		s_addr;
};

struct sockaddr_mctp {
	__kernel_sa_family_t	smctp_family;
	__u16			__smctp_pad0;
	unsigned int		smctp_network;
	struct mctp_addr	smctp_addr;
	__u8			smctp_type;
	__u8			smctp_tag;
	__u8			__smctp_pad1;
};

struct sockaddr_mctp_ext {
	struct sockaddr_mctp	smctp_base;
	int			smctp_ifindex;
	__u8			smctp_halen;
	__u8			__smctp_pad0[3];
	__u8			smctp_haddr[MAX_ADDR_LEN];
};

/* A "fully qualified" MCTP address, which includes the system-local network ID,
 * required to uniquely resolve a routable EID.
 */
struct mctp_fq_addr {
	unsigned int	net;
	mctp_eid_t	eid;
};

#define MCTP_NET_ANY		0x0

#define MCTP_ADDR_NULL		0x00
#define MCTP_ADDR_ANY		0xff

#define MCTP_TAG_MASK		0x07
#define MCTP_TAG_OWNER		0x08
#define MCTP_TAG_PREALLOC	0x10

#define MCTP_OPT_ADDR_EXT	1

#define SIOCMCTPALLOCTAG	(SIOCPROTOPRIVATE + 0)
#define SIOCMCTPDROPTAG		(SIOCPROTOPRIVATE + 1)
#define SIOCMCTPALLOCTAG2	(SIOCPROTOPRIVATE + 2)
#define SIOCMCTPDROPTAG2	(SIOCPROTOPRIVATE + 3)

/* Deprecated: use mctp_ioc_tag_ctl2 / TAG2 ioctls instead, which defines the
 * MCTP network ID as part of the allocated tag. Using this assumes the default
 * net ID for allocated tags, which may not give correct behaviour on system
 * with multiple networks configured.
 */
struct mctp_ioc_tag_ctl {
	mctp_eid_t	peer_addr;

	/* For SIOCMCTPALLOCTAG: must be passed as zero, kernel will
	 * populate with the allocated tag value. Returned tag value will
	 * always have TO and PREALLOC set.
	 *
	 * For SIOCMCTPDROPTAG: userspace provides tag value to drop, from
	 * a prior SIOCMCTPALLOCTAG call (and so must have TO and PREALLOC set).
	 */
	__u8		tag;
	__u16		flags;
};

struct mctp_ioc_tag_ctl2 {
	/* Peer details: network ID, peer EID, local EID. All set by the
	 * caller.
	 *
	 * Local EID must be MCTP_ADDR_NULL or MCTP_ADDR_ANY in current
	 * kernels.
	 */
	unsigned int	net;
	mctp_eid_t	peer_addr;
	mctp_eid_t	local_addr;

	/* Set by caller, but no flags defined currently. Must be 0 */
	__u16		flags;

	/* For SIOCMCTPALLOCTAG2: must be passed as zero, kernel will
	 * populate with the allocated tag value. Returned tag value will
	 * always have TO and PREALLOC set.
	 *
	 * For SIOCMCTPDROPTAG2: userspace provides tag value to drop, from
	 * a prior SIOCMCTPALLOCTAG2 call (and so must have TO and PREALLOC set).
	 */
	__u8		tag;

};

#endif /* __UAPI_MCTP_H */
