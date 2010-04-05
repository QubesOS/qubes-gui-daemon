#ifndef _XEN_BALLOON_H
#define _XEN_BALLOON_H

/* Allocate/free a set of empty pages in low memory (i.e., no RAM mapped). */
struct page **alloc_empty_pages_and_pagevec(int nr_pages);
void free_empty_pages_and_pagevec(struct page **pagevec, int nr_pages);

#endif
