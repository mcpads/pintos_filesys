#ifdef FILESYS_CACHE_H_
#define FILESYS_CACHE_H

#include "devices/block.h"
#define MAX_CACHE_SIZE 64

enum buf_flag_t {
  B_VALID = 0x00,
  B_BUSY = 0x01,
  B_DIRTY = 0x02,
};

struct buf
{
  block_sector_t idx;
  enum buf_flag_T flag;
  char data[BLOCK_SECTOR_SIZE];
};

void cache_init(void);
void cache_write(block_sector_t, const void*);
void cache_read(block_sector_t, void*);
void cache_writeback(block_sector_t);
void cache_flush(void);


#endif
