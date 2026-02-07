#include "types.h"

struct procinfo {
  int pid;
  char name[16];
  int state;
  uint64 sz;
};
