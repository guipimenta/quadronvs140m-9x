/* NVIDIA/G86 build variant of the mini-VDD dispatch handlers.
 * Same code as vxd_vdd_vesa.c but compiled with NV_DETECT so the boot-handshake
 * breadcrumbs (NV_blog) in register_display_driver are emitted. Linked into
 * nvmini.vxd in place of vxd_vdd_vesa.obj; vesamini.vxd keeps the plain one. */
/* NV_DETECT comes from the makefile (-DNV_DETECT), matching vxd_vesa_nv.obj. */
#define VESA
#include "vxd_vdd.c"
