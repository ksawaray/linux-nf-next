// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *	IPV4 GSO/GRO offload support
 *	Linux INET implementation
 *
 *	UDPv4 GSO support
 */

#include <linux/skbuff.h>
#include <net/gro.h>
#include <net/gso.h>
#include <net/udp.h>
#include <net/protocol.h>
#include <net/inet_common.h>
#include <net/udp_tunnel.h>

#if IS_ENABLED(CONFIG_NET_UDP_TUNNEL)

/*
 * Dummy GRO tunnel callback, exists mainly to avoid dangling/NULL
 * values for the udp tunnel static call.
 */
static struct sk_buff *dummy_gro_rcv(struct sock *sk,
				     struct list_head *head,
				     struct sk_buff *skb)
{
	NAPI_GRO_CB(skb)->flush = 1;
	return NULL;
}

typedef struct sk_buff *(*udp_tunnel_gro_rcv_t)(struct sock *sk,
						struct list_head *head,
						struct sk_buff *skb);

struct udp_tunnel_type_entry {
	udp_tunnel_gro_rcv_t gro_receive;
	refcount_t count;
};

#define UDP_MAX_TUNNEL_TYPES (IS_ENABLED(CONFIG_GENEVE) + \
			      IS_ENABLED(CONFIG_VXLAN) * 2 + \
			      IS_ENABLED(CONFIG_NET_FOU) * 2 + \
			      IS_ENABLED(CONFIG_XFRM) * 2)

DEFINE_STATIC_CALL(udp_tunnel_gro_rcv, dummy_gro_rcv);
static DEFINE_STATIC_KEY_FALSE(udp_tunnel_static_call);
static DEFINE_MUTEX(udp_tunnel_gro_type_lock);
static struct udp_tunnel_type_entry udp_tunnel_gro_types[UDP_MAX_TUNNEL_TYPES];
static unsigned int udp_tunnel_gro_type_nr;
static DEFINE_SPINLOCK(udp_tunnel_gro_lock);

void udp_tunnel_update_gro_lookup(struct net *net, struct sock *sk, bool add)
{
	bool is_ipv6 = sk->sk_family == AF_INET6;
	struct udp_sock *tup, *up = udp_sk(sk);
	struct udp_tunnel_gro *udp_tunnel_gro;

	spin_lock(&udp_tunnel_gro_lock);
	udp_tunnel_gro = &net->ipv4.udp_tunnel_gro[is_ipv6];
	if (add)
		hlist_add_head(&up->tunnel_list, &udp_tunnel_gro->list);
	else if (up->tunnel_list.pprev)
		hlist_del_init(&up->tunnel_list);

	if (udp_tunnel_gro->list.first &&
	    !udp_tunnel_gro->list.first->next) {
		tup = hlist_entry(udp_tunnel_gro->list.first, struct udp_sock,
				  tunnel_list);

		rcu_assign_pointer(udp_tunnel_gro->sk, (struct sock *)tup);
	} else {
		RCU_INIT_POINTER(udp_tunnel_gro->sk, NULL);
	}

	spin_unlock(&udp_tunnel_gro_lock);
}
EXPORT_SYMBOL_GPL(udp_tunnel_update_gro_lookup);

void udp_tunnel_update_gro_rcv(struct sock *sk, bool add)
{
	struct udp_tunnel_type_entry *cur = NULL;
	struct udp_sock *up = udp_sk(sk);
	int i, old_gro_type_nr;

	if (!UDP_MAX_TUNNEL_TYPES || !up->gro_receive)
		return;

	mutex_lock(&udp_tunnel_gro_type_lock);

	/* Check if the static call is permanently disabled. */
	if (udp_tunnel_gro_type_nr > UDP_MAX_TUNNEL_TYPES)
		goto out;

	for (i = 0; i < udp_tunnel_gro_type_nr; i++)
		if (udp_tunnel_gro_types[i].gro_receive == up->gro_receive)
			cur = &udp_tunnel_gro_types[i];

	old_gro_type_nr = udp_tunnel_gro_type_nr;
	if (add) {
		/*
		 * Update the matching entry, if found, or add a new one
		 * if needed
		 */
		if (cur) {
			refcount_inc(&cur->count);
			goto out;
		}

		if (unlikely(udp_tunnel_gro_type_nr == UDP_MAX_TUNNEL_TYPES)) {
			pr_err_once("Too many UDP tunnel types, please increase UDP_MAX_TUNNEL_TYPES\n");
			/* Ensure static call will never be enabled */
			udp_tunnel_gro_type_nr = UDP_MAX_TUNNEL_TYPES + 1;
		} else {
			cur = &udp_tunnel_gro_types[udp_tunnel_gro_type_nr++];
			refcount_set(&cur->count, 1);
			cur->gro_receive = up->gro_receive;
		}
	} else {
		/*
		 * The stack cleanups only successfully added tunnel, the
		 * lookup on removal should never fail.
		 */
		if (WARN_ON_ONCE(!cur))
			goto out;

		if (!refcount_dec_and_test(&cur->count))
			goto out;

		/* Avoid gaps, so that the enable tunnel has always id 0 */
		*cur = udp_tunnel_gro_types[--udp_tunnel_gro_type_nr];
	}

	if (udp_tunnel_gro_type_nr == 1) {
		static_call_update(udp_tunnel_gro_rcv,
				   udp_tunnel_gro_types[0].gro_receive);
		static_branch_enable(&udp_tunnel_static_call);
	} else if (old_gro_type_nr == 1) {
		static_branch_disable(&udp_tunnel_static_call);
		static_call_update(udp_tunnel_gro_rcv, dummy_gro_rcv);
	}

out:
	mutex_unlock(&udp_tunnel_gro_type_lock);
}
EXPORT_SYMBOL_GPL(udp_tunnel_update_gro_rcv);

