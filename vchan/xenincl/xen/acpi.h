#ifndef _XEN_ACPI_H
#define _XEN_ACPI_H

#include <linux/types.h>
#include <acpi/acpi_drivers.h>
#include <acpi/processor.h>

#ifdef CONFIG_XEN_S3
#include <asm/xen/hypervisor.h>

static inline bool xen_pv_acpi(void)
{
	return xen_pv_domain();
}
#else
static inline bool xen_pv_acpi(void)
{
	return false;
}
#endif

int acpi_notify_hypervisor_state(u8 sleep_state,
				 u32 pm1a_cnt, u32 pm1b_cnd);

/*
 * Following are interfaces for xen acpi processor control
 */

/* Events notified to xen */
#define PROCESSOR_PM_INIT	1
#define PROCESSOR_PM_CHANGE	2
#define PROCESSOR_HOTPLUG	3

/* Objects for the PM events */
#define PM_TYPE_IDLE		0
#define PM_TYPE_PERF		1
#define PM_TYPE_THR		2
#define PM_TYPE_MAX		3

/* Processor hotplug events */
#define HOTPLUG_TYPE_ADD	0
#define HOTPLUG_TYPE_REMOVE	1

#ifdef CONFIG_ACPI_PROCESSOR_XEN

struct processor_cntl_xen_ops {
	/* Transfer processor PM events to xen */
int (*pm_ops[PM_TYPE_MAX])(struct acpi_processor *pr, int event);
	/* Notify physical processor status to xen */
	int (*hotplug)(struct acpi_processor *pr, int type);
};

extern int processor_cntl_xen(void);
extern int processor_cntl_xen_pm(void);
extern int processor_cntl_xen_pmperf(void);
extern int processor_cntl_xen_pmthr(void);
extern int processor_cntl_xen_prepare(struct acpi_processor *pr);
extern int processor_cntl_xen_notify(struct acpi_processor *pr,
			int event, int type);
extern int processor_cntl_xen_power_cache(int cpu, int cx,
		struct acpi_power_register *reg);
#else

static inline int processor_cntl_xen(void) { return 0; }
static inline int processor_cntl_xen_pm(void) { return 0; }
static inline int processor_cntl_xen_pmperf(void) { return 0; }
static inline int processor_cntl_xen_pmthr(void) { return 0; }
static inline int processor_cntl_xen_notify(struct acpi_processor *pr,
			int event, int type)
{
	return 0;
}
static inline int processor_cntl_xen_prepare(struct acpi_processor *pr)
{
	return 0;
}
static inline int processor_cntl_xen_power_cache(int cpu, int cx,
		struct acpi_power_register *reg)
{
	return 0;
}
#endif /* CONFIG_ACPI_PROCESSOR_XEN */

#endif	/* _XEN_ACPI_H */
