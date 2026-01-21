#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buf[512];

enum {
  WC_LINES = 0,
  WC_WORDS = 1,
  WC_BYTES = 2,
  NUM_WCS = 3,
};

void
wc(int fd, char *name, unsigned print_type)
{
  int i, n;
  int inword;
  int counts[NUM_WCS];
  char* fmt = "%d";

  inword = 0;
  while((n = read(fd, buf, sizeof(buf))) > 0)
    for(i=0; i<n; i++){
      counts[WC_BYTES]++;
      if(buf[i] == '\n')
        counts[WC_LINES]++;
      if(strchr(" \r\t\n\v", buf[i]))
        inword = 0;
      else if(!inword){
        counts[WC_WORDS]++;
        inword = 1;
      }
    }
  if(n < 0){
    printf("wc: read error\n");
    exit(1);
  }

  for(i = 0; i < NUM_WCS; i++)
    if(print_type & (1 << i)){
      printf(fmt, counts[i]);
      fmt = " %d";
    }
  printf(" %s\n", name);
}

int
main(int argc, char *argv[])
{
  int fd, i;
  unsigned print_type = 0, optind = 1;

  // Argument parsing.
  // TODO: Handle options being intermixed with arguments by moving
  // options to the front of argv.
  for(optind = 1; optind < argc; optind++){
    if(argv[optind][0] != '-' || argv[optind][1] == '\0')break;

    switch(argv[optind][1]){
    case 'l':
      print_type |= 1 << WC_LINES;
      break;
    case 'w':
      print_type |= 1 << WC_WORDS;
      break;
    case 'c':
      print_type |= 1 << WC_BYTES;
      break;
    default:
      printf("wc: invalid option %s", argv[optind]);
      exit(22);   // EINVAL
    }
  }

  // Decrease arg count by the number of options we've parsed,
  // and increase the arg pointer by the same amount.
  argc -= optind;
  argv += optind;

  if(print_type == 0)
    print_type = (1 << WC_LINES) | (1 << WC_WORDS) | (1 << WC_BYTES);

  if(argc <= 0){
    wc(0, "", print_type);
    exit(0);
  }

  for(i = 0; i < argc; i++){
    if((fd = open(argv[i], O_RDONLY)) < 0){
      printf("wc: cannot open %s\n", argv[i]);
      exit(1);
    }
    wc(fd, argv[i], print_type);
    close(fd);
  }
  exit(0);
}