static struct sk_buff *udp_tunnel_gro_rcv(struct sock *sk,
					  struct list_head *head,
					  struct sk_buff *skb)
{
	if (static_branch_likely(&udp_tunnel_static_call)) {
		if (unlikely(gro_recursion_inc_test(skb))) {
			NAPI_GRO_CB(skb)->flush |= 1;
			return NULL;
		}
		return static_call(udp_tunnel_gro_rcv)(sk, head, skb);
	}
	return call_gro_receive_sk(udp_sk(sk)->gro_receive, sk, head, skb);
}

#else

static struct sk_buff *udp_tunnel_gro_rcv(struct sock *sk,
					  struct list_head *head,
					  struct sk_buff *skb)
{
	return call_gro_receive_sk(udp_sk(sk)->gro_receive, sk, head, skb);
}

#endif

static struct sk_buff *__skb_udp_tunnel_segment(struct sk_buff *skb,
	netdev_features_t features,
	struct sk_buff *(*gso_inner_segment)(struct sk_buff *skb,
					     netdev_features_t features),
	__be16 new_protocol, bool is_ipv6)
{
	int tnl_hlen = skb_inner_mac_header(skb) - skb_transport_header(skb);
	bool remcsum, need_csum, offload_csum, gso_partial;
	struct sk_buff *segs = ERR_PTR(-EINVAL);
	struct udphdr *uh = udp_hdr(skb);
	u16 mac_offset = skb->mac_header;
	__be16 protocol = skb->protocol;
	u16 mac_len = skb->mac_len;
	int udp_offset, outer_hlen;
	__wsum partial;
	bool need_ipsec;

	if (unlikely(!pskb_may_pull(skb, tnl_hlen)))
		goto out;

	/* Adjust partial header checksum to negate old length.
	 * We cannot rely on the value contained in uh->len as it is
	 * possible that the actual value exceeds the boundaries of the
	 * 16 bit length field due to the header being added outside of an
	 * IP or IPv6 frame that was already limited to 64K - 1.
	 */
	if (skb_shinfo(skb)->gso_type & SKB_GSO_PARTIAL)
		partial = (__force __wsum)uh->len;
	else
		partial = (__force __wsum)htonl(skb->len);
	partial = csum_sub(csum_unfold(uh->check), partial);

	/* setup inner skb. */
	skb->encapsulation = 0;
	SKB_GSO_CB(skb)->encap_level = 0;
	__skb_pull(skb, tnl_hlen);
	skb_reset_mac_header(skb);
	skb_set_network_header(skb, skb_inner_network_offset(skb));
	skb_set_transport_header(skb, skb_inner_transport_offset(skb));
	skb->mac_len = skb_inner_network_offset(skb);
	skb->protocol = new_protocol;

	need_csum = !!(skb_shinfo(skb)->gso_type & SKB_GSO_UDP_TUNNEL_CSUM);
	skb->encap_hdr_csum = need_csum;

	remcsum = !!(skb_shinfo(skb)->gso_type & SKB_GSO_TUNNEL_REMCSUM);
	skb->remcsum_offload = remcsum;

	need_ipsec = skb_dst(skb) && dst_xfrm(skb_dst(skb));
	/* Try to offload checksum if possible */
	offload_csum = !!(need_csum &&
			  !need_ipsec &&
			  (skb->dev->features &
			   (is_ipv6 ? (NETIF_F_HW_CSUM | NETIF_F_IPV6_CSUM) :
				      (NETIF_F_HW_CSUM | NETIF_F_IP_CSUM))));

	features &= skb->dev->hw_enc_features;
	if (need_csum)
		features &= ~NETIF_F_SCTP_CRC;

	/* The only checksum offload we care about from here on out is the
	 * outer one so strip the existing checksum feature flags and
	 * instead set the flag based on our outer checksum offload value.
	 */
	if (remcsum) {
		features &= ~NETIF_F_CSUM_MASK;
		if (!need_csum || offload_csum)
			features |= NETIF_F_HW_CSUM;
	}

	/* segment inner packet. */
	segs = gso_inner_segment(skb, features);
	if (IS_ERR_OR_NULL(segs)) {
		skb_gso_error_unwind(skb, protocol, tnl_hlen, mac_offset,
				     mac_len);
		goto out;
	}

	gso_partial = !!(skb_shinfo(segs)->gso_type & SKB_GSO_PARTIAL);

	outer_hlen = skb_tnl_header_len(skb);
	udp_offset = outer_hlen - tnl_hlen;
	skb = segs;
	do {
		unsigned int len;

		if (remcsum)
			skb->ip_summed = CHECKSUM_NONE;

		/* Set up inner headers if we are offloading inner checksum */
		if (skb->ip_summed == CHECKSUM_PARTIAL) {
			skb_reset_inner_headers(skb);
			skb->encapsulation = 1;
		}

		skb->mac_len = mac_len;
		skb->protocol = protocol;

		__skb_push(skb, outer_hlen);
		skb_reset_mac_header(skb);
		skb_set_network_header(skb, mac_len);
		skb_set_transport_header(skb, udp_offset);
		len = skb->len - udp_offset;
		uh = udp_hdr(skb);

		/* If we are only performing partial GSO the inner header
		 * will be using a length value equal to only one MSS sized
		 * segment instead of the entire frame.
		 */
		if (gso_partial && skb_is_gso(skb)) {
			uh->len = htons(skb_shinfo(skb)->gso_size +
					SKB_GSO_CB(skb)->data_offset +
					skb->head - (unsigned char *)uh);
		} else {
			uh->len = htons(len);
		}

		if (!need_csum)
			continue;

		uh->check = ~csum_fold(csum_add(partial,
				       (__force __wsum)htonl(len)));

		if (skb->encapsulation || !offload_csum) {
			uh->check = gso_make_checksum(skb, ~uh->check);
			if (uh->check == 0)
				uh->check = CSUM_MANGLED_0;
		} else {
			skb->ip_summed = CHECKSUM_PARTIAL;
			skb->csum_start = skb_transport_header(skb) - skb->head;
			skb->csum_offset = offsetof(struct udphdr, check);
		}
	} while ((skb = skb->next));
out:
	return segs;
}

