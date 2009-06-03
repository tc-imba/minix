
#define _SYSTEM 1

#include <minix/callnr.h>
#include <minix/com.h>
#include <minix/config.h>
#include <minix/const.h>
#include <minix/ds.h>
#include <minix/endpoint.h>
#include <minix/keymap.h>
#include <minix/minlib.h>
#include <minix/type.h>
#include <minix/ipc.h>
#include <minix/sysutil.h>
#include <minix/syslib.h>
#include <minix/debug.h>

#include <errno.h>
#include <env.h>

#include "glo.h"
#include "vm.h"
#include "proto.h"
#include "util.h"
#include "sanitycheck.h"
#include "region.h"

/*===========================================================================*
 *				do_fork					     *
 *===========================================================================*/
PUBLIC int do_fork(message *msg)
{
  int r, proc, s, childproc, fullvm;
  struct vmproc *vmp, *vmc;
  pt_t origpt;
  vir_bytes msgaddr;

  SANITYCHECK(SCL_FUNCTIONS);

  if(vm_isokendpt(msg->VMF_ENDPOINT, &proc) != OK) {
	printf("VM: bogus endpoint VM_FORK %d\n", msg->VMF_ENDPOINT);
  SANITYCHECK(SCL_FUNCTIONS);
	return EINVAL;
  }

  childproc = msg->VMF_SLOTNO;
  if(childproc < 0 || childproc >= NR_PROCS) {
	printf("VM: bogus slotno VM_FORK %d\n", msg->VMF_SLOTNO);
  SANITYCHECK(SCL_FUNCTIONS);
	return EINVAL;
  }

  vmp = &vmproc[proc];		/* parent */
  vmc = &vmproc[childproc];	/* child */
  vm_assert(vmc->vm_slot == childproc);

  if(vmp->vm_flags & VMF_HAS_DMA) {
	printf("VM: %d has DMA memory and may not fork\n", msg->VMF_ENDPOINT);
	return EINVAL;
  }

  fullvm = vmp->vm_flags & VMF_HASPT;

  /* The child is basically a copy of the parent. */
  origpt = vmc->vm_pt;
  *vmc = *vmp;
  vmc->vm_slot = childproc;
  vmc->vm_regions = NULL;
  vmc->vm_endpoint = NONE;	/* In case someone tries to use it. */
  vmc->vm_pt = origpt;
  vmc->vm_flags &= ~VMF_HASPT;

#if VMSTATS
  vmc->vm_bytecopies = 0;
#endif

  SANITYCHECK(SCL_DETAIL);

  if(fullvm) {
	SANITYCHECK(SCL_DETAIL);

	if(pt_new(&vmc->vm_pt) != OK) {
		printf("VM: fork: pt_new failed\n");
		return ENOMEM;
	}

	vmc->vm_flags |= VMF_HASPT;

	SANITYCHECK(SCL_DETAIL);

	if(map_proc_copy(vmc, vmp) != OK) {
		printf("VM: fork: map_proc_copy failed\n");
		pt_free(&vmc->vm_pt);
		return(ENOMEM);
	}

	if(vmp->vm_heap) {
		vmc->vm_heap = map_region_lookup_tag(vmc, VRT_HEAP);
		vm_assert(vmc->vm_heap);
	}

	SANITYCHECK(SCL_DETAIL);
  } else {
        phys_bytes prog_bytes, parent_abs, child_abs; /* Intel only */
        phys_clicks prog_clicks, child_base;

	/* Determine how much memory to allocate.  Only the data and stack
	 * need to be copied, because the text segment is either shared or
	 * of zero length.
	 */

	prog_clicks = (phys_clicks) vmp->vm_arch.vm_seg[S].mem_len;
	prog_clicks += (vmp->vm_arch.vm_seg[S].mem_vir - vmp->vm_arch.vm_seg[D].mem_vir);
	prog_bytes = (phys_bytes) prog_clicks << CLICK_SHIFT;
	if ( (child_base = ALLOC_MEM(prog_clicks, 0)) == NO_MEM) {
  SANITYCHECK(SCL_FUNCTIONS);
		return(ENOMEM);
	}

	/* Create a copy of the parent's core image for the child. */
	child_abs = (phys_bytes) child_base << CLICK_SHIFT;
	parent_abs = (phys_bytes) vmp->vm_arch.vm_seg[D].mem_phys << CLICK_SHIFT;
	FIXME("VM uses kernel for abscopy");
	s = sys_abscopy(parent_abs, child_abs, prog_bytes);
	if (s < 0) vm_panic("do_fork can't copy", s);

	/* A separate I&D child keeps the parents text segment.  The data and stack
	* segments must refer to the new copy.
	*/
	if (!(vmc->vm_flags & VMF_SEPARATE))
		vmc->vm_arch.vm_seg[T].mem_phys = child_base;
	vmc->vm_arch.vm_seg[D].mem_phys = child_base;
	vmc->vm_arch.vm_seg[S].mem_phys = vmc->vm_arch.vm_seg[D].mem_phys +
           (vmp->vm_arch.vm_seg[S].mem_vir - vmp->vm_arch.vm_seg[D].mem_vir);
  }

  /* Only inherit these flags. */
  vmc->vm_flags &= (VMF_INUSE|VMF_SEPARATE|VMF_HASPT);

  /* Tell kernel about the (now successful) FORK. */
  if((r=sys_fork(vmp->vm_endpoint, childproc,
	&vmc->vm_endpoint, vmc->vm_arch.vm_seg,
	fullvm ? PFF_VMINHIBIT : 0, &msgaddr)) != OK) {
        vm_panic("do_fork can't sys_fork", r);
  }

  if(fullvm) {
	if(handle_memory(vmc, msgaddr, sizeof(message), 1) != OK)
		vm_panic("can't make message writable (child)", NO_NUM);
	if(handle_memory(vmp, msgaddr, sizeof(message), 1) != OK)
		vm_panic("can't make message writable (parent)", NO_NUM);
	if((r=pt_bind(&vmc->vm_pt, vmc)) != OK)
		vm_panic("fork can't pt_bind", r);
  }

  /* Inform caller of new child endpoint. */
  msg->VMF_CHILD_ENDPOINT = vmc->vm_endpoint;

  SANITYCHECK(SCL_FUNCTIONS);
  return OK;
}

