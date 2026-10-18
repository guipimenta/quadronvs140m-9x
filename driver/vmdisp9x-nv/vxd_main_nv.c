/* NVIDIA/G86 build variant.
 * For now it scans out through the generic VESA backend (so the desktop works
 * on the T61 today) while adding native G86 hardware detection (Milestone 2).
 * As the native path matures, the VESA scanout is replaced by native modeset.
 */
#define NV_DETECT
#define VESA
#include "vxd_main.c"
