/* pdrift_mathstubs.c -- with the lean Jarek YM2151 core (-DPDR_LEAN_YM) the build
 * links real libm for the table init (sin/pow/log/floor), so we must NOT stub
 * those (libm's pow/sin internally use log2/cos etc., and a 0-returning stub
 * would corrupt them). Only `cexp` is stubbed: bebbo's libm doesn't provide it,
 * Musashi's m68kfpu.c references it, and a 68000 never executes FPU ops so it's
 * never called. The renderer stays integer/float-free; only YM init uses libm. */
double cexp(double x) { (void)x; return 0.0; }