struct sk_buff *skb_udp_tunnel_segment(struct sk_buff *skb,
				       netdev_features_t features,
				       bool is_ipv6)
{
	const struct net_offload __rcu **offloads;
	__be16 protocol = skb->protocol;
	const struct net_offload *ops;
	struct sk_buff *segs = ERR_PTR(-EINVAL);
	struct sk_buff *(*gso_inner_segment)(struct sk_buff *skb,
					     netdev_features_t features);

	rcu_read_lock();

	switch (skb->inner_protocol_type) {
	case ENCAP_TYPE_ETHER:
		protocol = skb->inner_protocol;
		gso_inner_segment = skb_mac_gso_segment;
		break;
	case ENCAP_TYPE_IPPROTO:
		offloads = is_ipv6 ? inet6_offloads : inet_offloads;
		ops = rcu_dereference(offloads[skb->inner_ipproto]);
		if (!ops || !ops->callbacks.gso_segment)
			goto out_unlock;
		gso_inner_segment = ops->callbacks.gso_segment;
		break;
	default:
		goto out_unlock;
	}

	segs = __skb_udp_tunnel_segment(skb, features, gso_inner_segment,
					protocol, is_ipv6);

out_unlock:
	rcu_read_unlock();

	return segs;
}
EXPORT_SYMBOL(skb_udp_tunnel_segment);

static void __udpv4_gso_segment_csum(struct sk_buff *seg,
				     __be32 *oldip, __be32 *newip,
				     __be16 *oldport, __be16 *newport)
{
	struct udphdr *uh;
	struct iphdr *iph;

	if (*oldip == *newip && *oldport == *newport)
		return;

	uh = udp_hdr(seg);
	iph = ip_hdr(seg);

	if (uh->check) {
		inet_proto_csum_replace4(&uh->check, seg, *oldip, *newip,
					 true);
		inet_proto_csum_replace2(&uh->check, seg, *oldport, *newport,
					 false);
		if (!uh->check)
			uh->check = CSUM_MANGLED_0;
	}
	*oldport = *newport;

	csum_replace4(&iph->check, *oldip, *newip);
	*oldip = *newip;
}

