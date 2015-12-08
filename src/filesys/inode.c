#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

// 128 pointers per block
#define INDIRECT_PTRS 128

// 4 DIRECT, 9 INDIRECT, 1 DOUBLE INDIRECT = 14 BLOCK PTRS
#define DIRECT_BLOCKS 4
#define INDIRECT_BLOCKS 9
#define DOUBLE_DIRECT_BLOCKS 1
#define INODE_PTRS 14


/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    // block_sector_t start;               /* First data sector. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[107];               /* Not used. */

    uint32_t direct_index;
    uint32_t indirect_index;
    uint32_t double_indirect_index;
    block_sector_t blocks[14];
    bool is_dir;
    block_sector_t parent;
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
    // struct inode_disk data;             /* Inode content. */

    off_t length;                       /* File size in bytes. */
    off_t read_length;
    uint32_t direct_index;
    uint32_t indirect_index;
    uint32_t double_indirect_index;
    block_sector_t blocks[14];
    bool is_dir;
    block_sector_t parent;
    struct lock lock;
  };

//ADDED
bool inode_alloc (struct inode_disk *inode_disk);
off_t inode_grow (struct inode* inode, off_t length);
void inode_free (struct inode *inode);

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t length, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < length)
  {
    uint32_t idx;
    uint32_t blocks[INDIRECT_PTRS];

    // direct blocks
    if (pos < DIRECT_BLOCKS * BLOCK_SECTOR_SIZE)
    {
      return inode->blocks[pos / BLOCK_SECTOR_SIZE];
    }

    // indirect blocks
    else if (pos < (DIRECT_BLOCKS + INDIRECT_BLOCKS * INDIRECT_PTRS)
      * BLOCK_SECTOR_SIZE)
    {
      // read up corresponding indirect block
      pos -= DIRECT_BLOCKS * BLOCK_SECTOR_SIZE;
      idx = pos / (INDIRECT_PTRS * BLOCK_SECTOR_SIZE) + DIRECT_BLOCKS;
      block_read(fs_device, inode->blocks[idx], &blocks);

      pos %= INDIRECT_PTRS * BLOCK_SECTOR_SIZE;
      return blocks[pos / BLOCK_SECTOR_SIZE];
    }

    // double indirect blocks
    else
    {
      // first level block
      block_read(fs_device, inode->blocks[INODE_PTRS - 1], &blocks);

      // second level block
      pos -= (DIRECT_BLOCKS + INDIRECT_BLOCKS * INDIRECT_PTRS) * BLOCK_SECTOR_SIZE;
      idx = pos / (INDIRECT_PTRS * BLOCK_SECTOR_SIZE);
      block_read(fs_device, blocks[idx], &blocks);

      pos %= INDIRECT_PTRS * BLOCK_SECTOR_SIZE;
      return blocks[pos / BLOCK_SECTOR_SIZE];
    }
  }
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool is_dir)
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
      // ADDED
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
      disk_inode->parent = ROOT_DIR_SECTOR;
      if (inode_alloc(disk_inode)) 
        {
          block_write (fs_device, sector, disk_inode);
          success = true; 
        } 
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

  /* Initialize. */ // ADDED
  struct inode_disk inode_disk;

  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->lock);

  // copy disk data to inode
  block_read(fs_device, inode->sector, &inode_disk);
  inode->length = inode_disk.length;
  inode->read_length = inode_disk.length;
  inode->direct_index = inode_disk.direct_index;
  inode->indirect_index = inode_disk.indirect_index;
  inode->double_indirect_index = inode_disk.double_indirect_index;
  inode->is_dir = inode_disk.is_dir;
  inode->parent = inode_disk.parent;
  memcpy(&inode->blocks, &inode_disk.blocks, INODE_PTRS * sizeof(block_sector_t));
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
void
inode_close (struct inode *inode) 
{
  struct inode_disk inode_disk;

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
          // free_map_release (inode->data.start,
          //                   bytes_to_sectors (inode->data.length)); 
          // ADDED
          inode_free(inode);
        }
      else // write back
        {
          inode_disk.length = inode->length;
          inode_disk.magic = INODE_MAGIC;
          inode_disk.direct_index = inode->direct_index;
          inode_disk.indirect_index = inode->indirect_index;
          inode_disk.double_indirect_index = inode->double_indirect_index;
          inode_disk.is_dir = inode->is_dir;
          inode_disk.parent = inode->parent;
          memcpy(&inode_disk.blocks, &inode->blocks,
            INODE_PTRS * sizeof(block_sector_t));
          block_write(fs_device, inode->sector, &inode_disk);
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
  // uint8_t *bounce = NULL;

  off_t length = inode->read_length;

  if(offset >= length)
    return 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, length, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      // ADDED
      int cache_idx = access_cache_entry(sector_idx, true);
      memcpy(buffer + bytes_read, cache_array[cache_idx].block + sector_ofs,
       chunk_size);
      cache_array[cache_idx].accessed = true;
      cache_array[cache_idx].open_cnt--;

      // if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        // {
          /* Read full sector directly into caller's buffer. */
      //     block_read (fs_device, sector_idx, buffer + bytes_read);
      //   }
      // else 
      //   {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
        //   if (bounce == NULL) 
        //     {
        //       bounce = malloc (BLOCK_SECTOR_SIZE);
        //       if (bounce == NULL)
        //         break;
        //     }
        //   block_read (fs_device, sector_idx, bounce);
        //   memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        // }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  // free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  // uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  //ADDED
  /* beyond EOF, need extend */
  if(offset + size > inode_length(inode))
  {
    // no sync required for dirs
    if(!inode->is_dir)
      lock_acquire(&inode->lock);

    inode->length = inode_grow(inode, offset + size);

    if(!inode->is_dir)
      lock_release(&inode->lock);
  }


  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, inode_length(inode),
       offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      // ADDED
      int cache_idx = access_cache_entry(sector_idx, true);
      memcpy(cache_array[cache_idx].block + sector_ofs, buffer + bytes_written, chunk_size);
      cache_array[cache_idx].accessed = true;
      cache_array[cache_idx].dirty = true;
      cache_array[cache_idx].open_cnt--;

      // if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        // {
          /* Write full sector directly to disk. */
          // block_write (fs_device, sector_idx, buffer + bytes_written);
        // }
      // else 
        // {
          /* We need a bounce buffer. */
          // if (bounce == NULL) 
          //   {
          //     bounce = malloc (BLOCK_SECTOR_SIZE);
          //     if (bounce == NULL)
          //       break;
          //   }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
        //   if (sector_ofs > 0 || chunk_size < sector_left) 
        //     block_read (fs_device, sector_idx, bounce);
        //   else
        //     memset (bounce, 0, BLOCK_SECTOR_SIZE);
        //   memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
        //   block_write (fs_device, sector_idx, bounce);
        // }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  // free (bounce);
  inode->read_length = inode_length(inode);
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
  return inode->length;
}

