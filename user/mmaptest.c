#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/mmap.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pid;
  uint64 shared_addr, shared_addr_2;

  printf("Testing mmap shared memory...\n");

  // Map shared memory
  shared_addr = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED);
  shared_addr_2 = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED);
  if(shared_addr == 0 || shared_addr_2 == 0){
    printf("mmap failed\n");
    exit(1);
  }

  printf("Mapped shared memory at 0x%p and 0x%p\n", (void*)shared_addr, (void*)shared_addr_2);

  // Write to shared memory
  int *shared_data = (int*)shared_addr;
  int *shared_data_2 = (int*)shared_addr_2;
  *shared_data = 42;
  *shared_data_2 = 43;
  printf("Parent wrote: %d and %d\n", *shared_data, *shared_data_2);

  // Fork a child process
  pid = fork();
  if(pid < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(pid == 0){
    // Child process
    printf("Child read: %d and %d\n", *shared_data, *shared_data_2);

    // Modify the shared data
    *shared_data = 100;
    *shared_data_2 = 101;
    printf("Child wrote: %d and %d\n", *shared_data, *shared_data_2);

    // Test unmapping in child
    if(munmap(shared_addr) < 0 || munmap(shared_addr_2) < 0)
      printf("Child: munmap failed\n");
    else
      printf("Child: munmap succeeded\n");

    exit(0);
  } else {
    // Parent process
    wait(0);

    // Verify the child's modification is visible
    printf("Parent read after child: %d and %d\n", *shared_data, *shared_data_2);

    // Unmap shared memory
    if(munmap(shared_addr) < 0 || munmap(shared_addr_2) < 0)
      printf("Parent: munmap failed\n");
    else
      printf("Parent: munmap succeeded\n");
  }

  exit(0);
}
