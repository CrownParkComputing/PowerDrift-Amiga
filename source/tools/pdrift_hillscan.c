/* pdrift_hillscan.c -- run the attract demo and find HILL frames (rotate shear
 * dxy/dyx nonzero), to diagnose the "sprites vanish on hills" report: does the
 * live 16B sprite count drop on a hill (machine hides them) or stay (a render/
 * clip issue)? Dumps the first strong-tilt frame for visual inspection. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pdrift_machine.h"
#include "pdrift_render.h"

static uint16_t rw(unsigned i){ return (uint16_t)((pdm_rotate_buffer[i*2]<<8)|pdm_rotate_buffer[i*2+1]); }
static int32_t rs(unsigned i){ return (int32_t)(((uint32_t)rw(i)<<16)|rw(i+1)); }

static void load(const char*dir,const char*name,uint8_t*dst,size_t want){char p[1024];snprintf(p,sizeof p,"%s/%s",dir,name);FILE*f=fopen(p,"rb");if(!f){perror(p);exit(1);}if(fread(dst,1,want,f)!=want){fprintf(stderr,"short %s\n",p);exit(1);}fclose(f);}
static uint8_t*loada(const char*dir,const char*name,size_t want){uint8_t*b=malloc(want);load(dir,name,b,want);return b;}

/* count non-hidden 16B (HUD/scoreboard) sprite entries the machine has posted */
static int count_bsprites(void){
    int n=0;
    for(unsigned e=0;e<0x1000/16;e++){
        uint16_t d[3];
        for(int i=0;i<3;i++) d[i]=(uint16_t)((pdm_bsprite_ram[(e*8+i)*2]<<8)|pdm_bsprite_ram[(e*8+i)*2+1]);
        if(d[2]&0x8000) break;                 /* end marker */
        if((d[2]&0x4000)) continue;            /* hide bit */
        if((d[0]&0xff) >= (d[0]>>8)) continue;  /* top>=bottom */
        n++;
    }
    return n;
}

static void dump(const char*path){
    static uint32_t rgb[PDR_W*PDR_H];
    pdrift_render_frame();
    pdrift_render_resolve_rgb(rgb);
    FILE*f=fopen(path,"wb");fprintf(f,"P6\n%d %d\n255\n",PDR_W,PDR_H);
    for(unsigned i=0;i<PDR_W*PDR_H;i++){uint8_t px[3]={(uint8_t)(rgb[i]>>16),(uint8_t)(rgb[i]>>8),(uint8_t)rgb[i]};fwrite(px,1,3,f);}
    fclose(f);
}

int main(int argc,char**argv){
    const char*dir=argc>1?argv[1]:"build/pdrift";
    load(dir,"maincpu.bin",pdm_maincpu,PDM_MAINCPU_SIZE);
    load(dir,"subx.bin",pdm_subx,PDM_SUBX_SIZE);
    load(dir,"suby.bin",pdm_suby,PDM_SUBY_SIZE);
    pdr_ypixrom=loada(dir,"ysprites_pix.bin",PDR_YPIX_BYTES);
    pdr_brom=loada(dir,"bsprites.bin",PDR_BROM_BYTES);
    pdrift_machine_default_inputs();
    pdrift_machine_init();

    int dumped_flat=0, dumped_hill=0, maxtilt=0;
    for(int fr=0;fr<9000;fr++){
        pdrift_machine_run_frame();
        int32_t dxy=rs(0x3f8), dyx=rs(0x3fa);
        int at=(dxy<0?-dxy:dxy); int bt=(dyx<0?-dyx:dyx); int tilt=at>bt?at:bt;
        if(tilt>maxtilt)maxtilt=tilt;
        int bs=count_bsprites();
        if(fr%300==0) fprintf(stderr,"fr %4d dxy=%7d dyx=%7d tilt=%6d bsprites=%d\n",fr,dxy,dyx,tilt,bs);
        if(fr==300||fr==2400||fr==8700){ char p[64]; snprintf(p,sizeof p,"/tmp/scan_%04d.ppm",fr); dump(p); fprintf(stderr,"DUMP  fr %d tilt=%d bs=%d -> %s\n",fr,tilt,bs,p); }
        (void)dumped_flat;(void)dumped_hill;
    }
    fprintf(stderr,"max tilt over 9000 attract frames = %d (dumped_hill=%d)\n",maxtilt,dumped_hill);
    return 0;
}