static struct sk_buff *__udpv4_gso_segment_list_csum(struct sk_buff *segs)
{
	struct sk_buff *seg;
	struct udphdr *uh, *uh2;
	struct iphdr *iph, *iph2;

	seg = segs;
	uh = udp_hdr(seg);
	iph = ip_hdr(seg);

	if ((udp_hdr(seg)->dest == udp_hdr(seg->next)->dest) &&
	    (udp_hdr(seg)->source == udp_hdr(seg->next)->source) &&
	    (ip_hdr(seg)->daddr == ip_hdr(seg->next)->daddr) &&
	    (ip_hdr(seg)->saddr == ip_hdr(seg->next)->saddr))
		return segs;

	while ((seg = seg->next)) {
		uh2 = udp_hdr(seg);
		iph2 = ip_hdr(seg);

		__udpv4_gso_segment_csum(seg,
					 &iph2->saddr, &iph->saddr,
					 &uh2->source, &uh->source);
		__udpv4_gso_segment_csum(seg,
					 &iph2->daddr, &iph->daddr,
					 &uh2->dest, &uh->dest);
	}

	return segs;
}

static void __udpv6_gso_segment_csum(struct sk_buff *seg,
				     struct in6_addr *oldip,
				     const struct in6_addr *newip,
				     __be16 *oldport, __be16 newport)
{
	struct udphdr *uh = udp_hdr(seg);

	if (ipv6_addr_equal(oldip, newip) && *oldport == newport)
		return;

	if (uh->check) {
		inet_proto_csum_replace16(&uh->check, seg, oldip->s6_addr32,
					  newip->s6_addr32, true);

		inet_proto_csum_replace2(&uh->check, seg, *oldport, newport,
					 false);
		if (!uh->check)
			uh->check = CSUM_MANGLED_0;
	}

	*oldip = *newip;
	*oldport = newport;
}

static struct sk_buff *__udpv6_gso_segment_list_csum(struct sk_buff *segs)
{
	const struct ipv6hdr *iph;
	const struct udphdr *uh;
	struct ipv6hdr *iph2;
	struct sk_buff *seg;
	struct udphdr *uh2;

	seg = segs;
	uh = udp_hdr(seg);
	iph = ipv6_hdr(seg);
	uh2 = udp_hdr(seg->next);
	iph2 = ipv6_hdr(seg->next);

	if (!(*(const u32 *)&uh->source ^ *(const u32 *)&uh2->source) &&
	    ipv6_addr_equal(&iph->saddr, &iph2->saddr) &&
	    ipv6_addr_equal(&iph->daddr, &iph2->daddr))
		return segs;

	while ((seg = seg->next)) {
		uh2 = udp_hdr(seg);
		iph2 = ipv6_hdr(seg);

		__udpv6_gso_segment_csum(seg, &iph2->saddr, &iph->saddr,
					 &uh2->source, uh->source);
		__udpv6_gso_segment_csum(seg, &iph2->daddr, &iph->daddr,
					 &uh2->dest, uh->dest);
	}

	return segs;
}

static struct sk_buff *__udp_gso_segment_list(struct sk_buff *skb,
					      netdev_features_t features,
					      bool is_ipv6)
{
	unsigned int mss = skb_shinfo(skb)->gso_size;

	skb = skb_segment_list(skb, features, skb_mac_header_len(skb));
	if (IS_ERR(skb))
		return skb;

	udp_hdr(skb)->len = htons(sizeof(struct udphdr) + mss);

	if (is_ipv6)
		return __udpv6_gso_segment_list_csum(skb);
	else
		return __udpv4_gso_segment_list_csum(skb);
}

