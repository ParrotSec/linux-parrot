// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2017 Facebook
 */
#include <stddef.h>
#include <string.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/in.h>
#include <linux/tcp.h>
#include <linux/pkt_cls.h>
#include "bpf_helpers.h"
#include "bpf_endian.h"

#define barrier() __asm__ __volatile__("": : :"memory")
int _version SEC("version") = 1;

/* llvm will optimize both subprograms into exactly the same BPF assembly
 *
 * Disassembly of section .text:
 *
 * 0000000000000000 test_pkt_access_subprog1:
 * ; 	return skb->len * 2;
 *        0:	61 10 00 00 00 00 00 00	r0 = *(u32 *)(r1 + 0)
 *        1:	64 00 00 00 01 00 00 00	w0 <<= 1
 *        2:	95 00 00 00 00 00 00 00	exit
 *
 * 0000000000000018 test_pkt_access_subprog2:
 * ; 	return skb->len * val;
 *        3:	61 10 00 00 00 00 00 00	r0 = *(u32 *)(r1 + 0)
 *        4:	64 00 00 00 01 00 00 00	w0 <<= 1
 *        5:	95 00 00 00 00 00 00 00	exit
 *
 * Which makes it an interesting test for BTF-enabled verifier.
 */
static __attribute__ ((noinline))
int test_pkt_access_subprog1(volatile struct __sk_buff *skb)
{
	return skb->len * 2;
}

static __attribute__ ((noinline))
int test_pkt_access_subprog2(int val, volatile struct __sk_buff *skb)
{
	return skb->len * val;
}

SEC("classifier/test_pkt_access")
int test_pkt_access(struct __sk_buff *skb)
{
	void *data_end = (void *)(long)skb->data_end;
	void *data = (void *)(long)skb->data;
	struct ethhdr *eth = (struct ethhdr *)(data);
	struct tcphdr *tcp = NULL;
	__u8 proto = 255;
	__u64 ihl_len;

	if (eth + 1 > data_end)
		return TC_ACT_SHOT;

	if (eth->h_proto == bpf_htons(ETH_P_IP)) {
		struct iphdr *iph = (struct iphdr *)(eth + 1);

		if (iph + 1 > data_end)
			return TC_ACT_SHOT;
		ihl_len = iph->ihl * 4;
		proto = iph->protocol;
		tcp = (struct tcphdr *)((void *)(iph) + ihl_len);
	} else if (eth->h_proto == bpf_htons(ETH_P_IPV6)) {
		struct ipv6hdr *ip6h = (struct ipv6hdr *)(eth + 1);

		if (ip6h + 1 > data_end)
			return TC_ACT_SHOT;
		ihl_len = sizeof(*ip6h);
		proto = ip6h->nexthdr;
		tcp = (struct tcphdr *)((void *)(ip6h) + ihl_len);
	}

	if (test_pkt_access_subprog1(skb) != skb->len * 2)
		return TC_ACT_SHOT;
	if (test_pkt_access_subprog2(2, skb) != skb->len * 2)
		return TC_ACT_SHOT;
	if (tcp) {
		if (((void *)(tcp) + 20) > data_end || proto != 6)
			return TC_ACT_SHOT;
		barrier(); /* to force ordering of checks */
		if (((void *)(tcp) + 18) > data_end)
			return TC_ACT_SHOT;
		if (tcp->urg_ptr == 123)
			return TC_ACT_OK;
	}

	return TC_ACT_UNSPEC;
}
