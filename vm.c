#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "vm_dbg.h"

/* platform dep (unix) */
// #include <signal.h>
// #include <unistd.h>
// #include <fcntl.h>
// #include <sys/time.h>
// #include <sys/types.h>
// #include <sys/termios.h>
// #include <sys/mman.h>

#define NOPS (16)

#define OPC(i) ((i)>>12)
#define DR(i) (((i)>>9)&0x7)
#define SR1(i) (((i)>>6)&0x7)
#define SR2(i) ((i)&0x7)
#define FIMM(i) ((i>>5)&01)
#define IMM(i) ((i)&0x1F)
#define SEXTIMM(i) sext(IMM(i),5)
#define FCND(i) (((i)>>9)&0x7)
#define POFF(i) sext((i)&0x3F, 6)
#define POFF9(i) sext((i)&0x1FF, 9)
#define POFF11(i) sext((i)&0x7FF, 11)
#define FL(i) (((i)>>11)&1)
#define BR(i) (((i)>>6)&0x7)
#define TRP(i) ((i)&0xFF)

typedef void (*op_ex_f)(uint16_t i);
typedef void (*trp_ex_f)();

enum regist { R0 = 0, R1, R2, R3, R4, R5, R6, R7, RPC, RCND, RCNT };
enum flags { FP = 1 << 0, FZ = 1 << 1, FN = 1 << 2 };
enum m_regist { MR_KBSR = 0xFE00, MR_KBDR = 0xFE02 };
enum traps { GETC = 0x20, OUT = 0x21, PUTS = 0x22, IN = 0x23, PUTSP = 0x24, HALT = 0x25 };

uint16_t mem[UINT16_MAX] = {0};
uint16_t reg[RCNT] = {0};
uint16_t PC_START = 0x3000;

static inline uint16_t mr(uint16_t address) { 
    return mem[address]; 
}
static inline void mw(uint16_t address, uint16_t val) { 
    mem[address] = val; 
}
static inline uint16_t sext(uint16_t n, int b) { 
    return ((n>>(b-1))&1) ? (n|(0xFFFF << b)) : n; 
}
static inline void uf(enum regist r) {
    if (reg[r]==0) reg[RCND] = FZ;
    else if (reg[r]>>15) reg[RCND] = FN;
    else reg[RCND] = FP;
}

// Operations
static inline void add(uint16_t i) { 
    reg[DR(i)] = reg[SR1(i)] + (FIMM(i) ? SEXTIMM(i) : reg[SR2(i)]); 
    uf(DR(i)); 
}
static inline void and(uint16_t i) { 
    reg[DR(i)] = SR1(i) & (FIMM(i) ? SEXTIMM(i) : SR2(i)); 
    uf(DR(i)); 
}
static inline void ldi(uint16_t i) { 
    reg[DR(i)] = mr(mr(reg[RPC]+POFF9(i))); 
    uf(DR(i)); 
}
static inline void not(uint16_t i) { 
    reg[DR(i)]=~(SR1(i)); 
    uf(DR(i)); 
}
static inline void br(uint16_t i) { 
    if (reg[RCND] & FCND(i)) {
         reg[RPC] += POFF9(i); 
    } 
}
static inline void jsr(uint16_t i) { 
    reg[R7] = reg[RPC]; 
    reg[RPC] = (FL(i)) ? reg[RPC] + POFF11(i) : BR(i); 
}
static inline void jmp(uint16_t i) { 
    reg[RPC] = reg[BR(i)]; 
}
static inline void ld(uint16_t i) { 
    reg[DR(i)] = mr(reg[RPC] + POFF9(i)); 
    uf(DR(i)); 
}
static inline void ldr(uint16_t i) {
    reg[DR(i)] = mr(reg[SR1(i)] + POFF(i));
    uf(reg[DR(i)]); 
}
static inline void lea(uint16_t i) { 
    reg[DR(i)] =reg[RPC] + POFF9(i); 
    uf(reg[DR(i)]); 
}
static inline void st(uint16_t i) { 
    mw(reg[RPC] + POFF9(i), DR(i)); 
}
static inline void sti(uint16_t i) { 
    mw(mr(reg[RPC] + POFF9(i)), DR(i)); 
}
static inline void str(uint16_t i) { 
    mw(reg[SR1(i)] + POFF(i), DR(i)); 
}
static inline void rti(uint16_t i) {} // unused
static inline void res(uint16_t i) {} // unused

// TRAPS
static inline void trp_getc() {
    reg[R0] = getchar();
}
static inline void trp_out() {
    printf("%c", (char)reg[R0]);
}
static inline void trp_puts() {
    uint16_t *c = mem + reg[R0];
    while(c) {
        printf("%c", (char)*c);
        c++;
    }
}
static inline void trp_in() {
    printf("Enter a character: ");
    char c = getchar();
    putc(c, stdout);
    reg[R0] = (uint16_t)c;
}
static inline void trp_putsp() {
    uint16_t* c = mem + reg[R0];
    while (*c)
    {
        char char1 = (*c) & 0xFF;
        putc(char1, stdout);
        char char2 = (*c) >> 8;
        if (char2) putc(char2, stdout);
        ++c;
    }
    fflush(stdout);
}
bool running = true;
static inline void trp_halt() { 
    running = false;
} 
trp_ex_f trp_ex[6] = { trp_getc, trp_out, trp_puts, trp_in, trp_putsp, trp_halt };
enum { trp_offset = 0x20 };
static inline void trap(uint16_t i) {
    trp_ex[TRP(i)-trp_offset]();
}
op_ex_f op_ex[NOPS] = { /*0*/ br, add, ld, st, jsr, and, ldr, str, rti, not, ldi, sti, jmp, res, lea, trap };

void ld_img(char *fname) {
    FILE *in = fopen(fname, "rb");
    if (NULL==in) {
        fprintf(stderr, "Cannot open file %s.\n", fname);
        exit(1);
    }
    uint16_t *p = mem + PC_START;
    fread(p, sizeof(uint16_t), (UINT16_MAX-PC_START), in);
    fclose(in);
}

void start() {
    reg[RPC] = PC_START;
    while(running) {
        uint16_t i = mr(reg[RPC]++);
        op_ex[OPC(i)](i);
    }
}

int main(void) {
    ld_img("out.obj");
    fprintf(stdout, "-------------------------------------\n");
    fprintf(stdout, "Occupied memory after program load:\n");
    fprintf(stdout, "-------------------------------------\n");
    fprintf_mem_nonzero(stdout, mem, UINT16_MAX);
    start();
    fprintf(stdout, "----------------------------------------\n");
    fprintf(stdout, "Occupied memory after program execution:\n");
    fprintf(stdout, "----------------------------------------\n");
    fprintf_mem_nonzero(stdout, mem, UINT16_MAX);
    fprintf(stdout, "----------------------------------\n");
    fprintf(stdout, "Registers after program execution:\n");
    fprintf(stdout, "----------------------------------\n");
    fprintf_reg_all(stdout, reg, RCNT);
    return 0;
}