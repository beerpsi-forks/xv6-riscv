#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pid;
  uint64 shared_addr;

  printf("Testing mmap shared memory...\n");

  // Map shared memory
  shared_addr = mmap();
  if(shared_addr == 0){
    printf("mmap failed\n");
    exit(1);
  }

  printf("Mapped shared memory at 0x%p\n", (void*)shared_addr);

  // Write to shared memory
  int *shared_data = (int*)shared_addr;
  *shared_data = 42;
  printf("Parent wrote: %d\n", *shared_data);

  // Fork a child process
  pid = fork();
  if(pid < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(pid == 0){
    // Child process
    printf("Child read: %d\n", *shared_data);

    // Modify the shared data
    *shared_data = 100;
    printf("Child wrote: %d\n", *shared_data);

    // Test unmapping in child
    if(munmap(shared_addr) < 0)
      printf("Child: munmap failed\n");
    else
      printf("Child: munmap succeeded\n");

    exit(0);
  } else {
    // Parent process
    wait(0);

    // Verify the child's modification is visible
    printf("Parent read after child: %d\n", *shared_data);

    // Unmap shared memory
    if(munmap(shared_addr) < 0)
      printf("Parent: munmap failed\n");
    else
      printf("Parent: munmap succeeded\n");
  }

  exit(0);
}
