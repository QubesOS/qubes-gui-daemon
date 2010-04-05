#ifndef __XEN_PUBLIC_IO_URING_H__
#define __XEN_PUBLIC_IO_URING_H__

#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/system.h>

typedef unsigned RING_IDX;

#define NETCHANNEL2_MSG_PAD 255

/* The sring structures themselves.	 The _cons and _prod variants are
   different views of the same bit of shared memory, and are supposed
   to provide better checking of the expected use patterns.	 Fields in
   the shared ring are owned by either the producer end or the
   consumer end.  If a field is owned by your end, the other end will
   never modify it.	 If it's owned by the other end, the other end is
   allowed to modify it whenever it likes, and you can never do so.

   Fields owned by the other end are always const (because you can't
   change them).  They're also volatile, because there are a bunch
   of places where we go:

   local_x = sring->x;
   validate(local_x);
   use(local_x);

   and it would be very bad if the compiler turned that into:

   local_x = sring->x;
   validate(sring->x);
   use(local_x);

   because that contains a potential TOCTOU race (hard to exploit, but
   still present).	The compiler is only allowed to do that
   optimisation because it knows that local_x == sring->x at the start
   of the call to validate(), and it only knows that if it can reorder
   the read of sring->x over the sequence point at the end of the
   first statement.	 In other words, it can only do the bad
   optimisation if it knows that reads of sring->x are side-effect
   free.  volatile stops it from making that assumption.

   We don't need a full memory barrier here, because it's sufficient
   to copy the volatile data into stable guest-local storage, and
   volatile achieves that.	i.e. we don't need local_x to be precisely
   sring->x, but we do need it to be a stable snapshot of some
   previous valud of sring->x.

   Note that there are still plenty of other places where we *do* need
   full barriers.  volatile just deals with this one, specific, case.

   We could also deal with it by putting compiler barriers in all over
   the place.  The downside of that approach is that you need to put
   the barrier()s in lots of different places (basically, everywhere
   which needs to access these fields), and it's easy to forget one.
   barrier()s also have somewhat heavier semantics than volatile
   (because they prevent all reordering, rather than just reordering
   on this one field), although that's pretty much irrelevant because
   gcc usually treats pretty much any volatile access as a call to
   barrier().
*/

/* Messages are sent over sring pairs.	Each sring in a pair provides
 * a unidirectional byte stream which can generate events when either
 * the producer or consumer pointers cross a particular threshold.
 *
 * We define both sring_prod and sring_cons structures.	 The two
 * structures will always map onto the same physical bytes in memory,
 * but they provide different views of that memory which are
 * appropriate to either producers or consumers.
 *
 * Obviously, the endpoints need to agree on which end produces
 * messages on which ring.	The endpoint which provided the memory
 * backing the ring always produces on the first sring, and the one
 * which just mapped the ring produces on the second.  By convention,
 * these are known as the frontend and backend, respectively.
 */

/* For both rings, the producer (consumer) pointers point at the
 * *next* byte which is going to be produced (consumed).  An endpoint
 * must generate an event on the event channel port if it moves the
 * producer pointer (consumer pointer) across prod_event (cons_event).
 *
 * i.e if an endpoint ever updates a pointer so that the old pointer
 * is strictly less than the event, and the new pointer is greater
 * than or equal to the event then the remote must be notified.	 If
 * the pointer overflows the ring, treat the new value as if it were
 * (actual new value) + (1 << 32).
 */
struct netchannel2_sring_prod {
	RING_IDX prod;
	volatile const RING_IDX cons;
	volatile const RING_IDX prod_event;
	RING_IDX cons_event;
	unsigned char pad[48];
};

struct netchannel2_sring_cons {
	volatile const RING_IDX prod;
	RING_IDX cons;
	RING_IDX prod_event;
	volatile const RING_IDX cons_event;
	unsigned char pad[48];
};

struct netchannel2_frontend_shared {
	struct netchannel2_sring_prod prod;
	struct netchannel2_sring_cons cons;
};

struct netchannel2_backend_shared {
	struct netchannel2_sring_cons cons;
	struct netchannel2_sring_prod prod;
};

struct netchannel2_prod_ring {
	struct netchannel2_sring_prod *sring;
	void *payload;
	RING_IDX prod_pvt;
	/* This is the number of bytes available after prod_pvt last
	   time we checked, minus the number of bytes which we've
	   consumed since then.	 It's used to a avoid a bunch of
	   memory barriers when checking for ring space. */
	unsigned bytes_available;
	/* Number of bytes reserved by nc2_reserve_payload_bytes() */
	unsigned reserve;
	size_t payload_bytes;
};

struct netchannel2_cons_ring {
	struct netchannel2_sring_cons *sring;
	const volatile void *payload;
	RING_IDX cons_pvt;
	size_t payload_bytes;
};

