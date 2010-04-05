#ifndef __NETCHANNEL2_H__
#define __NETCHANNEL2_H__

#include <xen/interface/io/uring.h>

/* Tell the other end how many packets its allowed to have
 * simultaneously outstanding for transmission.	 An endpoint must not
 * send PACKET messages which would take it over this limit.
 *
 * The SET_MAX_PACKETS message must be sent before any PACKET
 * messages.  It should only be sent once, unless the ring is
 * disconnected and reconnected.
 */
#define NETCHANNEL2_MSG_SET_MAX_PACKETS 1
struct netchannel2_msg_set_max_packets {
	struct netchannel2_msg_hdr hdr;
	uint32_t max_outstanding_packets;
};

/* Pass a packet to the other end.  The packet consists of a header,
 * followed by a bunch of fragment descriptors, followed by an inline
 * packet prefix.  Every fragment descriptor in a packet must be the
 * same type, and the type is determined by the header.	 The receiving
 * endpoint should respond with a finished_packet message as soon as
 * possible.  The prefix may be no more than
 * NETCHANNEL2_MAX_INLINE_BYTES.  Packets may contain no more than
 * NETCHANNEL2_MAX_PACKET_BYTES bytes of data, including all fragments
 * and the prefix.
 *
 * If a SET_MAX_FRAGMENTS_PER_PACKET message has been received, the
 * number of fragments in the packet should respect that limit.
 * Otherwise, there should be at most one fragment in the packet
 * (there may be zero if the entire packet fits in the inline prefix).
 */
#define NETCHANNEL2_MSG_PACKET 2
#define NETCHANNEL2_MAX_PACKET_BYTES 65536
#define NETCHANNEL2_MAX_INLINE_BYTES 256
struct netchannel2_fragment {
	uint16_t size;
	/* The offset is always relative to the start of the page.
	   For pre_posted packet types, it is not relative to the
	   start of the buffer (although the fragment range will
	   obviously be within the buffer range). */
	uint16_t off;
	union {
		struct {
			grant_ref_t gref;
		} receiver_copy;
		struct {
			grant_ref_t gref;
		} receiver_map;
	};
};
struct netchannel2_msg_packet {
	struct netchannel2_msg_hdr hdr;
	uint32_t id; /* Opaque ID which is echoed into the finished
			packet message. */
	uint8_t type;
	uint8_t flags;
	uint8_t segmentation_type;
	uint8_t pad;
	uint16_t prefix_size;
	uint16_t mss;
	uint16_t csum_start;
	uint16_t csum_offset;
	/* Variable-size array.  The number of elements is determined
	   by the size of the message. */
	struct netchannel2_fragment frags[0];
};

/* TX csum offload.  The transmitting domain has skipped a checksum
 * calculation.  Before forwarding the packet on, the receiving domain
 * must first perform a 16 bit IP checksum on everything from
 * csum_start to the end of the packet, and then write the result to
 * an offset csum_offset in the packet.  This should only be set if
 * the transmitting domain has previously received a SET_OFFLOAD
 * message with csum = 1.
 */
#define NC2_PACKET_FLAG_csum_blank 1
/* RX csum offload.  The transmitting domain has already validated the
 * protocol-level checksum on this packet (i.e. TCP or UDP), so the
 * receiving domain shouldn't bother.  This does not tell you anything
 * about the IP-level checksum.  This can be set on any packet,
 * regardless of any SET_OFFLOAD messages which may or may not have
 * been sent. */
#define NC2_PACKET_FLAG_data_validated 2
/* If set, this flag indicates that this packet could have used a
 * bypass if one had been available, and so it should be sent to the
 * autobypass state machine.
 */
#define NC2_PACKET_FLAG_bypass_candidate 4
/* If set, the transmitting domain requires an event urgently when
 * this packet's finish message is sent.  Otherwise, the event can be
 * delayed. */
#define NC2_PACKET_FLAG_need_event 8

