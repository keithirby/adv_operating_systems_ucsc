#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "threads/interrupt.h"

/** Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_COUNT 123
#define INDIRECT_COUNT 128

/** On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  off_t length;                       /**< File size in bytes. */
  unsigned magic;                     /**< Magic number. */
  bool directory;

  // New: using indexing direct/indirect blocks
  block_sector_t direct_blocks[DIRECT_COUNT];
  block_sector_t indirect_block;
  block_sector_t double_indirect_block;
};

/** Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/** In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /**< Element in inode list. */
    block_sector_t sector;              /**< Sector number of disk location. */
    int open_cnt;                       /**< Number of openers. */
    bool removed;                       /**< True if deleted, false otherwise. */
    int deny_write_cnt;                 /**< 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /**< Inode content. */

    // NEW 
    off_t length;
  };

/* New: From the index, retrieve the sector */
static block_sector_t get_index_sector(const struct inode_disk *disk, off_t index) {
    printf("(get_index_sector) start\n");

    // index is in direct block
    if (index < DIRECT_COUNT) {
        printf("(get_index_sector) index is in direct block\n");
        return disk->direct_blocks[index];
    }

    // index is in indirect block
    index -= DIRECT_COUNT;
    if (index < INDIRECT_COUNT) {
      // TODO:
      printf("(get_index_sector) index is in indirect block\n");
    }

    // index is in doubly indirect block
    index -= INDIRECT_COUNT;
    if (index < (INDIRECT_COUNT * INDIRECT_COUNT)) {
      // TODO: 
      printf("(get_index_sector) index is in doubly indirect block\n");
    }

    // Handle index out of bounds
    return -1;
}

/** Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length) {
    return get_index_sector(&inode->data, (pos / BLOCK_SECTOR_SIZE));
  }
  else {
    return -1;
  }
}

/** List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/** Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}


/** Allocates the blocks for the inode based on the length **/
static bool inode_allocate(struct inode_disk *disk_inode, off_t length) {
  printf("(inode_allocate) start, length:%u\n", length);
  if (length < 0) {
    return false;
  }
  // string of zeros
  static char zero[BLOCK_SECTOR_SIZE];

  // get how many sectors the write will take
  size_t sector_ct = bytes_to_sectors(length);
  printf("(inode_allocate) sector_ct:%u\n", sector_ct);
  size_t direct_ct = sector_ct;
  size_t indirect_ct = 0;
  size_t dbl_indirect_ct = 0;

  // Determine the count of direct, indirect, and doubly indirect blocks
  if (sector_ct > DIRECT_COUNT) {
    // gonna need indrect blocks
    direct_ct = DIRECT_COUNT;
    sector_ct -= DIRECT_COUNT;
    indirect_ct = sector_ct;
    
    if (sector_ct > INDIRECT_COUNT) {
      // gonna need double indirect blocks
      indirect_ct = INDIRECT_COUNT;
      sector_ct -= INDIRECT_COUNT;
      dbl_indirect_ct = sector_ct;

      if (sector_ct > 0) {
        // not enough space
        return false;
      }
    }
  }

  // write to the direct blocks
  for (size_t i = 0; i < direct_ct; i++) {
    if (disk_inode->direct_blocks[i] == 0) {
      // write to the "1" free direct block
      printf("(inode_allocate) attempting direct allocation (index %d)!\n", i);
      if (!free_map_allocate(1, &disk_inode->direct_blocks[i])) {
        // failed to allocate
        printf("(inode_allocate) failed to allocated!\n", i);
        return false;
      }
      // init the block's values to zero
      buffer_cache_write(disk_inode->direct_blocks[i], zero, 0, BLOCK_SECTOR_SIZE);
    }
  }

  // write to the indirect block
  if (indirect_ct > 0) {
    // TODO:
    printf("(inode_allocate) indirect allocation failed!\n");
    return false;
  }

  // write to the double indirect block
  if (dbl_indirect_ct > 0) {
    // TODO: 
    printf("(inode_allocate) double indirect allocation failed!\n");
    return false;
  }

  printf("(inode_allocate) finished!\n");
  return true;
}