struct sk_buff *__udp_gso_segment(struct sk_buff *gso_skb,
				  netdev_features_t features, bool is_ipv6)
{
	struct sock *sk = gso_skb->sk;
	unsigned int sum_truesize = 0;
	struct sk_buff *segs, *seg;
	struct udphdr *uh;
	unsigned int mss;
	bool copy_dtor;
	__sum16 check;
	__be16 newlen;
	int ret = 0;

	mss = skb_shinfo(gso_skb)->gso_size;
	if (gso_skb->len <= sizeof(*uh) + mss)
		return ERR_PTR(-EINVAL);

	if (unlikely(skb_checksum_start(gso_skb) !=
		     skb_transport_header(gso_skb) &&
		     !(skb_shinfo(gso_skb)->gso_type & SKB_GSO_FRAGLIST)))
		return ERR_PTR(-EINVAL);

	/* We don't know if egress device can segment and checksum the packet
	 * when IPv6 extension headers are present. Fall back to software GSO.
	 */
	if (gso_skb->ip_summed != CHECKSUM_PARTIAL)
		features &= ~(NETIF_F_GSO_UDP_L4 | NETIF_F_CSUM_MASK);

	if (skb_gso_ok(gso_skb, features | NETIF_F_GSO_ROBUST)) {
		/* Packet is from an untrusted source, reset gso_segs. */
		skb_shinfo(gso_skb)->gso_segs = DIV_ROUND_UP(gso_skb->len - sizeof(*uh),
							     mss);
		return NULL;
	}

	if (skb_shinfo(gso_skb)->gso_type & SKB_GSO_FRAGLIST) {
		 /* Detect modified geometry and pass those to skb_segment. */
		if (skb_pagelen(gso_skb) - sizeof(*uh) == skb_shinfo(gso_skb)->gso_size)
			return __udp_gso_segment_list(gso_skb, features, is_ipv6);

		ret = __skb_linearize(gso_skb);
		if (ret)
			return ERR_PTR(ret);

		 /* Setup csum, as fraglist skips this in udp4_gro_receive. */
		gso_skb->csum_start = skb_transport_header(gso_skb) - gso_skb->head;
		gso_skb->csum_offset = offsetof(struct udphdr, check);
		gso_skb->ip_summed = CHECKSUM_PARTIAL;

		uh = udp_hdr(gso_skb);
		if (is_ipv6)
			uh->check = ~udp_v6_check(gso_skb->len,
						  &ipv6_hdr(gso_skb)->saddr,
						  &ipv6_hdr(gso_skb)->daddr, 0);
		else
			uh->check = ~udp_v4_check(gso_skb->len,
						  ip_hdr(gso_skb)->saddr,
						  ip_hdr(gso_skb)->daddr, 0);
	}

	skb_pull(gso_skb, sizeof(*uh));

	/* clear destructor to avoid skb_segment assigning it to tail */
	copy_dtor = gso_skb->destructor == sock_wfree;
	if (copy_dtor) {
		gso_skb->destructor = NULL;
		gso_skb->sk = NULL;
	}

	segs = skb_segment(gso_skb, features);
	if (IS_ERR_OR_NULL(segs)) {
		if (copy_dtor) {
			gso_skb->destructor = sock_wfree;
			gso_skb->sk = sk;
		}
		return segs;
	}

	/* GSO partial and frag_list segmentation only requires splitting
	 * the frame into an MSS multiple and possibly a remainder, both
	 * cases return a GSO skb. So update the mss now.
	 */
	if (skb_is_gso(segs))
		mss *= skb_shinfo(segs)->gso_segs;

	seg = segs;
	uh = udp_hdr(seg);

	/* preserve TX timestamp flags and TS key for first segment */
	skb_shinfo(seg)->tskey = skb_shinfo(gso_skb)->tskey;
	skb_shinfo(seg)->tx_flags |=
			(skb_shinfo(gso_skb)->tx_flags & SKBTX_ANY_TSTAMP);

	/* compute checksum adjustment based on old length versus new */
	newlen = htons(sizeof(*uh) + mss);
	check = csum16_add(csum16_sub(uh->check, uh->len), newlen);

	for (;;) {
		if (copy_dtor) {
			seg->destructor = sock_wfree;
			seg->sk = sk;
			sum_truesize += seg->truesize;
		}

		if (!seg->next)
			break;

		uh->len = newlen;
		uh->check = check;

		if (seg->ip_summed == CHECKSUM_PARTIAL)
			gso_reset_checksum(seg, ~check);
		else
			uh->check = gso_make_checksum(seg, ~check) ? :
				    CSUM_MANGLED_0;

		seg = seg->next;
		uh = udp_hdr(seg);
	}

	/* last packet can be partial gso_size, account for that in checksum */
	newlen = htons(skb_tail_pointer(seg) - skb_transport_header(seg) +
		       seg->data_len);
	check = csum16_add(csum16_sub(uh->check, uh->len), newlen);

	uh->len = newlen;
	uh->check = check;

	if (seg->ip_summed == CHECKSUM_PARTIAL)
		gso_reset_checksum(seg, ~check);
	else
		uh->check = gso_make_checksum(seg, ~check) ? : CSUM_MANGLED_0;

	/* On the TX path, CHECKSUM_NONE and CHECKSUM_UNNECESSARY have the same
	 * meaning. However, check for bad offloads in the GSO stack expects the
	 * latter, if the checksum was calculated in software. To vouch for the
	 * segment skbs we actually need to set it on the gso_skb.
	 */
	if (gso_skb->ip_summed == CHECKSUM_NONE)
		gso_skb->ip_summed = CHECKSUM_UNNECESSARY;

	/* update refcount for the packet */
	if (copy_dtor) {
		int delta = sum_truesize - gso_skb->truesize;

		/* In some pathological cases, delta can be negative.
		 * We need to either use refcount_add() or refcount_sub_and_test()
		 */
		if (likely(delta >= 0))
			refcount_add(delta, &sk->sk_wmem_alloc);
		else
			WARN_ON_ONCE(refcount_sub_and_test(-delta, &sk->sk_wmem_alloc));
	}
	return segs;
}
EXPORT_SYMBOL_GPL(__udp_gso_segment);

