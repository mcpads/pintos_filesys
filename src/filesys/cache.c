#include "filesys/cache.h"
#include "threads/thread.h"
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include <list.h>
#include <string.h>
#include "filesys/inode.h"

enum buf_flag_t {
  B_VALID = 0x0, // 00
  B_BUSY = 0x1, // 01
  B_DIRTY = 0x2, // 10
  B_WRITER = 0x4
};


/* Cache Entry */

struct cache_e
{
  struct lock rw_lock;
  struct condition rw_cond;
  int reader_count;

  struct lock cache_lock; // for get free cache

  block_sector_t sec;
  enum buf_flag_t flag; // TODO: Valid? Invalid?
  uint8_t data[BLOCK_SECTOR_SIZE];
  struct list_elem elem;
};

static struct list cache;

// Only used for element of cache
static struct cache_e _cache_buffer[MAX_CACHE_SIZE];


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
 * cache synch functions
 *
 * DESC | lock/unlock implementation with Readers-Writer Algorithm
 *
 */

void cache_write_acquire(struct cache_e* c)
{
  lock_acquire(&c->rw_lock);

  while((c->flag & B_WRITER) ||
	  c->reader_count > 0)
    cond_wait(&c->rw_cond, &c->rw_lock);
  
  c->flag |= B_WRITER;

  lock_release(&c->rw_lock);
}

void cache_write_release(struct cache_e* c)
{

  lock_acquire(&c->rw_lock);
  ASSERT(c->flag & B_WRITER);
  c->flag -= B_WRITER;

  cond_broadcast(&c->rw_cond, &c->rw_lock);
  lock_release(&c->rw_lock);
}

void cache_read_acquire(struct cache_e* c)
{
  lock_acquire(&c->rw_lock);

  while(c->flag & B_WRITER)
    cond_wait(&c->rw_cond, &c->rw_lock);
  c->reader_count++;

  lock_release(&c->rw_lock);
}

void cache_read_release(struct cache_e* c)
{
  lock_acquire(&c->rw_lock);

  ASSERT(c->reader_count > 0);
  if(--c->reader_count == 0)
    cond_signal(&c->rw_cond, &c->rw_lock);

  lock_release(&c->rw_lock);
}


/*
 * cacheWriteBackThread
 *
 * DESC | Write all dirty data per interval. (Clock algorithm)
 *
 * IN   | aux - Dummy NULL pointer
 *
 */
static void cacheWriteBackThread(void* aux UNUSED)
{
  int i;
  while(true){
    timer_sleep(TIMER_FREQ * 10); // TODO: HOW MUCH?
    // since list_elem 'could' rearrange each time, we just use array.
    
    for(i = 0 ; i < MAX_CACHE_SIZE ; i++) {
      cache_read_acquire(_cache_buffer + i);
      if(_cache_buffer[i].flag & B_DIRTY){ 
        block_write(fs_device, _cache_buffer[i].sec, _cache_buffer[i].data);
        _cache_buffer[i].flag -= B_DIRTY;
      }
      cache_read_release(_cache_buffer + i);
    }
  }
}

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
  for(i = 0 ; i < MAX_CACHE_SIZE ; i++){
    lock_init(&_cache_buffer[i].cache_lock);
    lock_init(&_cache_buffer[i].rw_lock);
    cond_init(&_cache_buffer[i].rw_cond);

    _cache_buffer[i].reader_count = 0;
    _cache_buffer[i].sec = -1;

    list_push_back(&cache, &_cache_buffer[i].elem);
  }
  thread_create("cache_wb", PRI_DEFAULT, cacheWriteBackThread, NULL);
}




/*
 * cacheGetFree
 *
 * DESC | Get B_VALID cache element, update each hit
 *
 * RET  | if cache is full, NULL
 *      | else last element of cache mem
 */
static struct cache_e* 
cacheGetFree(void)
{
  int i;
  for(i = 0 ; i < MAX_CACHE_SIZE ; i++){
    if(_cache_buffer[i].flag
        || !lock_try_acquire(&_cache_buffer[i].cache_lock)) continue;
    _cache_buffer[i].flag |= B_BUSY;
    cacheUpdate(&_cache_buffer[i].elem);

    lock_release(&_cache_buffer[i].cache_lock);
    return _cache_buffer + i;
  }
  return NULL;
}

/*
 * cacheGetIdx
 *
 * DESC | Get element which has same values with given, update each hit
 *
 * IN   | sec - given sector number
 *
 * RET  | If fail, NULL
 *      | else, list_elem* of found value
 */
static struct cache_e*
cacheGetIdx(block_sector_t sec)
{
  int i;

  for(i = 0 ; i < MAX_CACHE_SIZE ; i++){
    struct cache_e* temp = _cache_buffer + i;

    if(temp->sec == sec){
      lock_acquire(&temp->cache_lock);
      if(temp->sec != sec){
        lock_release(&temp->cache_lock);
        return NULL;
      }
      else{
        cacheUpdate(&temp->elem);
	lock_release(&temp->cache_lock);
        return temp;
      }
    }
  }
  return NULL;
}

/* structure for load thread */

struct ahead_set
{
  block_sector_t sec;
  struct semaphore* sema;
};

static void cache_eviction(void);



/*
 * cacheLoadThread
 *
 * DESC | read-ahead block with thread_create
 *
 * IN   | aux - pointer of ahead_set
 *
 */

