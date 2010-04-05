#ifndef XEN_LIVE_MAPS_H__
#define XEN_LIVE_MAPS_H__

/* A mechanism for tracking where pages have been grant mapped from.
   Anything which can map pages through a grant reference is supposed
   to allocate a page_tracker and then, whenever they map a grant:

   a) Flag the page as foreign with SetPageForeign(), and
   b) Register the struct page with a tracker through start_tracking_page().

   If you later need to grant access to the page (either with a normal
   grant or implicitly in a copy grant operation), you should use
   lookup_tracker_page() to find out what domain and grant reference
   it was mapped from.

   Obviously, if a backend knows that the page will never need to be
   re-granted once it's been mapped, it can avoid doing all this
   stuff.

   The number of trackers is quite limited, so they shouldn't be
   allocated unnecessarily.  One per backend class is reasonable
   (i.e. netback, blkback, etc.), but one per backend device probably
   isn't.
*/

#include <linux/mm.h>
#include <xen/grant_table.h>

#ifdef CONFIG_XEN

/* We use page->private to store some index information so that we can
   find the tracking information later.  The top few bits are used to
   identify the tracker, and the rest are used as an index into that
   tracker. */

/* How many bits to use for tracker IDs. */
#define LIVE_MAP_TRACKER_BITS 2

/* How many bits to use for tracker indexes. */
#define LIVE_MAP_TRACKER_IDX_BITS (32 - LIVE_MAP_TRACKER_BITS)

/* Maximum number of trackers */
#define LIVE_MAP_NR_TRACKERS (1 << LIVE_MAP_TRACKER_BITS)

/* Bitmask of index inside tracker */
#define LIVE_MAP_TRACKER_IDX_MASK (~0u >> LIVE_MAP_TRACKER_BITS)

/* Turn off some moderately expensive debug checks. */
#undef LIVE_MAPS_DEBUG

struct page_foreign_tracked {
        domid_t dom;
        grant_ref_t gref;
        void *ctxt;
#ifdef LIVE_MAPS_DEBUG
        unsigned in_use;
#endif
};

struct page_foreign_tracker {
        unsigned size;
        unsigned id;
        struct page_foreign_tracked contents[];
};

extern struct page_foreign_tracker *foreign_trackers[LIVE_MAP_NR_TRACKERS];

/* Allocate a foreign page tracker.  @size is the maximum index in the
   tracker.  Returns NULL on error. */
struct page_foreign_tracker *alloc_page_foreign_tracker(unsigned size);

/* Release a tracker allocated with alloc_page_foreign_tracker.  There
   should be no tracked pages when this is called. */
void free_page_foreign_tracker(struct page_foreign_tracker *pft);

static inline struct page_foreign_tracker *tracker_for_page(struct page *p)
{
        unsigned idx = page_private(p);
        return foreign_trackers[idx >> LIVE_MAP_TRACKER_IDX_BITS];
}

static inline void *get_page_tracker_ctxt(struct page *p)
{
        struct page_foreign_tracker *pft = tracker_for_page(p);
        unsigned idx = page_private(p);
        return pft->contents[idx & LIVE_MAP_TRACKER_IDX_MASK].ctxt;
}

/* Start tracking a page.  @idx is an index in the tracker which is
   not currently in use, and must be less than the size of the
   tracker.  The page must be marked as foreign before this is called.
   The caller is expected to make sure that the page is not a
   simulataneous target of lookup_tracker_page().  The page should be
   passed to stop_tracking_page() when the grant is unmapped. */
static inline void start_tracking_page(struct page_foreign_tracker *pft,
                                       struct page *p,
                                       domid_t dom,
                                       grant_ref_t gref,
                                       unsigned idx,
                                       void *ctxt)
{
        BUG_ON(!PageForeign(p));
#ifdef LIVE_MAPS_DEBUG
        BUG_ON(idx > pft->size);
        BUG_ON(pft->contents[idx].in_use);
        pft->contents[idx].in_use = 1;
#endif
        pft->contents[idx].dom = dom;
        pft->contents[idx].gref = gref;
        pft->contents[idx].ctxt = ctxt;
        set_page_private(p, idx | (pft->id << LIVE_MAP_TRACKER_IDX_BITS));
}

static inline void stop_tracking_page(struct page *p)
{
#ifdef LIVE_MAPS_DEBUG
        struct page_foreign_tracker *pft;
        unsigned idx = page_private(p);
        BUG_ON(!PageForeign(p));
        pft = tracker_for_page(p);
        BUG_ON((idx & LIVE_MAP_TRACKER_IDX_MASK) >= pft->size);
        BUG_ON(!pft->contents[idx & LIVE_MAP_TRACKER_IDX_MASK].in_use);
        pft->contents[idx & LIVE_MAP_TRACKER_IDX_MASK].in_use = 0;
        set_page_private(p, 0);
#endif
}

/* Lookup a page which is tracked in some tracker.
   start_tracking_page() must have been called previously.  *@dom and
   *@gref will be set to the values which were specified when
   start_tracking_page() was called. */
static inline void lookup_tracker_page(struct page *p, domid_t *dom,
                                       grant_ref_t *gref)
{
        struct page_foreign_tracker *pft;
        unsigned idx = page_private(p);
        BUG_ON(!PageForeign(p));
        pft = tracker_for_page(p);
#ifdef LIVE_MAPS_DEBUG
        BUG_ON(!pft);
        BUG_ON((idx & LIVE_MAP_TRACKER_IDX_MASK) >= pft->size);
        BUG_ON(!pft->contents[idx & LIVE_MAP_TRACKER_IDX_MASK].in_use);
#endif
        *dom = pft->contents[idx & LIVE_MAP_TRACKER_IDX_MASK].dom;
        *gref = pft->contents[idx & LIVE_MAP_TRACKER_IDX_MASK].gref;
}

static inline int page_is_tracked(struct page *p)
{
        return PageForeign(p) && p->mapping;
}

#else /* !CONFIG_XEN */
static inline int page_is_tracked(struct page *p)
{
        return 0;
}
static void lookup_tracker_page(struct page *p, domid_t *domid,
                                grant_ref_t *gref)
{
        BUG();
}
#endif

#endif /* !XEN_LIVE_MAPS_H__ */
