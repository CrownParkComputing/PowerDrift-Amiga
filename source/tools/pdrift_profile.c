/* pdrift_profile.c -- host profiler: where does a frame's CPU go?
 * Splits the per-frame cost across machine (3x68k), renderer, Z80 sound CPU,
 * and audio synth (ymfm + SegaPCM + mix) so we can target optimizations. Host
 * x86 timings are only a proxy for the 68030, but the RELATIVE integer costs
 * transfer well enough to rank the hotspots. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pdrift_machine.h"
#include "pdrift_render.h"
#include "pdrift_audio.h"

static void load(const char *dir, const char *name, uint8_t *dst, size_t want)
{ char p[1024]; snprintf(p,sizeof p,"%s/%s",dir,name); FILE*f=fopen(p,"rb"); if(!f){perror(p);exit(1);} if(fread(dst,1,want,f)!=want){fprintf(stderr,"short %s\n",p);exit(1);} fclose(f);}
static uint8_t *loada(const char *dir, const char *name, size_t want)
{ uint8_t*b=malloc(want); load(dir,name,b,want); return b; }
static double ms(struct timespec a, struct timespec b){ return (b.tv_sec-a.tv_sec)*1e3 + (b.tv_nsec-a.tv_nsec)/1e6; }

int main(int argc, char **argv)
{
    const char *dir = argc>1?argv[1]:"build/pdrift";
    int N = argc>2?atoi(argv[2]):300;

    load(dir,"maincpu.bin",pdm_maincpu,PDM_MAINCPU_SIZE);
    load(dir,"subx.bin",pdm_subx,PDM_SUBX_SIZE);
    load(dir,"suby.bin",pdm_suby,PDM_SUBY_SIZE);
    pdr_ypixrom = loada(dir,"ysprites_pix.bin",PDR_YPIX_BYTES);
    pdr_brom = loada(dir,"bsprites.bin",PDR_BROM_BYTES);
    uint8_t *soundcpu=loada(dir,"soundcpu.bin",0x10000), *pcm=loada(dir,"pcm.bin",0x200000);

    pdrift_machine_default_inputs();
    pdrift_machine_init();
    pdr_audio_init(soundcpu,pcm,0x200000);

    /* warm up to where music is playing */
    for (int i=0;i<600;i++){ pdrift_machine_run_frame(); uint8_t c; while(pdrift_machine_sound_latch_take(&c)) pdr_audio_command(c); pdr_audio_run_cycles(PDR_Z80_CLOCK/60); }

    const int cyc = PDR_Z80_CLOCK/60;
    const int smp = PDR_OUT_RATE/60;
    signed char buf[PDR_OUT_RATE/60 + 8];
    struct timespec a,b;
    double t_mach=0,t_rend=0,t_z80=0,t_synth=0,t_ym=0;

    for (int i=0;i<N;i++){
        clock_gettime(CLOCK_MONOTONIC,&a); pdrift_machine_run_frame(); clock_gettime(CLOCK_MONOTONIC,&b); t_mach+=ms(a,b);
        uint8_t c; while(pdrift_machine_sound_latch_take(&c)) pdr_audio_command(c);
        clock_gettime(CLOCK_MONOTONIC,&a); pdrift_render_frame(); clock_gettime(CLOCK_MONOTONIC,&b); t_rend+=ms(a,b);
        clock_gettime(CLOCK_MONOTONIC,&a); pdr_audio_run_cycles(cyc); clock_gettime(CLOCK_MONOTONIC,&b); t_z80+=ms(a,b);
        clock_gettime(CLOCK_MONOTONIC,&a); pdr_audio_render(buf,smp); clock_gettime(CLOCK_MONOTONIC,&b); t_synth+=ms(a,b);
    }
    /* ymfm alone: same number of native generates as one audio-second's worth */
    extern int pdr_ym2151_sample(void);
    int ymcalls = smp*N;
    clock_gettime(CLOCK_MONOTONIC,&a);
    volatile int s=0; for (int i=0;i<ymcalls;i++) s+=pdr_ym2151_sample();
    clock_gettime(CLOCK_MONOTONIC,&b); t_ym=ms(a,b);

    double total=t_mach+t_rend+t_z80+t_synth;
    printf("per-frame CPU over %d frames (host x86 proxy):\n", N);
    printf("  machine(3x68k) %7.3f ms  %5.1f%%\n", t_mach/N, 100*t_mach/total);
    printf("  renderer       %7.3f ms  %5.1f%%\n", t_rend/N, 100*t_rend/total);
    printf("  Z80 sound cpu  %7.3f ms  %5.1f%%\n", t_z80/N, 100*t_z80/total);
    printf("  audio synth    %7.3f ms  %5.1f%%  (ymfm+segapcm+mix)\n", t_synth/N, 100*t_synth/total);
    printf("    of which ymfm~%7.3f ms  %5.1f%% of frame\n", t_ym/N, 100*t_ym/total);
    printf("  TOTAL          %7.3f ms/frame -> %.0f fps (host)\n", total/N, 1000.0/(total/N));
    printf("  VIDEO-only (machine+render) = %.1f%%, AUDIO (z80+synth) = %.1f%%\n",
           100*(t_mach+t_rend)/total, 100*(t_z80+t_synth)/total);
    return 0;
}