static void cacheLoadThread(void* aux)
{
  struct ahead_set aheadWrap = *(struct ahead_set*)aux;
  free(aux);

  struct cache_e* ahead;


  if(ahead = cacheGetIdx(aheadWrap.sec)
      || aheadWrap.sec >= block_size(fs_device)){
    sema_up(aheadWrap.sema);
    thread_exit();
  }
  
  while((ahead = cacheGetFree()) == NULL)
    cache_eviction();

  ahead->sec = aheadWrap.sec;
  sema_up(aheadWrap.sema);

  cache_write_acquire(ahead);

  block_read(fs_device, aheadWrap.sec, ahead->data);
  cache_write_release(ahead);

  cacheUpdate(&ahead->elem);

  thread_exit();
}

/* 
 * cacheLoadBlock
 *
 * DESC | Load 1 data (+ Read-Ahead) from sector if cache miss occur, 
 *      | and return list_elem* of sector
 *
 * IN   | sec - given sector number
 *
 * RET  | entry* of given value
 *
 */


static struct cache_e*
cacheLoadBlock(block_sector_t sec)
{
  struct semaphore sema1;
  sema_init(&sema1, 0);

  struct cache_e* ndata;
  while((ndata = cacheGetFree()) == NULL)
    cache_eviction(); 


  ndata->sec = sec;

  cache_write_acquire(ndata);
  block_read(fs_device, sec, ndata->data);
  cache_write_release(ndata);
  cacheUpdate(&ndata->elem);

  struct ahead_set* aheadWrap = malloc(sizeof(struct ahead_set));

  if(aheadWrap){
    aheadWrap->sec = sec + 1;
    aheadWrap->sema = &sema1;
    thread_create("ahead_reader", PRI_DEFAULT, cacheLoadThread, aheadWrap);
    sema_down(&sema1);
  }

  return ndata;
}

/* NOTE:
 * every function that use read/write function will take 
 * cache_e size >= BLOCK_SECTOR_SIZE with bounce.
 * Do not have to care about cache_e size. 
 */


/*
 * cache_write
 *
 * DESC | Write Data 'from' and other extra field to valid cache entry.
 *      | Then, Update Cache(MRU), mark DIRTY flag.
 *
 * IN   | sec - given sector number
 *      | from - caller's data
 *
 */
void cache_write(block_sector_t sec, const void* from)
{
  struct cache_e* buffer = cacheGetIdx(sec);
  if(buffer == NULL)
    buffer = cacheLoadBlock(sec);

  // get lock by cacheGetIdx or cacheLoadBlock
  cache_write_acquire(buffer);
  memcpy(buffer->data, from, BLOCK_SECTOR_SIZE);
  buffer->flag |= B_DIRTY;
  cacheUpdate(&buffer->elem);
  cache_write_release(buffer);
}


/*
 * cache_read
 *
 * DESC | Write cache to Data 'to', and fill extra field to valid cache entry.
 *      | Then, Update Cache(MRU).
 *
 * IN   | sec - given sector number
 *      | to - caller's data
 *
 */
void cache_read(block_sector_t sec, void* to)
{
  struct cache_e* buffer = cacheGetIdx(sec);
  if(buffer == NULL)
    buffer = cacheLoadBlock(sec);

  // get lock by cacheGetIdx or cacheLoadBlock
  
  cache_read_acquire(buffer);
  memcpy(to, buffer->data, BLOCK_SECTOR_SIZE);
  cacheUpdate(&buffer->elem);
  cache_read_release(buffer);
}



/*
 * oneblock_init
 *
 * DESC | Init buffer to empty
 *
 * IN   | buffer - given buffer
 *
 */
static void oneblock_release(struct cache_e* buffer)
{
  buffer->sec = -1;
  buffer->flag = B_VALID;
  buffer->reader_count = 0;
//  memset(buffer->data, 0, BLOCK_SECTOR_SIZE); // Really have to do?
}


/*
 * cache_force_one
 *
 * DESC | force one sector of cache to write-back, and initialize.
 *
 * IN   | buffer - given buffer
 *      | force  - if true, ignore buffer flag/readers count.
 *
 */
static bool cache_force_one(struct cache_e* buffer, bool force)
{

  if(!force && (buffer->flag & B_WRITER
      || buffer->reader_count > 0)) return false;

  if(buffer->flag & B_DIRTY){
//if (buffer->sec != 0)
//printf ("buffer->sec: %d\n", buffer->sec);
    buffer->flag -= B_DIRTY;
    block_write(fs_device, buffer->sec, buffer->data);
  }
  oneblock_release(buffer);
  
  return true;

}

/*
 * cache_flush
 *
 * DESC | clear all cache (If dirty, write-back)
 *
 */
void cache_flush(void)
{
  int i;
  for(i = 0 ; i < MAX_CACHE_SIZE ; i++)
    cache_force_one(&_cache_buffer[i].elem, true);
}


/* 
 * cache_eviction 
 *
 * DESC | clear element of list by LRU algorithm
 *
 */
static void cache_eviction(void)
{
  struct list_elem* pos = list_rbegin(&cache);
  while(true){
    struct cache_e* temp = list_entry(pos, struct cache_e, elem);

    if(!lock_try_acquire(&temp->cache_lock))
      continue;
    if(temp->flag == B_VALID){
      lock_release(&temp->cache_lock);
      return;
    }
    else {
      if(cache_force_one(temp, false)){
	if(pos == list_rbegin(&cache))
	  pos = pos->prev;
	else
	  pos = list_rbegin(&cache);
	lock_release(&temp->cache_lock);
	continue;
      }
      lock_release(&temp->cache_lock);
      return;
    }

  }
}
