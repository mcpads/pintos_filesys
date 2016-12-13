#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "devices/block.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define NUM_OF_DIRECTS 10
#define NUM_OF_INDIRECTS 10
#define MAX_DIRECTS 5120
#define MAX_INDIRECTS 5120*128

void COND_block_write(struct block* b, block_sector_t sec, const void* from)
{
#ifdef FILESYS
  cache_write(sec, from);
#else
  block_write(b, sec, from);
#endif
}

void COND_block_read(struct block* b, block_sector_t sec, void* to)
{
#ifdef FILESYS
  cache_read(sec, to);
#else
  block_read(b, sec, to);
#endif
}



/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  block_sector_t start;               /* First data sector. */
  off_t length;                       /* File size in bytes. */
  unsigned magic;                     /* Magic number. */
  uint32_t unused[103];               /* Not used. */

  /* For Subdirectory */
  block_sector_t parent_dir; // if it is not a dir, parent dir : -1.

  block_sector_t d_blocks[NUM_OF_DIRECTS]; // Direct blocks refering.
  block_sector_t ind_blocks[NUM_OF_INDIRECTS]; // Indirect blocks refering.
  block_sector_t d_ind_blocks; // Double indirect blocks refering.
};

/*
 * inode_disk_indirect
 *
 * DESC | Structure on disk containing bunch of block indices of direct blocks.
 *      | Must be BLOCK_SECTOR_SIZE bytes long.
 */
struct inode_disk_indirect
{
  block_sector_t d_blocks[128];
};

/*
 * inode_disk_double_indirect
 *
 * DESC | Structure on disk containing bunch of block indices of indirect
 *      | blocks. Must be BLOCK_SECTOR_SIZE bytes long.
 */
struct inode_disk_double_indirect
{
  block_sector_t ind_blocks[128];
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
  static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
{
  struct list_elem elem;              /* Element in inode list. */
  block_sector_t sector;              /* Sector number of disk location. */
  int open_cnt;                       /* Number of openers. */
  bool removed;                       /* True if deleted, false otherwise. */
  int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
  struct inode_disk data;             /* Inode content. */
};

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */

/* MODIFIED
 * byte_to_sector
 * 
 * DSEC | By accessing in the order of (DIRECTS -> INDIRECTS -> DOUBLE),
 *      | find the sector index where pos located in.
 */
  static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (inode->data.length == 0)
    return -1;
  // If the pos in the range of DIRECTS (<=512*10)
  if (pos < MAX_DIRECTS) {
    uint32_t d_idx = inode->data.d_blocks[pos / BLOCK_SECTOR_SIZE];
    ASSERT (d_idx != -1);
    return d_idx;
  }
  // else if the pos is in the range of INDIRECTS (5120< <=512*128*10)
  else if (pos < MAX_DIRECTS + MAX_INDIRECTS) {
    block_sector_t ind_idx = inode->data.ind_blocks[(pos - MAX_DIRECTS) / 
      (BLOCK_SECTOR_SIZE*128)];
    ASSERT (ind_idx != -1);
    off_t remaining = (pos - MAX_DIRECTS) % (BLOCK_SECTOR_SIZE*128);
    struct inode_disk_indirect idi;
    COND_block_read (fs_device, ind_idx, &idi);
    block_sector_t d_idx = idi.d_blocks[remaining / BLOCK_SECTOR_SIZE];
    ASSERT (d_idx != -1);
    return d_idx;
  }
  // else
  else if (pos < inode->data.length) {
    block_sector_t d_ind_idx = inode->data.d_ind_blocks;
    ASSERT (d_ind_idx != -1);
    struct inode_disk_double_indirect iddi;
    COND_block_read (fs_device, d_ind_idx, &iddi);
    block_sector_t ind_idx = iddi.ind_blocks[(pos-MAX_DIRECTS-MAX_INDIRECTS) /
      (BLOCK_SECTOR_SIZE*128)];
    ASSERT (ind_idx != -1);
    off_t remaining = (pos-MAX_DIRECTS-MAX_INDIRECTS) % (BLOCK_SECTOR_SIZE*128);
    struct inode_disk_indirect idi;
    COND_block_read (fs_device, ind_idx, &idi);
    block_sector_t d_idx = idi.d_blocks[remaining / BLOCK_SECTOR_SIZE];
    ASSERT (d_idx != -1);
    return d_idx; 
  }
  else {
    return -1;
  }

