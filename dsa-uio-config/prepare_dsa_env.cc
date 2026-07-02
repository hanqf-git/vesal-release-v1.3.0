#include <stdio.h>
extern "C" {
#include "dsa_uio_config.h"
}

int main(int argc, char *argv[])
{
  int res = InitDsaConfig();
  int enabled_wq_num = EnableDsaWQs();
  printf("res=%d, enabled_wq_num=%d\n", res, enabled_wq_num);
  if(res<0 || enabled_wq_num <= 0)return -1;
  return 0;
}