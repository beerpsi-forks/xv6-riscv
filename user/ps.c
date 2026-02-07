#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/getproc.h"

static char *states[] = {
  "unused", "used", "sleep",
  "runnable", "run", "zombie",
};

int
main()
{
  struct procinfo pinfos[64];
  int n = getprocs(pinfos);

  if(n < 0){
    printf("ps: error getting processes\n");
    exit(1);
  }

  printf("PID\tSTATE\tSIZE\tNAME\n");

  for(int i = 0; i < n; i++){
    char *state_str = (pinfos[i].state >= 0 && pinfos[i].state < 6) ? states[pinfos[i].state] : "???";

    printf("%d\t%s\t%lu\t%s\n", pinfos[i].pid, state_str, pinfos[i].sz, pinfos[i].name);
  }

  exit(0);
}