  /*  if (pos < inode->data.length)
      return inode->data.start + pos / BLOCK_SECTOR_SIZE;
      else
      return -1;
      */
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
  void
inode_init (void) 
{
  list_init (&open_inodes);
#ifdef FILESYS
  cache_init();
#endif
}

block_sector_t inode_get_parent(const struct inode* inode)
{
  return inode->data.parent_dir;
}

bool inode_is_dir(const struct inode* inode)
{
  return inode->data.parent_dir != -1;
}

int inode_get_open_cnt(const struct inode* inode)
{
  return inode->open_cnt;
}


/* 
 * init_block_sectors
 * 
 * DSEC | Init blocks in inode_dist to -1.
 */
  void
init_blocks (block_sector_t* blocks, int num)
{
  int i;
  for (i=0; i<num; i++)
    blocks[i] = -1;
}

/* 
 * allocate_inode_data
 * 
 * DSEC | Allocate inode data to inode_disk.
 */
  bool
allocate_inode_data (struct inode_disk* id, block_sector_t sectors, int start)
{
  bool success = false;
  static char zeros[BLOCK_SECTOR_SIZE];
  int i;
  struct inode_disk_indirect idi;
  if (start < 10)
  {
    // Do nothing
  }
  else if (start < 1290)
  {
    block_sector_t start_sec = id->ind_blocks[(start-10) / 128];
    if (start_sec != -1)
      COND_block_read (fs_device, start_sec, &idi);
  }
  else
  {
    // Do nothing
  }

  struct inode_disk_double_indirect iddi;
  if (id->d_ind_blocks == -1)
    init_blocks (iddi.ind_blocks, 128);
  else
  {
    COND_block_read (fs_device, id->d_ind_blocks, &iddi);
    if (start >= 1290)
    {
      block_sector_t start_sec = iddi.ind_blocks[(start-1290) / 128];
      if (start_sec != -1)
        COND_block_read (fs_device, start_sec, &idi);
    }
  }

  for (i = start; i < sectors; i++)
  {
    if (i < 10)
    {
      if (!free_map_allocate (1, &id->d_blocks[i])) return false;
      COND_block_write (fs_device, id->d_blocks[i], zeros);
    }
    else if (i < 1290)
    {
      int i_a = i - 10;
      block_sector_t sec = i_a % 128;
      if (sec == 0)
      {
        if (!free_map_allocate (1, &id->ind_blocks[i_a / 128]))
          return false;
        init_blocks (idi.d_blocks, 128);
      }
      ASSERT (idi.d_blocks[sec] == -1);
      if (!free_map_allocate (1, &idi.d_blocks[sec])) return false;
      COND_block_write (fs_device, idi.d_blocks[sec], zeros);
      if (sec == 127 || i == sectors - 1)
        COND_block_write (fs_device, id->ind_blocks[i_a / 128], &idi);
    }
    else
    {
      if (i == 1290)
      {
        if (!free_map_allocate (1, &id->d_ind_blocks))
          return false;
      }
      int i_a = i - 1290;
      block_sector_t sec = i_a % 128;
      if (sec == 0)
      {
        if (!free_map_allocate (1, &iddi.ind_blocks[i_a / 128]))
          return false;
        init_blocks (idi.d_blocks, 128);
      }
      ASSERT (idi.d_blocks[sec] == -1);
      if (!free_map_allocate (1, &idi.d_blocks[sec])) return false;
      COND_block_write (fs_device, idi.d_blocks[sec], zeros);
      if (sec == 127 || i == sectors - 1)
        COND_block_write (fs_device, iddi.ind_blocks[i_a / 128], &idi); 
      if (i == sectors - 1)
        COND_block_write (fs_device, id->d_ind_blocks, &iddi);
    }
  }
  return true; 
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */

/* MODIFIED
 * inode_create
 * 
 * DSEC | Allocate disk sector one by one rather than consecutive sectors.
 */
  bool
inode_create (block_sector_t sector, off_t length, block_sector_t parent)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
  {
    size_t sectors = bytes_to_sectors (length);
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    init_blocks (disk_inode->d_blocks, 10);
    init_blocks (disk_inode->ind_blocks, 10);
    disk_inode->d_ind_blocks = -1;
    disk_inode->parent_dir = parent; // if file, -1. if dir, none -1
    success = allocate_inode_data (disk_inode, sectors, 0);
    COND_block_write (fs_device, sector, disk_inode);
    free (disk_inode);
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
  struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
      e = list_next (e)) 
  {
    inode = list_entry (e, struct inode, elem);
    if (inode->sector == sector) 
    {
      inode_reopen (inode);
      return inode; 
    }
  }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  COND_block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
  struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
  block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */

/* MODIFIED
 * inode_close
 * 
 * DSEC | When inode is removed, sector indices are not in order unlike before,
 *      | so they have to be released in certain order we specified.
 */
  void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
  {
    /* Remove from inode list and release lock. */
    list_remove (&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) 
    {
      free_map_release (inode->sector, 1);
      //          free_map_release (inode->data.start,
      //                            bytes_to_sectors (inode->data.length));
      int i;
      for (i = 0; i < NUM_OF_DIRECTS; i++)
      {
        if (inode->data.d_blocks[i] != -1)
        {
          free_map_release (inode->data.d_blocks[i], 1);
        }
        else
          break;
      }
      for (i = 0; i < NUM_OF_INDIRECTS; i++)
      {
        if (inode->data.ind_blocks[i] != -1)
        {
          block_sector_t sec = inode->data.ind_blocks[i];
          struct inode_disk_indirect idi;
          COND_block_read (fs_device, sec, &idi);
          int j;
          bool flag = false;
          for (j = 0; j < 128; j++)
          {
            if (idi.d_blocks[j] != -1)
              free_map_release (idi.d_blocks[j], 1);
            else
            {
              flag = true;
              break;
            }
          }
          free_map_release (inode->data.ind_blocks[i], 1);
          if (flag)
            break;
        }
        else
          break;
      }
      if (inode->data.d_ind_blocks != -1)
      {
        struct inode_disk_double_indirect iddi;
        COND_block_read (fs_device, inode->data.d_ind_blocks, &iddi);
        int m;
        for (m = 0; m < 128; m++)
        {
          if (iddi.ind_blocks[m] != -1)
          {
            block_sector_t sec = iddi.ind_blocks[m];
            struct inode_disk_indirect idi;
            COND_block_read (fs_device, sec, &idi);
            int n;
            bool flag = false;
            for (n = 0; n < 128; n++)
            {
              if (idi.d_blocks[n] != -1)
                free_map_release (idi.d_blocks[n], 1);
              else
              {
                flag = true;
                break;
              }
            }
            free_map_release (iddi.ind_blocks[m], 1);
            if (flag)
              break;
          }
          else
            break;
        }
        free_map_release (inode->data.d_ind_blocks, 1);
      }
    }

    free (inode); 
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
  void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
  off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  if (inode->sector == 0) 
    {
//printf ("size: %d, off: %d, len: %d, by: %d, inode_sec: %d\n", size, offset, inode_length(inode), bytes_read, inode->sector);
//      COND_block_read (fs_device, inode->sector, buffer);
//      return size;
    }

  while (size > 0) 
  {
    if (offset > inode->data.length && inode->sector != 0) break;

    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector (inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length (inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
    {
      /* Read full sector directly into caller's buffer. */
      COND_block_read (fs_device, sector_idx, buffer + bytes_read);
    }
    else 
    {
      /* Read sector into bounce buffer, then partially copy
         into caller's buffer. */
      if (bounce == NULL) 
      {
        bounce = malloc (BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }
      COND_block_read (fs_device, sector_idx, bounce);
      memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */

/* MODIFIED
 * inode_write_at
 * 
 * DSEC | Implement file growth at the end of the file. 
 */
  off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
    off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt || fs_device == block_size (fs_device))
    return 0;

  // Filling with zeros within gap
  if (offset + size > inode_length (inode))
  {
    int sectors = bytes_to_sectors (offset + size);
    int start = bytes_to_sectors (inode_length (inode));
    if (!allocate_inode_data (&inode->data, sectors, start)) ASSERT (0);
    inode->data.length += offset - inode->data.length + size;
    COND_block_write (fs_device, inode->sector, &inode->data);
  }

  while (size > 0) 
  {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector (inode, offset);
    ASSERT (sector_idx != -1);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length (inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;

    if (chunk_size <= 0)
      ASSERT (0);

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
    {
      /* Write full sector directly to disk. */
      COND_block_write (fs_device, sector_idx, buffer + bytes_written);
    }
    else 
    {
      /* We need a bounce buffer. */
      if (bounce == NULL) 
      {
        bounce = malloc (BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }

      /* If the sector contains data before or after the chunk
         we're writing, then we need to read in the sector
         first.  Otherwise we start with a sector of all zeros. */
      if (sector_ofs > 0 || chunk_size < sector_left) 
        COND_block_read (fs_device, sector_idx, bounce);
      else
        memset (bounce, 0, BLOCK_SECTOR_SIZE);
      memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
      COND_block_write (fs_device, sector_idx, bounce);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
  void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
  void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
  off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
