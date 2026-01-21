#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buf[512];

enum {
  CAT_LINENO = 0,
};

int
readline(int fd, char *buf, int maxlen)
{
  int n;
  char c;
  int i = 0;

  /* Read one character at a time from fd */
  while((n = read(fd, &c, 1)) > 0){
    buf[i] = c;
    /* Look for the newline character */
    if(c == '\n')
      /* We are at the end of the line, so stop reading */
      break;
    i += 1;
    /* We don't want to read more characters than we have room */
    if(i >= (maxlen - 1)){
      /* We can't recover, so just print a message and exit */
      fprintf(2, "readline() - line too long\n");
      exit(-1);
    }
  }
  /* This is a little tricky. If read() returns 0 AND we didn't
     read previous characters for this line, then we want to return 0.
     Also, if read returns a value less than 0, we want to return this
     error condition. */
  if(((n == 0) && (i == 0)) || (n < 0))
    return n;

  /* Add the null terminator to the end for the string buffer */
  i += 1;
  buf[i] = '\0';
  return i;
}

void
cat(int fd, unsigned print_type)
{
  int n;
  unsigned lineno = 0, tmp = 0;
  char space = ' ';

  if(print_type & (1 << CAT_LINENO))
    while((n = readline(fd, buf, sizeof(buf))) > 0){
      lineno++;
      tmp = lineno;

      // 6 columns for the line number, left-padded with spaces
      while((tmp *= 10) <= 999999)
        write(1, &space, 1);

      printf("%d  %s", lineno, buf);
    }
  else
    while((n = read(fd, buf, sizeof(buf))) > 0)
      if(write(1, buf, n) != n){
        fprintf(2, "cat: write error\n");
        exit(1);
      }


  if(n < 0){
    fprintf(2, "cat: read error\n");
    exit(1);
  }
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
    case 'n':
      print_type |= 1 << CAT_LINENO;
      break;
    default:
      printf("cat: invalid option %s", argv[optind]);
      exit(22);   // EINVAL
    }
  }

  // Decrease arg count by the number of options we've parsed,
  // and increase the arg pointer by the same amount.
  argc -= optind;
  argv += optind;

  if(argc <= 0){
    cat(0, print_type);
    exit(0);
  }

  for(i = 0; i < argc; i++){
    if((fd = open(argv[i], O_RDONLY)) < 0){
      fprintf(2, "cat: cannot open %s\n", argv[i]);
      exit(1);
    }
    cat(fd, print_type);
    close(fd);
  }
  exit(0);
}