// ADDED
bool
inode_alloc (struct inode_disk *inode_disk)
{
  struct inode inode;
  inode.length = 0;
  inode.direct_index = 0;
  inode.indirect_index = 0;
  inode.double_indirect_index = 0;

  inode_grow(&inode, inode_disk->length);
  inode_disk->direct_index = inode.direct_index;
  inode_disk->indirect_index = inode.indirect_index;
  inode_disk->double_indirect_index = inode.double_indirect_index;
  memcpy(&inode_disk->blocks, &inode.blocks, INODE_PTRS * sizeof(block_sector_t));
  return true;
}


off_t
inode_grow (struct inode *inode, off_t length)
{
  static char zeros[BLOCK_SECTOR_SIZE];

  size_t grow_sectors = bytes_to_sectors(length) - bytes_to_sectors(inode->length);

  if (grow_sectors == 0)
  {
    return length;
  }

  // direct blocks (index < 4)
  while (inode->direct_index < DIRECT_BLOCKS && grow_sectors != 0)
  {
    free_map_allocate (1, &inode->blocks[inode->direct_index]);
    block_write(fs_device, inode->blocks[inode->direct_index], zeros);
    inode->direct_index++;
    grow_sectors--;
  }

  // indirect blocks (index < 13)
  while (inode->direct_index < DIRECT_BLOCKS + INDIRECT_BLOCKS
    && grow_sectors != 0)
  {
    block_sector_t blocks[128];

    // read up first level block
    if (inode->indirect_index == 0)
      free_map_allocate(1, &inode->blocks[inode->direct_index]);
    else
      block_read(fs_device, inode->blocks[inode->direct_index], &blocks);

    // expand
    while (inode->indirect_index < INDIRECT_PTRS && grow_sectors != 0)
    {
      free_map_allocate(1, &blocks[inode->indirect_index]);
      block_write(fs_device, blocks[inode->indirect_index], zeros);
      inode->indirect_index++;
      grow_sectors--;
    }
    
    // write back expanded blocks
    block_write(fs_device, inode->blocks[inode->direct_index], &blocks);

    // go to new indirect block
    if (inode->indirect_index == INDIRECT_PTRS)
    {
      inode->indirect_index = 0;
      inode->direct_index++;
    }
  }

  // double indirect blocks
  if (inode->direct_index == INODE_PTRS - 1 && grow_sectors != 0)
  {
    block_sector_t level_one[128];
    block_sector_t level_two[128];

    // read up first level block
    if (inode->double_indirect_index == 0 && inode->indirect_index == 0)
      free_map_allocate(1, &inode->blocks[inode->direct_index]);
    else
      block_read(fs_device, inode->blocks[inode->direct_index], &level_one);

    // expand
    while (inode->indirect_index < INDIRECT_PTRS && grow_sectors != 0)
    {
      // read up second level block
      if (inode->double_indirect_index == 0)
        free_map_allocate(1, &level_one[inode->indirect_index]);
      else
        block_read(fs_device, level_one[inode->indirect_index], &level_two);

      // expand
      while (inode->double_indirect_index < INDIRECT_PTRS && grow_sectors != 0)
      {
        free_map_allocate(1, &level_two[inode->double_indirect_index]);
        block_write(fs_device, level_two[inode->double_indirect_index], zeros);
        inode->double_indirect_index++;
        grow_sectors--;
      }

      // write back level two blocks
      block_write(fs_device, level_one[inode->indirect_index], &level_two);

      // go to new level two blocks
      if (inode->double_indirect_index == INDIRECT_PTRS)
      {
        inode->double_indirect_index = 0;
        inode->indirect_index++;
      }
    }

    // write back level one blocks
    block_write(fs_device, inode->blocks[inode->direct_index], &level_one);
  }
  
  return length;
}

