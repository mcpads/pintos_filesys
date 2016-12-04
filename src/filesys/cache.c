#include "filesys/cache.h"
#include <list.h>
#include <string.h>

enum buf_flag_t {
  B_VALID = 0x0, // 00
  B_BUSY = 0x1, // 01
  B_DIRTY = 0x2, // 10
};

struct buf
{
  struct block* block;
  block_sector_t sec;
  enum buf_flag_t flag; // TODO: Valid? Invalid?
  uint8_t data[BLOCK_SECTOR_SIZE];
  struct list_elem elem;
};


static struct list cache;

// Only used for element of cache
static struct buf _cache_buffer[MAX_CACHE_SIZE];
// TODO: We have to maintain this struct as LRU
//
// Least Recently used value -> end
// if no free slot, rbegin ~ rbegin - 1


/* 
 * cache_init
 *
 * DESC | Initialize cache in first time.
 *
 */
void cache_init()
{
  int i;
  list_init(&cache);
  for(i = 0 ; i < MAX_CACHE_SIZE ; i++)
    list_push_back(&cache, &_cache_buffer[i].elem);
}


/*
 * cacheGetFree
 *
 * DESC | Get B_VALID cache element
 *
 * RET  | if cache is full, NULL
 *      | else last element
 */
static struct list_elem* 
cacheGetFree(void)
{
  struct list_elem* pos = list_rbegin(&cache);
  struct buf* buffer = list_entry(pos, struct buf, elem);
  if(buffer->flag) // VALID
    return NULL;
  return pos;
}


/*
 * cacheGetIdx
 *
 * DESC | Get element which has same values with given
 *
 * IN   | b - given block*
 *      | sec - given sector number
 *
 * RET  | If fail, NULL
 *      | else, list_elem* of found value
 */
static struct list_elem*
cacheGetIdx(struct block* b, block_sector_t sec)
{
  struct list_elem* pos;
  for(pos = list_begin(&cache) ;
	  pos != list_end(&cache) ;
	  pos = pos->next){
    struct buf* temp = list_entry(pos, struct buf, elem);
    if(temp->block == b && temp->sec == sec)
      return pos;
  }
  return NULL;
}



/*
 * cacheUpdate
 *
 * DESC | Move MRU value to first slot.
 *
 * IN   | e - value that just updated (_ -> !B_VALID)
 *
 */
static void
cacheUpdate(struct list_elem* e)
{
  list_remove(e);
  list_push_front(&cache, e);
}


/*
 * cacheUpdateFree
 *
 * DESC | Move Free value to last slot.
 *
 * IN   | e - value that just updated (!B_VALID -> B_VALID)
 *
 */
static void
cacheUpdateFree(struct list_elem* e)
{
  list_remove(e);
  list_push_back(&cache, e);
}



static void cache_eviction(void);


/* 
 * cacheLoadBlock
 *
 * DESC | Load 2 data (Read-Ahead) from block if cache miss occur, 
 *      | and return list_elem* of block
 *
 * IN   | b - given block*
 *      | sec - given sector number
 *
 * RET  | list_elem* of given value
 *
 * TODO: We must use thread_create to load ahead data.
 * Will divide function to Spawn function / thread function.
 */
static struct list_elem*
cacheLoadBlock(struct block* b, block_sector_t sec)
{
  struct list_elem* now;
  while((now = cacheGetFree()) == NULL)
    cache_eviction(); 


  struct list_elem* ahead = now->prev;
  struct buf* ndata = list_entry(now, struct buf, elem);
  struct buf* adata = list_entry(ahead, struct buf, elem);
  // Block read
  
  block_read(b, sec, ndata->data);

  ndata->flag |= B_BUSY;
  ndata->block = b;
  ndata->sec = sec;

  block_read(b, sec+1, adata->data);

  adata->flag |= B_BUSY;
  adata->block = b;
  adata->sec = sec+1;
  
  cacheUpdate(ahead);
  cacheUpdate(now);

  return now;
}

/* NOTE:
 * every function that use read/write function will take 
 * buf size >= BLOCK_SECTOR_SIZE with bounce.
 * Do not have to care about buf size. 
 */


/*
 * cache_write
 *
 * DESC | Write Data 'from' and other extra field to valid cache.
 *      | Then, Update Cache(MRU), mark (BUSY | DIRTY) flag.
 *
 * IN   | b - given block
 *      | sec - given sector number
 *      | from - caller's data
 *
 */
void cache_write(struct block* b, block_sector_t sec, const void* from)
{ 
  struct list_elem* where = cacheGetIdx(b, sec);
  if(where == NULL)
    where = cacheLoadBlock(b, sec);

  struct buf* buffer = list_entry(where, struct buf, elem);

  buffer->flag |= B_DIRTY;

  memcpy(buffer->data, from, BLOCK_SECTOR_SIZE);

  cacheUpdate(where);
}


/*
 * cache_read
 *
 * DESC | Write cache to Data 'to', and fill extra field to valid cache.
 *      | Then, Update Cache(MRU), mark BUSY flag.
 *
 * IN   | b - given block
 *      | sec - given sector number
 *      | to - caller's data
 *
 */
void cache_read(struct block* b, block_sector_t sec, void* to)
{
  struct list_elem* where = cacheGetIdx(b, sec);
  if(where == NULL)
    where = cacheLoadBlock(b, sec);

  struct buf* buffer = list_entry(where, struct buf, elem);

  memcpy(to, buffer->data, BLOCK_SECTOR_SIZE);

  cacheUpdate(where);
}

/*
 * cache_writeback
 *
 * DESC | Find cache that has given values. Then block_write to block.
 *
 * IN   | b - given block
 *      | sec - given sector number
 *
 * TODO: Multi-thread
 */
void cache_writeback(struct block* b, block_sector_t sec)
{
  struct list_elem* pos = cacheGetIdx(b, sec);
  if(pos != NULL){
    struct buf* buffer = list_entry(pos, struct buf, elem);
    if(buffer->flag & B_DIRTY) return;
    block_write(b, sec, buffer->data);
  }
}

/*
 * oneblock_init
 *
 * DESC | Init buffer to empty
 *
 * IN   | buffer - given buffer
 *
 */
static void oneblock_init(struct buf* buffer)
{
  buffer->block = NULL;
  buffer->sec = 0;
  buffer->flag = B_VALID;
  memset(buffer->data, 0, BLOCK_SECTOR_SIZE); // Really have to do?
}


/*
 * cache_force_one
 *
 * DESC | force one sector of cache to write-back, and initialize.
 *      | "force clear"
 *
 * IN   | pos - list_elem* of cache
 *
 */
static void cache_force_one(struct list_elem* pos)
{
  struct buf* buffer = list_entry(pos, struct buf, elem);

  if(buffer->flag & B_DIRTY)
    block_write(buffer->block, buffer->sec, buffer->data);

  oneblock_init(buffer);
}

/*
 * cache_flush
 *
 * DESC | clear all cache (If dirty, write-back)
 *
 */
void cache_flush(void)
{
  struct list_elem* pos;
  for(pos = list_begin(&cache) ; 
      pos != list_end(&cache);
      pos = pos->next)
    cache_force_one(pos);
}


/* 
 * cache_eviction 
 *
 * DESC | clear 2 element of list by LRU algorithm
 *
 */
static void cache_eviction(void)
{
  struct list_elem* pos = list_rbegin(&cache);
  struct list_elem* n = pos->prev;
  cache_force_one(pos);
  cache_force_one(n);
>>>>>>> cache test
}