static struct sk_buff *udp4_ufo_fragment(struct sk_buff *skb,
					 netdev_features_t features)
{
	struct sk_buff *segs = ERR_PTR(-EINVAL);
	unsigned int mss;
	__wsum csum;
	struct udphdr *uh;
	struct iphdr *iph;

	if (skb->encapsulation &&
	    (skb_shinfo(skb)->gso_type &
	     (SKB_GSO_UDP_TUNNEL|SKB_GSO_UDP_TUNNEL_CSUM))) {
		segs = skb_udp_tunnel_segment(skb, features, false);
		goto out;
	}

	if (!(skb_shinfo(skb)->gso_type & (SKB_GSO_UDP | SKB_GSO_UDP_L4)))
		goto out;

	if (!pskb_may_pull(skb, sizeof(struct udphdr)))
		goto out;

	if (skb_shinfo(skb)->gso_type & SKB_GSO_UDP_L4)
		return __udp_gso_segment(skb, features, false);

	mss = skb_shinfo(skb)->gso_size;
	if (unlikely(skb->len <= mss))
		goto out;

	/* Do software UFO. Complete and fill in the UDP checksum as
	 * HW cannot do checksum of UDP packets sent as multiple
	 * IP fragments.
	 */

	uh = udp_hdr(skb);
	iph = ip_hdr(skb);

	uh->check = 0;
	csum = skb_checksum(skb, 0, skb->len, 0);
	uh->check = udp_v4_check(skb->len, iph->saddr, iph->daddr, csum);
	if (uh->check == 0)
		uh->check = CSUM_MANGLED_0;

	skb->ip_summed = CHECKSUM_UNNECESSARY;

	/* If there is no outer header we can fake a checksum offload
	 * due to the fact that we have already done the checksum in
	 * software prior to segmenting the frame.
	 */
	if (!skb->encap_hdr_csum)
		features |= NETIF_F_HW_CSUM;

	/* Fragment the skb. IP headers of the fragments are updated in
	 * inet_gso_segment()
	 */
	segs = skb_segment(skb, features);
out:
	return segs;
}


#define UDP_GRO_CNT_MAX 64
static struct sk_buff *udp_gro_receive_segment(struct list_head *head,
					       struct sk_buff *skb)
{
	struct udphdr *uh = udp_gro_udphdr(skb);
	struct sk_buff *pp = NULL;
	struct udphdr *uh2;
	struct sk_buff *p;
	unsigned int ulen;
	int ret = 0;
	int flush;

	/* requires non zero csum, for symmetry with GSO */
	if (!uh->check) {
		NAPI_GRO_CB(skb)->flush = 1;
		return NULL;
	}

	/* Do not deal with padded or malicious packets, sorry ! */
	ulen = ntohs(uh->len);
	if (ulen <= sizeof(*uh) || ulen != skb_gro_len(skb)) {
		NAPI_GRO_CB(skb)->flush = 1;
		return NULL;
	}
	/* pull encapsulating udp header */
	skb_gro_pull(skb, sizeof(struct udphdr));