void
inode_free (struct inode *inode)
{
  size_t sector_num = bytes_to_sectors(inode->length);
  size_t idx = 0;
  
  if(sector_num == 0)
  {
    return;
  }

  // free direct blocks (index < 4)
  while (idx < DIRECT_BLOCKS && sector_num != 0)
  {
    free_map_release (inode->blocks[idx], 1);
    sector_num--;
    idx++;
  }

  // free indirect blocks (index < 13)
  while (inode->direct_index >= DIRECT_BLOCKS && 
    idx < DIRECT_BLOCKS + INDIRECT_BLOCKS && sector_num != 0)
  {
    size_t free_blocks = sector_num < INDIRECT_PTRS ?  sector_num : INDIRECT_PTRS;

    size_t i;
    block_sector_t block[128];
    block_read(fs_device, inode->blocks[idx], &block);

    for (i = 0; i < free_blocks; i++)
    {
      free_map_release(block[i], 1);
      sector_num--;
    }

    free_map_release(inode->blocks[idx], 1);
    idx++;
  }

  // free double indirect blocks (index 13)
  if (inode->direct_index == INODE_PTRS - 1)
  {
    size_t i, j;
    block_sector_t level_one[128], level_two[128];

    // read up first level block
    block_read(fs_device, inode->blocks[INODE_PTRS - 1], &level_one);

    // calculate # of indirect blocks
    size_t indirect_blocks = DIV_ROUND_UP(sector_num, INDIRECT_PTRS * BLOCK_SECTOR_SIZE);

    for (i = 0; i < indirect_blocks; i++)
    {
      size_t free_blocks = sector_num < INDIRECT_PTRS ? sector_num : INDIRECT_PTRS;
      
      // read up second level block
      block_read(fs_device, level_one[i], &level_two);

      for (j = 0; j < free_blocks; j++)
      {
        // free sectors
        free_map_release(level_two[j], 1);
        sector_num--;
      }

      // free second level block
      free_map_release(level_one[i], 1);
    }

    // free first level block itself
    free_map_release(inode->blocks[INODE_PTRS - 1], 1);
  }
}

bool
inode_is_dir (const struct inode *inode)
{
  return inode->is_dir;
}

int
inode_get_open_cnt (const struct inode *inode)
{
  return inode->open_cnt;
}

block_sector_t
inode_get_parent (const struct inode *inode)
{
  return inode->parent;
}

bool
inode_set_parent (block_sector_t parent, block_sector_t child)
{
  struct inode* inode = inode_open(child);

  if (!inode)
    return false;

  inode->parent = parent;
  inode_close(inode);
  return true;
}

void inode_lock (const struct inode *inode)
{
  lock_acquire(&((struct inode *)inode)->lock);
}

void inode_unlock (const struct inode *inode)
{
  lock_release(&((struct inode *) inode)->lock);
}