/* The mechanism which should be used to receive the data part of
 * a packet:
 *
 * receiver_copy -- The transmitting domain has granted the receiving
 *		    domain access to the original RX buffers using
 *		    copy-only grant references.	 The receiving domain
 *		    should copy the data out of the buffers and issue
 *		    a FINISH message.
 *
 *		    Due to backend bugs, it is in not safe to use this
 *		    packet type except on bypass rings.
 *
 * receiver_map -- The transmitting domain has granted the receiving
 *                 domain access to the original RX buffers using
 *                 full (mappable) grant references.  This can be
 *                 treated the same way as receiver_copy, but the
 *                 receiving domain also has the option of mapping
 *                 the fragments, rather than copying them.  If it
 *                 decides to do so, it should ensure that the fragments
 *                 will be unmapped in a reasonably timely fashion,
 *                 and don't e.g. become stuck in a receive buffer
 *                 somewhere.  In general, anything longer than about
 *                 a second is likely to cause problems.  Once all
 *                 grant references have been unmapper, the receiving
 *                 domain should send a FINISH message.
 *
 *                 This packet type may not be used on bypass rings.
 *
 * small -- The packet does not have any fragment descriptors
 *	    (i.e. the entire thing is inline in the ring).  The receiving
 *	    domain should simply the copy the packet out of the ring
 *	    into a locally allocated buffer.  No FINISH message is required
 *	    or allowed.
 *
 *	    This packet type may be used on any ring.
 *
 * All endpoints must be able to receive all packet types, but note
 * that it is correct to treat receiver_map and small packets as
 * receiver_copy ones. */
#define NC2_PACKET_TYPE_receiver_copy 1
#define NC2_PACKET_TYPE_receiver_map 3
#define NC2_PACKET_TYPE_small 4

#define NC2_PACKET_SEGMENTATION_TYPE_none  0
#define NC2_PACKET_SEGMENTATION_TYPE_tcpv4 1

/* Tell the other end that we're finished with a message it sent us,
   and it can release the transmit buffers etc.	 This must be sent in
   response to receiver_copy and receiver_map packets.	It must not be
   sent in response to pre_posted or small packets. */
#define NETCHANNEL2_MSG_FINISH_PACKET 3
struct netchannel2_msg_finish_packet {
	struct netchannel2_msg_hdr hdr;
	uint32_t id;
};

/* Tell the other end what sort of offloads we're going to let it use.
 * An endpoint must not use any offload unless it has been enabled
 * by a previous SET_OFFLOAD message. */
/* Note that there is no acknowledgement for this message.  This means
 * that an endpoint can continue to receive PACKET messages which
 * require offload support for some time after it disables task
 * offloading.  The endpoint is expected to handle this case correctly
 * (which may just mean dropping the packet and returning a FINISH
 * message, if appropriate).
 */
#define NETCHANNEL2_MSG_SET_OFFLOAD 4
struct netchannel2_msg_set_offload {
	struct netchannel2_msg_hdr hdr;
	/* Checksum offload.  If this is 0, the other end must
	 * calculate checksums before sending the packet.  If it is 1,
	 * the other end does not have to perform the calculation.
	 */
	uint8_t csum;
	/* Segmentation offload.  If this is 0, the other end must not
	 * generate any packet messages with a segmentation type other
	 * than NC2_PACKET_SEGMENTATION_TYPE_none.  If it is 1, the
	 * other end may also generate packets with a type of
	 * NC2_PACKET_SEGMENTATION_TYPE_tcpv4.
	 */
	uint8_t tcpv4_segmentation_offload;
	uint16_t reserved;
};

/* Set the maximum number of fragments which can be used in any packet
 * (not including the inline prefix).  Until this is sent, there can
 * be at most one such fragment per packet.  The maximum must not be
 * set to zero. */
/* Note that there is no acknowledgement for this message, and so if
 * an endpoint tries to reduce the number of fragments then it may
 * continue to recieve over-fragmented packets for some time.  The
 * receiving endpoint is expected to deal with this.
 */
#define NETCHANNEL2_MSG_SET_MAX_FRAGMENTS_PER_PACKET 5
struct netchannel2_msg_set_max_fragments_per_packet {
	struct netchannel2_msg_hdr hdr;
	uint32_t max_frags_per_packet;
};

/* Attach to a bypass ring as a frontend.  The receiving domain should
 * map the bypass ring (which will be in the sending domain's memory)
 * and attach to it in the same as it attached to the original ring.
 * This bypass ring will, once it's been successfully set up, be used
 * for all packets destined for @remote_mac (excluding broadcasts).
 *
 * @ring_domid indicates which domain allocated the ring pages, and
 * hence which domain should be specified when grant mapping
 * @control_gref, @prod_gref, and @cons_gref.  It can be set to
 * DOMID_SELF, in which case the domain ID of the domain sending the
 * message should be used.
 *
 * @peer_domid indicates the domain ID of the domain on the other end
 * of the ring.
 *
 * @handle gives a unique handle for the bypass which will be used in
 * future messages.
 *
 * @peer_trusted is true if the peer should be trusted by the domain
 * which sent the bypass message.
 *
 * @ring_pages gives the number of valid grefs in the @prod_grefs and
 * @cons_grefs arrays.
 *
 * @is_backend_like indicates which ring attach the receiving domain
 * should use.  If @is_backend_like is set, the receiving domain
 * should interpret the control area as a netchannel2_backend_shared.
 * Otherwise, it's a netchannel2_frontend_shared.  Also, a
 * backend-like endpoint should receive an event channel from the peer
 * domain, while a frontend-like one should send one.  Once
 * established, the ring is symmetrical.
 *
 *
 * BYPASS messages can only be sent by a trusted endpoint.  They may
 * not be sent over bypass rings.
 *
 * No packets may be sent over the ring until a READY message is
 * received.  Until that point, all packets must be sent over the
 * parent ring.
 */
