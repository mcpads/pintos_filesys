#ifndef FILESYS_CACHE_H_
#define FILESYS_CACHE_H_

#include "devices/block.h"
#define MAX_CACHE_SIZE 64

void cache_init(void);
void cache_write(struct block*, block_sector_t, const void*);
void cache_read(struct block*, block_sector_t, void*);
void cache_writeback(struct block*, block_sector_t);
void cache_flush(void);

#endif
