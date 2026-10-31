#ifndef MIRROR_CONFIG_H
#define MIRROR_CONFIG_H
/* Only the dedicated game mirror opts into batch policy. Other images and
 * regression builds keep the common driver's legacy default. Override at
 * compile time to build the eight matched control experiments (0..7). */
#ifndef KEY3_BATCH_FLAGS
#ifdef MIRROR_KEY3
#define KEY3_BATCH_FLAGS 7u
#else
#define KEY3_BATCH_FLAGS 0u
#endif
#endif
#ifndef KEY3_SUPPLY_PROBE
#define KEY3_SUPPLY_PROBE 1u
#endif
/* Firmware mirror selection for the multi-image FPGA split (2026-10-21).
 * MIRROR_LAUNCH = slot-0 launcher: MC-style menu, keys 1/2/3 reconfigure to
 *   image slots 1/2/3; every other image returns here on a four-key hold.
 * MIRROR_M12    = key1 BASIC + key2 COMPARE image (legacy + bucket engines).
 * MIRROR_KEY3   = key3 EXTENSION game image (legacy DIRECT engine only).
 * No define     = regression build retaining both pages (projects/console).
 * key4 LAYERS (mc_layers/mc_runner, tile4 engine) is retired in every build. */
#if defined(MIRROR_LAUNCH) && (defined(MIRROR_M12) || defined(MIRROR_KEY3))
#error "select at most one mirror"
#endif
#ifdef MIRROR_LAUNCH
#define MIRROR_HAS_KEY2 0
#define MIRROR_HAS_KEY3 0
#else
#ifndef MIRROR_HAS_KEY2
#if defined(MIRROR_KEY3)
#define MIRROR_HAS_KEY2 0
#else
#define MIRROR_HAS_KEY2 1
#endif
#endif
#ifndef MIRROR_HAS_KEY3
#if defined(MIRROR_M12)
#define MIRROR_HAS_KEY3 0
#else
#define MIRROR_HAS_KEY3 1
#endif
#endif
#endif
#endif