	list_for_each_entry(p, head, list) {
		if (!NAPI_GRO_CB(p)->same_flow)
			continue;

		uh2 = udp_hdr(p);

		/* Match ports only, as csum is always non zero */
		if ((*(u32 *)&uh->source != *(u32 *)&uh2->source)) {
			NAPI_GRO_CB(p)->same_flow = 0;
			continue;
		}

		if (NAPI_GRO_CB(skb)->is_flist != NAPI_GRO_CB(p)->is_flist) {
			NAPI_GRO_CB(skb)->flush = 1;
			return p;
		}

		flush = gro_receive_network_flush(uh, uh2, p);

		/* Terminate the flow on len mismatch or if it grow "too much".
		 * Under small packet flood GRO count could elsewhere grow a lot
		 * leading to excessive truesize values.
		 * On len mismatch merge the first packet shorter than gso_size,
		 * otherwise complete the GRO packet.
		 */
		if (ulen > ntohs(uh2->len) || flush) {
			pp = p;
		} else {
			if (NAPI_GRO_CB(skb)->is_flist) {
				if (!pskb_may_pull(skb, skb_gro_offset(skb))) {
					NAPI_GRO_CB(skb)->flush = 1;
					return NULL;
				}
				if ((skb->ip_summed != p->ip_summed) ||
				    (skb->csum_level != p->csum_level)) {
					NAPI_GRO_CB(skb)->flush = 1;
					return NULL;
				}
				ret = skb_gro_receive_list(p, skb);
			} else {
				skb_gro_postpull_rcsum(skb, uh,
						       sizeof(struct udphdr));

				ret = skb_gro_receive(p, skb);
			}
		}

		if (ret || ulen != ntohs(uh2->len) ||
		    NAPI_GRO_CB(p)->count >= UDP_GRO_CNT_MAX)
			pp = p;

		return pp;
	}

	/* mismatch, but we never need to flush */
	return NULL;
}

struct sk_buff *udp_gro_receive(struct list_head *head, struct sk_buff *skb,
				struct udphdr *uh, struct sock *sk)
{
	struct sk_buff *pp = NULL;
	struct sk_buff *p;
	struct udphdr *uh2;
	unsigned int off = skb_gro_offset(skb);
	int flush = 1;

	/* We can do L4 aggregation only if the packet can't land in a tunnel
	 * otherwise we could corrupt the inner stream. Detecting such packets
	 * cannot be foolproof and the aggregation might still happen in some
	 * cases. Such packets should be caught in udp_unexpected_gso later.
	 */
	NAPI_GRO_CB(skb)->is_flist = 0;
	if (!sk || !udp_sk(sk)->gro_receive) {
		/* If the packet was locally encapsulated in a UDP tunnel that
		 * wasn't detected above, do not GRO.
		 */
		if (skb->encapsulation)
			goto out;

		if (skb->dev->features & NETIF_F_GRO_FRAGLIST)
			NAPI_GRO_CB(skb)->is_flist = sk ? !udp_test_bit(GRO_ENABLED, sk) : 1;

		if ((!sk && (skb->dev->features & NETIF_F_GRO_UDP_FWD)) ||
		    (sk && udp_test_bit(GRO_ENABLED, sk)) || NAPI_GRO_CB(skb)->is_flist)
			return call_gro_receive(udp_gro_receive_segment, head, skb);

		/* no GRO, be sure flush the current packet */
		goto out;
	}

	if (NAPI_GRO_CB(skb)->encap_mark ||
	    (uh->check && skb->ip_summed != CHECKSUM_PARTIAL &&
	     NAPI_GRO_CB(skb)->csum_cnt == 0 &&
	     !NAPI_GRO_CB(skb)->csum_valid))
		goto out;

	/* mark that this skb passed once through the tunnel gro layer */
	NAPI_GRO_CB(skb)->encap_mark = 1;

	flush = 0;

	list_for_each_entry(p, head, list) {
		if (!NAPI_GRO_CB(p)->same_flow)
			continue;

		uh2 = (struct udphdr   *)(p->data + off);

		/* Match ports and either checksums are either both zero
		 * or nonzero.
		 */
		if ((*(u32 *)&uh->source != *(u32 *)&uh2->source) ||
		    (!uh->check ^ !uh2->check)) {
			NAPI_GRO_CB(p)->same_flow = 0;
			continue;
		}
	}

	skb_gro_pull(skb, sizeof(struct udphdr)); /* pull encapsulating udp header */
	skb_gro_postpull_rcsum(skb, uh, sizeof(struct udphdr));
	pp = udp_tunnel_gro_rcv(sk, head, skb);

out:
	skb_gro_flush_final(skb, pp, flush);
	return pp;
}
EXPORT_SYMBOL(udp_gro_receive);

static struct sock *udp4_gro_lookup_skb(struct sk_buff *skb, __be16 sport,
					__be16 dport)
{
	const struct iphdr *iph = skb_gro_network_header(skb);
	struct net *net = dev_net_rcu(skb->dev);
	struct sock *sk;
	int iif, sdif;

	sk = udp_tunnel_sk(net, false);
	if (sk && dport == htons(sk->sk_num))
		return sk;

	inet_get_iif_sdif(skb, &iif, &sdif);

	return __udp4_lib_lookup(net, iph->saddr, sport,
				 iph->daddr, dport, iif,
				 sdif, net->ipv4.udp_table, NULL);
}

