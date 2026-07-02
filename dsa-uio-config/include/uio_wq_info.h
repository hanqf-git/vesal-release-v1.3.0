#ifndef __UIO_WQ_INFO_H__
#define __UIO_WQ_INFO_H__

struct UioWqInfo {
  char *bdf;
  int numa_node;
  void* portal;
};

#endif