/* A message header.  There is one of these at the start of every
 * message.	 @type is one of the #define's below, and @size is the
 * size of the message, including the header and any padding.
 * size should be a multiple of 8 so we avoid unaligned memory copies.
 * structs defining message formats should have sizes multiple of 8
 * bytes and should use paddding fields if needed.
 */
struct netchannel2_msg_hdr {
	uint8_t type;
	uint8_t flags;
	uint16_t size;
};

/* Copy some bytes from the shared ring to a stable local buffer,
 * starting at the private consumer pointer.  Does not update the
 * private consumer pointer.
 */
static inline void nc2_copy_from_ring_off(struct netchannel2_cons_ring *ring,
					  void *buf,
					  size_t nbytes,
					  unsigned off)
{
	unsigned start, end;

	start = (ring->cons_pvt + off) & (ring->payload_bytes-1);
	end = (ring->cons_pvt + nbytes + off) & (ring->payload_bytes-1);
	/* We cast away the volatile modifier to get rid of an
	   irritating compiler warning, and compensate with a
	   barrier() at the end. */
	memcpy(buf, (const void *)ring->payload + start, nbytes);
	barrier();
}

static inline void nc2_copy_from_ring(struct netchannel2_cons_ring *ring,
				      void *buf,
				      size_t nbytes)
{
	nc2_copy_from_ring_off(ring, buf, nbytes, 0);
}


/* Copy some bytes to the shared ring, starting at the private
 * producer pointer.  Does not update the private pointer.
 */
static inline void nc2_copy_to_ring_off(struct netchannel2_prod_ring *ring,
					const void *src,
					unsigned nr_bytes,
					unsigned off)
{
	unsigned start, end;

	start = (ring->prod_pvt + off) & (ring->payload_bytes-1);
	end = (ring->prod_pvt + nr_bytes + off) & (ring->payload_bytes-1);
	memcpy(ring->payload + start, src, nr_bytes);
}

static inline void nc2_copy_to_ring(struct netchannel2_prod_ring *ring,
				    const void *src,
				    unsigned nr_bytes)
{
	nc2_copy_to_ring_off(ring, src, nr_bytes, 0);
}

static inline void __nc2_send_pad(struct netchannel2_prod_ring *ring,
				  unsigned nr_bytes)
{
	struct netchannel2_msg_hdr msg;
	msg.type = NETCHANNEL2_MSG_PAD;
	msg.flags = 0;
	msg.size = nr_bytes;
	nc2_copy_to_ring(ring, &msg, sizeof(msg));
	ring->prod_pvt += nr_bytes;
	ring->bytes_available -= nr_bytes;
}

static inline int __nc2_ring_would_wrap(struct netchannel2_prod_ring *ring,
					unsigned nr_bytes)
{
	RING_IDX mask;
	mask = ~(ring->payload_bytes - 1);
	return (ring->prod_pvt & mask) != ((ring->prod_pvt + nr_bytes) & mask);
}

static inline unsigned __nc2_pad_needed(struct netchannel2_prod_ring *ring)
{
	return ring->payload_bytes -
		(ring->prod_pvt & (ring->payload_bytes - 1));
}

static inline void __nc2_avoid_ring_wrap(struct netchannel2_prod_ring *ring,
					 unsigned nr_bytes)
{
	if (!__nc2_ring_would_wrap(ring, nr_bytes))
		return;
	__nc2_send_pad(ring, __nc2_pad_needed(ring));

}

/* Prepare a message for the other end and place it on the shared
 * ring, updating the private producer pointer.	 You need to call
 * nc2_flush_messages() before the message is actually made visible to
 * the other end.  It is permissible to send several messages in a
 * batch and only flush them once.
 */
static inline void nc2_send_message(struct netchannel2_prod_ring *ring,
				    unsigned type,
				    unsigned flags,
				    const void *msg,
				    size_t size)
{
	struct netchannel2_msg_hdr *hdr = (struct netchannel2_msg_hdr *)msg;

	__nc2_avoid_ring_wrap(ring, size);

	hdr->type = type;
	hdr->flags = flags;
	hdr->size = size;

	nc2_copy_to_ring(ring, msg, size);
	ring->prod_pvt += size;
	BUG_ON(ring->bytes_available < size);
	ring->bytes_available -= size;
}

static inline volatile void *__nc2_get_message_ptr(struct netchannel2_prod_ring *ncrp)
{
	return (volatile void *)ncrp->payload +
		(ncrp->prod_pvt & (ncrp->payload_bytes-1));
}

/* Copy the private producer pointer to the shared producer pointer,
 * with a suitable memory barrier such that all messages placed on the
 * ring are stable before we do the copy.  This effectively pushes any
 * messages which we've just sent out to the other end.	 Returns 1 if
 * we need to notify the other end and 0 otherwise.
 */
