/* ym2151_amiga.h -- minimal environment for the standalone Jarek YM2151 core.
 * Replaces FBNeo's driver.h/state.h so ym2151.c builds outside FBNeo. No MAME
 * timers, no savestate, no bprintf -- pdrift_audio.c owns timers/IRQ. */
#ifndef YM2151_AMIGA_H
#define YM2151_AMIGA_H

#include <stdint.h>

typedef int8_t   INT8;
typedef int16_t  INT16;
typedef int32_t  INT32;
typedef int64_t  INT64;
typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;

#ifndef INLINE
#define INLINE static __inline__
#endif

/* YM2151 port write handler type (unused by Power Drift; the CT1/CT2 pins) */
typedef void (*write8_handler)(UINT32 offset, UINT32 data);

/* diagnostics no-op */
#define logerror(...) do { } while (0)

#endif