INDIRECT_CALLABLE_SCOPE
struct sk_buff *udp4_gro_receive(struct list_head *head, struct sk_buff *skb)
{
	struct udphdr *uh = udp_gro_udphdr(skb);
	struct sock *sk = NULL;
	struct sk_buff *pp;

	if (unlikely(!uh))
		goto flush;

	/* Don't bother verifying checksum if we're going to flush anyway. */
	if (NAPI_GRO_CB(skb)->flush)
		goto skip;

	if (skb_gro_checksum_validate_zero_check(skb, IPPROTO_UDP, uh->check,
						 inet_gro_compute_pseudo))
		goto flush;
	else if (uh->check)
		skb_gro_checksum_try_convert(skb, IPPROTO_UDP,
					     inet_gro_compute_pseudo);
skip:
	NAPI_GRO_CB(skb)->is_ipv6 = 0;

	if (static_branch_unlikely(&udp_encap_needed_key))
		sk = udp4_gro_lookup_skb(skb, uh->source, uh->dest);

	pp = udp_gro_receive(head, skb, uh, sk);
	return pp;

flush:
	NAPI_GRO_CB(skb)->flush = 1;
	return NULL;
}

static int udp_gro_complete_segment(struct sk_buff *skb)
{
	struct udphdr *uh = udp_hdr(skb);

	skb->csum_start = (unsigned char *)uh - skb->head;
	skb->csum_offset = offsetof(struct udphdr, check);
	skb->ip_summed = CHECKSUM_PARTIAL;

	skb_shinfo(skb)->gso_segs = NAPI_GRO_CB(skb)->count;
	skb_shinfo(skb)->gso_type |= SKB_GSO_UDP_L4;

	if (skb->encapsulation)
		skb->inner_transport_header = skb->transport_header;

	return 0;
}

int udp_gro_complete(struct sk_buff *skb, int nhoff,
		     udp_lookup_t lookup)
{
	__be16 newlen = htons(skb->len - nhoff);
	struct udphdr *uh = (struct udphdr *)(skb->data + nhoff);
	struct sock *sk;
	int err;

	uh->len = newlen;

	sk = INDIRECT_CALL_INET(lookup, udp6_lib_lookup_skb,
				udp4_lib_lookup_skb, skb, uh->source, uh->dest);
	if (sk && udp_sk(sk)->gro_complete) {
		skb_shinfo(skb)->gso_type = uh->check ? SKB_GSO_UDP_TUNNEL_CSUM
					: SKB_GSO_UDP_TUNNEL;

		/* clear the encap mark, so that inner frag_list gro_complete
		 * can take place
		 */
		NAPI_GRO_CB(skb)->encap_mark = 0;

		/* Set encapsulation before calling into inner gro_complete()
		 * functions to make them set up the inner offsets.
		 */
		skb->encapsulation = 1;
		err = udp_sk(sk)->gro_complete(sk, skb,
				nhoff + sizeof(struct udphdr));
	} else {
		err = udp_gro_complete_segment(skb);
	}

	if (skb->remcsum_offload)
		skb_shinfo(skb)->gso_type |= SKB_GSO_TUNNEL_REMCSUM;

	return err;
}
EXPORT_SYMBOL(udp_gro_complete);

INDIRECT_CALLABLE_SCOPE int udp4_gro_complete(struct sk_buff *skb, int nhoff)
{
	const u16 offset = NAPI_GRO_CB(skb)->network_offsets[skb->encapsulation];
	const struct iphdr *iph = (struct iphdr *)(skb->data + offset);
	struct udphdr *uh = (struct udphdr *)(skb->data + nhoff);

	/* do fraglist only if there is no outer UDP encap (or we already processed it) */
	if (NAPI_GRO_CB(skb)->is_flist && !NAPI_GRO_CB(skb)->encap_mark) {
		uh->len = htons(skb->len - nhoff);

		skb_shinfo(skb)->gso_type |= (SKB_GSO_FRAGLIST|SKB_GSO_UDP_L4);
		skb_shinfo(skb)->gso_segs = NAPI_GRO_CB(skb)->count;

		__skb_incr_checksum_unnecessary(skb);

		return 0;
	}

	if (uh->check)
		uh->check = ~udp_v4_check(skb->len - nhoff, iph->saddr,
					  iph->daddr, 0);

	return udp_gro_complete(skb, nhoff, udp4_lib_lookup_skb);
}

int __init udpv4_offload_init(void)
{
	net_hotdata.udpv4_offload = (struct net_offload) {
		.callbacks = {
			.gso_segment = udp4_ufo_fragment,
			.gro_receive  =	udp4_gro_receive,
			.gro_complete =	udp4_gro_complete,
		},
	};

	return inet_add_offload(&net_hotdata.udpv4_offload, IPPROTO_UDP);
}
