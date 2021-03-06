/* SPDX-License-Identifier: MIT */
/******************************************************************************
 * common.c
 *
 * Common stuff special to x86 goes here.
 *
 * Copyright (c) 2002-2003, K A Fraser & R Neugebauer
 * Copyright (c) 2005, Grzegorz Milos, Intel Research Cambridge
 * Copyright (c) 2017, Simon Kuenzer <simon.kuenzer@neclab.eu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */
/* mm.c
 ****************************************************************************
 * (C) 2003 - Rolf Neugebauer - Intel Research Cambridge
 * (C) 2005 - Grzegorz Milos - Intel Research Cambridge
 ****************************************************************************
 *
 *        File: mm.c
 *      Author: Rolf Neugebauer (neugebar@dcs.gla.ac.uk)
 *     Changes: Grzegorz Milos
 *
 *        Date: Aug 2003, chages Aug 2005
 *
 * Environment: Xen Minimal OS
 * Description: memory management related functions
 *              contains buddy page allocator from Xen.
 *
 ****************************************************************************
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <uk/arch/types.h>
#include <uk/arch/limits.h>
#include <uk/config.h>
#include <uk/print.h>
#include <uk/assert.h>
#include <uk/plat/config.h>
#include <uk/plat/console.h>
#include <uk/plat/bootstrap.h>

#include <xen/xen.h>
#include <common/events.h>
#if LIBUKSCHED
#include <common/sched.h>
#endif
#ifdef __X86_64__
#include <xen-x86/hypercall64.h>
#else
#include <xen-x86/hypercall32.h>
#endif
#include <xen-x86/irq.h>
#include <xen-x86/mm.h>
#include <xen-x86/setup.h>
#include <xen/arch-x86/cpuid.h>
#include <xen/arch-x86/hvm/start_info.h>

#define MAX_CMDLINE_SIZE 1024
static char cmdline[MAX_CMDLINE_SIZE];

start_info_t *HYPERVISOR_start_info;
shared_info_t *HYPERVISOR_shared_info;

/*
 * Just allocate the kernel stack here. SS:ESP is set up to point here
 * in head.S.
 */
char _libxenplat_bootstack[2*__STACK_SIZE];

/*
 * Memory region description
 */
#define UKPLAT_MEMRD_MAX_ENTRIES 2
unsigned int _libxenplat_mrd_num;
struct ukplat_memregion_desc _libxenplat_mrd[UKPLAT_MEMRD_MAX_ENTRIES];

static inline void _init_traps(void)
{
	trap_init();
}

static inline void _init_cpufeatures(void)
{
#if __SSE__
	unsigned long sse_status = 0x1f80;
#endif

	/* FPU */
	asm volatile("fninit");

#if __SSE__
	asm volatile("ldmxcsr %0" : : "m"(sse_status));
#endif
}

static inline void _init_shared_info(void)
{
	int ret;
	unsigned long pa = HYPERVISOR_start_info->shared_info;
	extern char _libxenplat_shared_info[__PAGE_SIZE];

	if ((ret = HYPERVISOR_update_va_mapping(
		 (unsigned long)_libxenplat_shared_info, __pte(pa | 7),
		 UVMF_INVLPG)))
		UK_CRASH("Failed to map shared_info: %d\n", ret);
	HYPERVISOR_shared_info = (shared_info_t *)_libxenplat_shared_info;
}

static inline void _init_mem(void)
{
	unsigned long start_pfn, max_pfn;

	_init_mem_prepare(&start_pfn, &max_pfn);

	if (max_pfn >= MAX_MEM_SIZE / __PAGE_SIZE)
		max_pfn = MAX_MEM_SIZE / __PAGE_SIZE - 1;

	uk_printd(DLVL_INFO, "     start_pfn: %lx\n", start_pfn);
	uk_printd(DLVL_INFO, "       max_pfn: %lx\n", max_pfn);

	_init_mem_build_pagetable(&start_pfn, &max_pfn);
	//_init_mem_clear_bootstrap(); /* FIXME - stack or text screwed up? */
	//_init_mem_set_readonly(&_text, &_erodata); /* FIXME - shared info ro? */

	/* Fill out mrd array */
	/* heap */
	_libxenplat_mrd[0].base  = to_virt(start_pfn << __PAGE_SHIFT);
	_libxenplat_mrd[0].len   = (size_t) to_virt(max_pfn << __PAGE_SHIFT)
		- (size_t) to_virt(start_pfn << __PAGE_SHIFT);
	_libxenplat_mrd[0].flags = (UKPLAT_MEMRF_ALLOCATABLE);
	_libxenplat_mrd_num = 1;
}

void _libxenplat_x86entry(void *start_info) __noreturn;

void _libxenplat_x86entry(void *start_info)
{
	uk_printd(DLVL_INFO, "Entering from Xen (x86, PV)...\n");

	_init_traps();
	_init_cpufeatures();
	HYPERVISOR_start_info = (start_info_t *)start_info;
	_init_shared_info(); /* remaps shared info */
	strncpy(cmdline, (char *)HYPERVISOR_start_info->cmd_line,
		MAX_CMDLINE_SIZE);

	/* Set up events. */
	init_events();

	/* ENABLE EVENT DELIVERY. This is disabled at start of day. */
	__sti();

	uk_printd(DLVL_INFO, "    start_info: %p\n", HYPERVISOR_start_info);
	uk_printd(DLVL_INFO, "   shared_info: %p\n", HYPERVISOR_shared_info);
	uk_printd(DLVL_INFO, "hypercall_page: %p\n", hypercall_page);

	_init_mem();

	ukplat_entry_argp(UK_NAME, cmdline, MAX_CMDLINE_SIZE);
}