static inline int nc2_flush_ring(struct netchannel2_prod_ring *ring)
{
	RING_IDX old_prod, new_prod;

	old_prod = ring->sring->prod;
	new_prod = ring->prod_pvt;

	wmb();

	ring->sring->prod = new_prod;

	/* We need the update to prod to happen before we read
	 * event. */
	mb();

	/* We notify if the producer pointer moves across the event
	 * pointer. */
	if ((RING_IDX)(new_prod - ring->sring->prod_event) <
	    (RING_IDX)(new_prod - old_prod))
		return 1;
	else
		return 0;
}

/* Copy the private consumer pointer to the shared consumer pointer,
 * with a memory barrier so that any previous reads from the ring
 * complete before the pointer is updated.	This tells the other end
 * that we're finished with the messages, and that it can re-use the
 * ring space for more messages.  Returns 1 if we need to notify the
 * other end and 0 otherwise.
 */
static inline int nc2_finish_messages(struct netchannel2_cons_ring *ring)
{
	RING_IDX old_cons, new_cons;

	old_cons = ring->sring->cons;
	new_cons = ring->cons_pvt;

	/* Need to finish reading from the ring before updating
	   cons */
	mb();
	ring->sring->cons = ring->cons_pvt;

	/* Need to publish our new consumer pointer before checking
	   event. */
	mb();
	if ((RING_IDX)(new_cons - ring->sring->cons_event) <
	    (RING_IDX)(new_cons - old_cons))
		return 1;
	else
		return 0;
}

/* Check whether there are any unconsumed messages left on the shared
 * ring.  Returns 1 if there are, and 0 if there aren't.  If there are
 * no more messages, set the producer event so that we'll get a
 * notification as soon as another one gets sent.  It is assumed that
 * all messages up to @prod have been processed, and none of the ones
 * after it have been. */
static inline int nc2_final_check_for_messages(struct netchannel2_cons_ring *ring,
					       RING_IDX prod)
{
	if (prod != ring->sring->prod)
		return 1;
	/* Request an event when more stuff gets poked on the ring. */
	ring->sring->prod_event = prod + 1;

	/* Publish event before final check for responses. */
	mb();
	if (prod != ring->sring->prod)
		return 1;
	else
		return 0;
}

/* Can we send a message with @nr_bytes payload bytes?	Returns 1 if
 * we can or 0 if we can't.	 If there isn't space right now, set the
 * consumer event so that we'll get notified when space is
 * available. */
static inline int nc2_can_send_payload_bytes(struct netchannel2_prod_ring *ring,
					     unsigned nr_bytes)
{
	unsigned space;
	RING_IDX cons;
	BUG_ON(ring->bytes_available > ring->payload_bytes);
	/* Times 2 because we might need to send a pad message */
	if (likely(ring->bytes_available > nr_bytes * 2 + ring->reserve))
		return 1;
	if (__nc2_ring_would_wrap(ring, nr_bytes))
		nr_bytes += __nc2_pad_needed(ring);
retry:
	cons = ring->sring->cons;
	space = ring->payload_bytes - (ring->prod_pvt - cons);
	if (likely(space >= nr_bytes + ring->reserve)) {
		/* We have enough space to send the message. */

		/* Need to make sure that the read of cons happens
		   before any following memory writes. */
		mb();

		ring->bytes_available = space;

		return 1;
	} else {
		/* Not enough space available.	Set an event pointer
		   when cons changes.  We need to be sure that the
		   @cons used here is the same as the cons used to
		   calculate @space above, and the volatile modifier
		   on sring->cons achieves that. */
		ring->sring->cons_event = cons + 1;

		/* Check whether more space became available while we
		   were messing about. */

		/* Need the event pointer to be stable before we do
		   the check. */
		mb();
		if (unlikely(cons != ring->sring->cons)) {
			/* Cons pointer changed.  Try again. */
			goto retry;
		}

		/* There definitely isn't space on the ring now, and
		   an event has been set such that we'll be notified
		   if more space becomes available. */
		/* XXX we get a notification as soon as any more space
		   becomes available.  We could maybe optimise by
		   setting the event such that we only get notified
		   when we know that enough space is available.	 The
		   main complication is handling the case where you
		   try to send a message of size A, fail due to lack
		   of space, and then try to send one of size B, where
		   B < A.  It's not clear whether you want to set the
		   event for A bytes or B bytes.  The obvious answer
		   is B, but that means moving the event pointer
		   backwards, and it's not clear that that's always
		   safe.  Always setting for a single byte is safe, so
		   stick with that for now. */
		return 0;
	}
}

static inline int nc2_reserve_payload_bytes(struct netchannel2_prod_ring *ring,
					    unsigned nr_bytes)
{
	if (nc2_can_send_payload_bytes(ring, nr_bytes)) {
		ring->reserve += nr_bytes;
		return 1;
	} else {
		return 0;
	}
}

#endif /* __XEN_PUBLIC_IO_URING_H__ */