struct netchannel2_msg_bypass_common {
	uint16_t ring_domid;
	uint16_t peer_domid;
	uint32_t handle;

	uint8_t remote_mac[6];
	uint8_t peer_trusted;
	uint8_t ring_pages;

	uint32_t control_gref;
	uint32_t pad;

	/* Followed by a run of @ring_pages uint32_t producer ring
	   grant references, then a run of @ring_pages uint32_t
	   consumer ring grant references */
};

#define NETCHANNEL2_MSG_BYPASS_FRONTEND 9
struct netchannel2_msg_bypass_frontend {
	struct netchannel2_msg_hdr hdr;
	uint32_t pad;
	struct netchannel2_msg_bypass_common common;
};

#define NETCHANNEL2_MSG_BYPASS_BACKEND 10
struct netchannel2_msg_bypass_backend {
	struct netchannel2_msg_hdr hdr;
	uint32_t port;
	struct netchannel2_msg_bypass_common common;
};

#define NETCHANNEL2_MSG_BYPASS_FRONTEND_READY 11
struct netchannel2_msg_bypass_frontend_ready {
	struct netchannel2_msg_hdr hdr;
	int32_t port;
};

/* This message is sent on a bypass ring once the sending domain is
 * ready to receive packets.  Until it has been received, the bypass
 * ring cannot be used to transmit packets.  It may only be sent once.
 *
 * Note that it is valid to send packet messages before *sending* a
 * BYPASS_READY message, provided a BYPASS_READY message has been
 * *received*.
 *
 * This message can only be sent on a bypass ring.
 */
#define NETCHANNEL2_MSG_BYPASS_READY 12
struct netchannel2_msg_bypass_ready {
	struct netchannel2_msg_hdr hdr;
	uint32_t pad;
};

/* Disable an existing bypass.	This is sent over the *parent* ring,
 * in the same direction as the original BYPASS message, when the
 * bypassed domain wishes to disable the ring.	The receiving domain
 * should stop sending PACKET messages over the ring, wait for FINISH
 * messages for any outstanding PACKETs, and then acknowledge this
 * message with a DISABLED message.
 *
 * This message may not be sent on bypass rings.
 */
#define NETCHANNEL2_MSG_BYPASS_DISABLE 13
struct netchannel2_msg_bypass_disable {
	struct netchannel2_msg_hdr hdr;
	uint32_t handle;
};
#define NETCHANNEL2_MSG_BYPASS_DISABLED 14
struct netchannel2_msg_bypass_disabled {
	struct netchannel2_msg_hdr hdr;
	uint32_t handle;
};

/* Detach from an existing bypass.  This is sent over the *parent* in
 * the same direction as the original BYPASS message, when the
 * bypassed domain wishes to destroy the ring.	The receiving domain
 * should immediately unmap the ring and respond with a DETACHED
 * message.  Any PACKET messages which haven't already received a
 * FINISH message are dropped.
 *
 * During a normal shutdown, this message will be sent after DISABLED
 * messages have been received from both endpoints.  However, it can
 * also be sent without a preceding DISABLE message if the other
 * endpoint appears to be misbehaving or has crashed.
 *
 * This message may not be sent on bypass rings.
 */
#define NETCHANNEL2_MSG_BYPASS_DETACH 15
struct netchannel2_msg_bypass_detach {
	struct netchannel2_msg_hdr hdr;
	uint32_t handle;
};
#define NETCHANNEL2_MSG_BYPASS_DETACHED 16
struct netchannel2_msg_bypass_detached {
	struct netchannel2_msg_hdr hdr;
	uint32_t handle;
};

#define NETCHANNEL2_MSG_SUGGEST_BYPASS 17
struct netchannel2_msg_suggest_bypass {
	struct netchannel2_msg_hdr hdr;
	unsigned char mac[6];
	uint16_t pad1;
	uint32_t pad2;
};

#endif /* !__NETCHANNEL2_H__ */
