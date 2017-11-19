#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

char * seed_file = "test-seed";

int main(int argc, char *argv[])
{
  int fd;
  char keypool[600];

  fd = open( seed_file, O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR );
  if (fd < 0)
    return errno;

  write(fd, keypool, sizeof(keypool));

  close (fd);

  return 0;
}
