#include "nav.h"
#include "readq.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
  if (argc != 2) {
    printf("usage: %s <test file>\n", argv[0]);
    return 1;
  }

  int fp = open(argv[1], O_RDONLY);

  readq bq;
  init_queue(&bq, fp);

  while (refill_queue(&bq)) {
    printf("buffer: %s\n", bq.buffer);
    process_output(&bq);
  }

  close(fp);

  print_links();

  return 0;
}
