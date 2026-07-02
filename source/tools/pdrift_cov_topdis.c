/*
 * pdrift_cov_topdis.c -- disassemble hottest pdrift_host_probe coverage PCs.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"

typedef struct {
    char cpu[8];
    uint32_t pc;
    uint32_t count;
} hit_t;

static uint8_t main_rom[0x080000];
static uint8_t subx_rom[0x040000];
static uint8_t suby_rom[0x040000];

unsigned int m68k_read_disassembler_8(unsigned int address) { (void)address; return 0xff; }
unsigned int m68k_read_disassembler_16(unsigned int address) { (void)address; return 0xffff; }
unsigned int m68k_read_disassembler_32(unsigned int address) { (void)address; return 0xffffffffu; }

static void die_errno(const char *what, const char *path)
{
    fprintf(stderr, "%s %s: %s\n", what, path, strerror(errno));
    exit(1);
}

static void read_file(const char *path, void *dst, size_t bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) die_errno("open", path);
    if (fread(dst, 1, bytes, f) != bytes) {
        fprintf(stderr, "short read: %s\n", path);
        exit(1);
    }
    fclose(f);
}

static const uint8_t *rom_for(const char *cpu, size_t *size)
{
    if (strcmp(cpu, "main") == 0) {
        *size = sizeof main_rom;
        return main_rom;
    }
    if (strcmp(cpu, "subx") == 0) {
        *size = sizeof subx_rom;
        return subx_rom;
    }
    *size = sizeof suby_rom;
    return suby_rom;
}

static int hit_cmp(const void *a, const void *b)
{
    const hit_t *ha = (const hit_t *)a;
    const hit_t *hb = (const hit_t *)b;
    if (ha->count < hb->count) return 1;
    if (ha->count > hb->count) return -1;
    return strcmp(ha->cpu, hb->cpu) ? strcmp(ha->cpu, hb->cpu) : (ha->pc > hb->pc) - (ha->pc < hb->pc);
}

static hit_t *load_hits(const char *path, size_t *out_count)
{
    FILE *f = fopen(path, "r");
    if (!f) die_errno("open", path);
    size_t cap = 8192;
    size_t count = 0;
    hit_t *hits = malloc(cap * sizeof hits[0]);
    if (!hits) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    char cpu[8] = "";
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                size_t n = (size_t)(end - line - 1);
                if (n >= sizeof cpu) n = sizeof cpu - 1;
                memcpy(cpu, line + 1, n);
                cpu[n] = 0;
            }
            continue;
        }
        char *p = line;
        unsigned long pc = strtoul(p, &p, 16);
        unsigned long hit_count = strtoul(p, &p, 10);
        if (!*cpu || hit_count == 0) continue;
        if (count == cap) {
            cap *= 2;
            hit_t *tmp = realloc(hits, cap * sizeof hits[0]);
            if (!tmp) {
                fprintf(stderr, "out of memory\n");
                exit(1);
            }
            hits = tmp;
        }
        snprintf(hits[count].cpu, sizeof hits[count].cpu, "%s", cpu);
        hits[count].pc = (uint32_t)pc;
        hits[count].count = (uint32_t)hit_count;
        count++;
    }
    fclose(f);
    *out_count = count;
    return hits;
}

int main(int argc, char **argv)
{
    const char *romdir = argc > 1 ? argv[1] : "build/pdrift";
    const char *coverage = argc > 2 ? argv[2] : "build/pdrift_cov_1500_play.txt";
    int top = argc > 3 ? atoi(argv[3]) : 40;
    if (top < 1) top = 1;

    char path[1024];
    snprintf(path, sizeof path, "%s/maincpu.bin", romdir);
    read_file(path, main_rom, sizeof main_rom);
    snprintf(path, sizeof path, "%s/subx.bin", romdir);
    read_file(path, subx_rom, sizeof subx_rom);
    snprintf(path, sizeof path, "%s/suby.bin", romdir);
    read_file(path, suby_rom, sizeof suby_rom);

    size_t count = 0;
    hit_t *hits = load_hits(coverage, &count);
    qsort(hits, count, sizeof hits[0], hit_cmp);

    if ((size_t)top > count) top = (int)count;
    for (int i = 0; i < top; i++) {
        size_t rom_size = 0;
        const uint8_t *rom = rom_for(hits[i].cpu, &rom_size);
        char dis[128];
        if (hits[i].pc < rom_size)
            m68k_disassemble_raw(dis, hits[i].pc, rom + hits[i].pc, rom + hits[i].pc + 2, M68K_CPU_TYPE_68000);
        else
            snprintf(dis, sizeof dis, "<outside rom>");
        printf("%-5s pc=%06x count=%10u  %s\n", hits[i].cpu, hits[i].pc, hits[i].count, dis);
    }
    free(hits);
    return 0;
}
