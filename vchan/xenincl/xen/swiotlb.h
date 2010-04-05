#ifndef __LINUX_XEN_SWIOTLB_H
#define __LINUX_XEN_SWIOTLB_H

#include <linux/types.h>

struct device;
struct dma_attrs;
struct scatterlist;

/*
 * Maximum allowable number of contiguous slabs to map,
 * must be a power of 2.  What is the appropriate value ?
 * The complexity of {map,unmap}_single is linearly dependent on this value.
 */
#define IO_TLB_SEGSIZE	128


/*
 * log of the size of each IO TLB slab.  The number of slabs is command line
 * controllable.
 */
#define IO_TLB_SHIFT 11

extern void *xen_swiotlb_alloc_boot(size_t bytes, unsigned long nslabs);
extern void *xen_swiotlb_alloc(unsigned order, unsigned long nslabs);

extern dma_addr_t xen_swiotlb_phys_to_bus(struct device *hwdev,
				      phys_addr_t address);
extern phys_addr_t xen_swiotlb_bus_to_phys(struct device *hwdev,
				       dma_addr_t address);

extern int xen_swiotlb_arch_range_needs_mapping(phys_addr_t paddr, size_t size);

extern void
*xen_swiotlb_alloc_coherent(struct device *hwdev, size_t size,
			dma_addr_t *dma_handle, gfp_t flags);

extern void
xen_swiotlb_free_coherent(struct device *hwdev, size_t size,
		      void *vaddr, dma_addr_t dma_handle);

extern dma_addr_t xen_swiotlb_map_page(struct device *dev, struct page *page,
				   unsigned long offset, size_t size,
				   enum dma_data_direction dir,
				   struct dma_attrs *attrs);
extern void xen_swiotlb_unmap_page(struct device *hwdev, dma_addr_t dev_addr,
			       size_t size, enum dma_data_direction dir,
			       struct dma_attrs *attrs);

extern int
xen_swiotlb_map_sg(struct device *hwdev, struct scatterlist *sg, int nents,
	       int direction);

extern void
xen_swiotlb_unmap_sg(struct device *hwdev, struct scatterlist *sg, int nents,
		 int direction);

extern int
xen_swiotlb_map_sg_attrs(struct device *hwdev, struct scatterlist *sgl, int nelems,
		     enum dma_data_direction dir, struct dma_attrs *attrs);

extern void
xen_swiotlb_unmap_sg_attrs(struct device *hwdev, struct scatterlist *sgl,
		       int nelems, enum dma_data_direction dir,
		       struct dma_attrs *attrs);

extern void
xen_swiotlb_sync_single_for_cpu(struct device *hwdev, dma_addr_t dev_addr,
			    size_t size, enum dma_data_direction dir);

extern void
xen_swiotlb_sync_sg_for_cpu(struct device *hwdev, struct scatterlist *sg,
			int nelems, enum dma_data_direction dir);

extern void
xen_swiotlb_sync_single_for_device(struct device *hwdev, dma_addr_t dev_addr,
			       size_t size, enum dma_data_direction dir);

extern void
xen_swiotlb_sync_sg_for_device(struct device *hwdev, struct scatterlist *sg,
			   int nelems, enum dma_data_direction dir);

extern void
xen_swiotlb_sync_single_range_for_cpu(struct device *hwdev, dma_addr_t dev_addr,
				  unsigned long offset, size_t size,
				  enum dma_data_direction dir);

extern void
xen_swiotlb_sync_single_range_for_device(struct device *hwdev, dma_addr_t dev_addr,
				     unsigned long offset, size_t size,
				     enum dma_data_direction dir);

extern int
xen_swiotlb_dma_mapping_error(struct device *hwdev, dma_addr_t dma_addr);

extern int
xen_swiotlb_dma_supported(struct device *hwdev, u64 mask);

extern void
xen_swiotlb_fixup(void *buf, size_t size, unsigned long nslabs);

#endif /* __LINUX_XEN_SWIOTLB_H */
