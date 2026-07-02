/* pdrift_playtest.c -- drive the machine through coin -> start -> gas on the host
 * and dump frames, to check whether gameplay actually advances (car accelerates,
 * road scrolls) independent of the Amiga. If the car drives here, the emulation
 * is fine and any "no speed" on target is input delivery; if not, it's a bug. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pdrift_machine.h"
#include "pdrift_render.h"

static double rms(struct timespec a, struct timespec b){return (b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6;}
static void bench(const char *label){
    struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
    for(int i=0;i<200;i++) pdrift_render_frame();
    clock_gettime(CLOCK_MONOTONIC,&b);
    fprintf(stderr,"  RENDER %-8s %.3f ms/frame (host)\n", label, rms(a,b)/200);
}

static void load(const char *dir, const char *name, uint8_t *dst, size_t want)
{ char p[1024]; snprintf(p,sizeof p,"%s/%s",dir,name); FILE*f=fopen(p,"rb"); if(!f){perror(p);exit(1);} if(fread(dst,1,want,f)!=want){fprintf(stderr,"short %s\n",p);exit(1);} fclose(f);}
static uint8_t *loada(const char *dir, const char *name, size_t want){uint8_t*b=malloc(want);load(dir,name,b,want);return b;}

static void dump(const char *path)
{
    static uint32_t rgb[PDR_W*PDR_H];
    pdrift_render_frame();
    pdrift_render_resolve_rgb(rgb);
    FILE*f=fopen(path,"wb"); fprintf(f,"P6\n%d %d\n255\n",PDR_W,PDR_H);
    for(unsigned i=0;i<PDR_W*PDR_H;i++){uint8_t px[3]={rgb[i]>>16,rgb[i]>>8,rgb[i]};fwrite(px,1,3,f);}
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dir = argc>1?argv[1]:"build/pdrift";
    const char *outdir = argc>2?argv[2]:"/tmp";
    load(dir,"maincpu.bin",pdm_maincpu,PDM_MAINCPU_SIZE);
    load(dir,"subx.bin",pdm_subx,PDM_SUBX_SIZE);
    load(dir,"suby.bin",pdm_suby,PDM_SUBY_SIZE);
    pdr_ypixrom=loada(dir,"ysprites_pix.bin",PDR_YPIX_BYTES);
    pdr_brom=loada(dir,"bsprites.bin",PDR_BROM_BYTES);
    pdrift_machine_default_inputs();
    pdrift_machine_init();

    char path[1024];
    for (int fr=0; fr<1200; fr++) {
        pdm_in.general = 0xdf;
        if (fr>=100 && fr<106) pdm_in.general &= (uint8_t)~0x40;   /* coin pulse */
        if (fr>=140 && fr<150) pdm_in.general &= (uint8_t)~0x08;   /* start pulse */
        if (fr>=160) { pdm_in.general |= 0x20; pdm_in.adc[4]=0xff; } /* gear + full gas */
        pdrift_machine_run_frame();
        if (fr==120||fr==200||fr==350||fr==500||fr==800||fr==1100) {
            snprintf(path,sizeof path,"%s/play_%04d.ppm",outdir,fr);
            dump(path);
            fprintf(stderr,"frame %d: general=%02x adc4=%02x -> %s\n", fr, pdm_in.general, pdm_in.adc[4], path);
        }
        { extern int pdm_palette_dirty; static int dfr=0, tot=0; tot++;
          if (pdm_palette_dirty) dfr++; pdm_palette_dirty=0;
          if (fr==1199) fprintf(stderr,"  palette-dirty frames: %d/%d (%.0f%%) -- lower = bigger gate win\n",dfr,tot,100.0*dfr/tot); }
        if (fr==120) bench("attract");
        if (fr==500) bench("racing");
        if (fr==1100) bench("racing2");
    }
    return 0;
}
