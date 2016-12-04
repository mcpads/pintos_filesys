#include "filesys/cache.h"


struct buf;

static struct buf cache[MAX_CACHE_SIZE];

static int cacheGetFree()
{
  int i;
  for(i = 0 ; i < MAX_CACHE_SIZE ; i++)
    if(cache[i].flag == B_VALID)
      return i;

  return -1;
}

void cache_init()
{
  // Do nothing, 0, 0x00, data 0.
}

void cache_write(block_sector_t , const void*)
{
  // DIRTY O
}

void cache_read(block_sector_t, void*)
{
  // DIRTY X
}

void cache_writeback(block_sector_t)
{
  
}

void cache_flush(void)
{

}