/** Deallocate the blocks for the inode**/
static bool inode_deallocate(struct inode_disk *disk_inode, off_t length) { 
  if (length < 0) {
    return false;
  }

  size_t sector_ct = bytes_to_sectors(length);
  printf("(inode_allocate) sector_ct:%u\n", sector_ct);
  size_t direct_ct = sector_ct;
  size_t indirect_ct = 0;
  size_t dbl_indirect_ct = 0;

  // Determine the count of direct, indirect, and doubly indirect blocks
  if (sector_ct > DIRECT_COUNT) {
    // gonna need indrect blocks
    direct_ct = DIRECT_COUNT;
    sector_ct -= DIRECT_COUNT;
    indirect_ct = sector_ct;
    
    if (sector_ct > INDIRECT_COUNT) {
      // gonna need double indirect blocks
      indirect_ct = INDIRECT_COUNT;
      sector_ct -= INDIRECT_COUNT;
      dbl_indirect_ct = sector_ct;

      if (sector_ct > 0) {
        // not enough space
        return false;
      }
    }
  }
  
  // finally release the data
  for (int i = 0; i < direct_ct; i++) {
    free_map_release(disk_inode->direct_blocks[i], 1);
  }

  // TODO: indirect and double indirect
  return true;
}

/** Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  printf("(inode_create) start, sector %d, length %d!\n", sector, length);
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      // Allocate the blocks for the inode
      if (inode_allocate(disk_inode, disk_inode->length))
        {
          // write to the cache
          printf("(inode_create) writing to cache!\n");
          buffer_cache_write(sector, disk_inode, 0, sizeof(disk_inode));
          printf("(inode_create) success!\n");
          success = true; 
        } 
      free (disk_inode);
    }
  printf("(inode_create) finished!\n");
  return success;
}

/** Reads an inode from SECTOR
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
  // block_read (fs_device, inode->sector, &inode->data);

  buffer_cache_read (inode->sector, &inode->data, 0, 512);
  
  return inode;
}

/** Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/** Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/** Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
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
          inode_deallocate(&inode->data, inode->data.length);
        }

      free (inode); 
    }
}

/** Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/** Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  printf("(read_at) starting...\n");
  while (size > 0)
  {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    /* Read the required part of the sector directly into caller's buffer. */
    //printf("    (read_at) reading sector into callers buffer....\n");
    buffer_cache_read(sector_idx, buffer + bytes_read, sector_ofs, chunk_size);

    /* Advance to the next chunk. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
    //printf(" | done \n");
  }
  printf("(read_at) finshed\n");
  return bytes_read;
}

/** Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at(struct inode *inode, const void *buffer_, off_t size, off_t offset)
{
  printf("(inode_write_at) start!\n");
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt != 0) {
    return 0;
  }

  // extend the file
  // if(byte_to_sector(inode, offset + size - 1) == -1u) {
  //   printf("(inode_write_at) extend file\n");
  //   inode_allocate(&inode->data, size+offset);
  //   inode->data.length = size+offset;
  //   buffer_cache_write(inode->sector, &inode->data, offset, size);
  // }

  while (size > 0)
  {
    printf("(inode_write_at) in loop\n");
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;

    if (chunk_size <= 0) {
      printf("(inode_write_at) chunk_size <= 0\n");
      break;
    }

    /* Write the required part of the sector directly into the cache entry. */
    buffer_cache_write(sector_idx, buffer + bytes_written, sector_ofs, chunk_size);

    /* Advance to the next chunk. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }

  /* Update inode length if we have written past the previous end of the inode. */
  if (offset > inode->data.length) {
    inode->data.length = offset;
    buffer_cache_write(inode->sector, &inode->data, 0, sizeof(inode->data));
  }

  printf("(inode_write_at) bytes written %u\n", bytes_written);

  return bytes_written;
}

/** Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/** Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/** Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
