#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "filesys/cache.h"

//#define debug_printf(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define debug_printf(fmt, ...) // Define as empty if debugging is disabled

/** A directory. */
struct dir 
{
    struct inode *inode;                /**< Backing store. */
    off_t pos;                          /**< Current position. */
};

/** A single directory entry. */
struct dir_entry 
{
    block_sector_t inode_sector;        /**< Sector number of header. */
    char name[NAME_MAX + 1];            /**< Null terminated file name. */
    bool in_use;                        /**< In use or free? */
};

/** Creates a directory with space for ENTRY_CNT entries in the
    given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  bool created;
  // Create an inode for the directory
    created = inode_create(sector, entry_cnt * sizeof(struct dir_entry), 1 /* directory = true */);
    if (!created) {
        return false;  // If inode creation fails, return false
    }

    // Open the directory by its inode
    struct dir *dir = dir_open(inode_open(sector));
    if (dir == NULL) {
        return false;  // If opening the directory fails, return false
    }

    // Create "." entry for the directory itself
    struct dir_entry dir_item = {
        .inode_sector = sector,
        .in_use = true
    };
    strlcpy(dir_item.name, ".", sizeof(dir_item.name));
    off_t write_size = inode_write_at(dir->inode, &dir_item, sizeof(dir_item), 0);

    // Create ".." entry for the parent directory
    struct dir_entry parent_item = {
        .inode_sector = inode_get_inumber(dir->inode),
        .in_use = true
    };
    strlcpy(parent_item.name, "..", sizeof(parent_item.name));
    write_size += inode_write_at(dir->inode, &parent_item, sizeof(parent_item), sizeof(dir_item));

    // Close the directory
    dir_close(dir);

    buffer_cache_close();

    return true;  // Return true if directory creation and entries creation were successful
}

/** Opens and returns the directory for the given INODE, of which
    it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
    struct dir *dir = calloc(1, sizeof *dir);
    if (inode != NULL && dir != NULL)
    {
        dir->inode = inode;
        dir->pos = 0;
        return dir;
    }
    else
    {
        inode_close(inode);
        free(dir);
        return NULL;
    }
}

/** Opens the root directory and returns a directory for it.
    Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
    return dir_open(inode_open(ROOT_DIR_SECTOR));
}

/** Opens and returns a new directory for the same inode as DIR.
    Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
    return dir_open(inode_reopen(dir->inode));
}

/** Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
    if (dir != NULL)
    {
        inode_close(dir->inode);
        free(dir);
    }
}

/** Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
    return dir->inode;
}

/** Searches DIR for a file with the given NAME.
    If successful, returns true, sets *EP to the directory entry
    if EP is non-null, and sets *OFSP to the byte offset of the
    directory entry if OFSP is non-null.
    otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
    struct dir_entry e;
    size_t ofs;

    ASSERT (dir != NULL);
    ASSERT (name != NULL);

    for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e;
         ofs += sizeof e) {
        if (e.in_use && !strcmp(name, e.name)) {
            if (ep != NULL) {
                *ep = e;
            }
            if (ofsp != NULL) {
                *ofsp = ofs;
            }
            return true;
        }
    }
    return false;
}

/** Searches DIR for a file with the given NAME
    and returns true if one exists, false otherwise.
    On success, sets *INODE to an inode for the file, otherwise to
    a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
    struct dir_entry e;

    ASSERT (dir != NULL);
    ASSERT (name != NULL);

    if (lookup(dir, name, &e, NULL)){
        *inode = inode_open(e.inode_sector);
    } else {
        *inode = NULL;
    }

    return *inode != NULL;
}

/** Adds a file or directory named NAME to DIR, which must not already contain a
    file or directory by that name. The file's inode is in sector
    INODE_SECTOR. Returns true if successful, false on failure.
    Fails if NAME is invalid (i.e. too long) or a disk or memory
    error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector, int is_dir)
{
    struct dir_entry e;
    off_t ofs;
    bool success = false;

    ASSERT (dir != NULL);
    ASSERT (name != NULL);

    /* Check NAME for validity. */
    if (*name == '\0' || strlen(name) > NAME_MAX) {
        debug_printf("(dir_add) Invalid name: %s\n", name);
        return false;
    }

    /* Check that NAME is not in use. */
    if (lookup(dir, name, NULL, NULL)) {
        debug_printf("(dir_add) Name already in use: %s\n", name);
        goto done;
    }

    /* Set OFS to offset of free slot.
       If there are no free slots, then it will be set to the
       current end-of-file. */
    for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e;
         ofs += sizeof e) 
    {
        if (!e.in_use)
            break;
    }

    /* Write slot. */
    e.in_use = true;
    strlcpy(e.name, name, sizeof e.name);
    e.inode_sector = inode_sector;
    if (inode_write_at(dir->inode, &e, sizeof e, ofs) != sizeof e) {
        debug_printf("(dir_add) Failed to write directory entry\n");
        goto done;
    }

    if (is_dir) {
        struct dir *sub_dir = dir_open(inode_open(inode_sector));
        if (sub_dir == NULL) {
            debug_printf("(dir_add) Failed to open sub directory\n");
            goto done;
        }

        /* Add "." entry */
        struct dir_entry dot;
        dot.in_use = true;
        strlcpy(dot.name, ".", sizeof dot.name);
        dot.inode_sector = inode_sector;
        if (inode_write_at(sub_dir->inode, &dot, sizeof dot, 0) != sizeof dot) {
            debug_printf("(dir_add) Failed to write . entry in sub directory\n");
            dir_close(sub_dir);
            goto done;
        }

        /* Add ".." entry */
        struct dir_entry dot_dot;
        dot_dot.in_use = true;
        strlcpy(dot_dot.name, "..", sizeof dot_dot.name);
        dot_dot.inode_sector = inode_get_inumber(dir->inode);
        if (inode_write_at(sub_dir->inode, &dot_dot, sizeof dot_dot, sizeof dot) != sizeof dot_dot) {
            debug_printf("(dir_add) Failed to write .. entry in sub directory\n");
            dir_close(sub_dir);
            goto done;
        }

        dir_close(sub_dir);
    }

    success = true;

done:
    if (!success) {
        debug_printf("(dir_add) Failed to add entry: %s\n", name);
    }
    return success;
}


/* SUpport function for dir_remove()*/
bool
dir_is_empty (struct dir *dir) 
{
  struct dir_entry e;
  off_t ofs;

  ASSERT (dir != NULL);

  for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) {
    if (e.in_use && strcmp(e.name, ".") != 0 && strcmp(e.name, "..") != 0) {
      printf("Directory not empty: %s\n", e.name);
      return false;
    }
  }
  return true;
}
/** Removes any entry for NAME in DIR.
    Returns true if successful, false on failure,
    which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;  // Directory entry to store the found entry
  struct inode *inode = NULL;  // Inode corresponding to the directory entry
  bool success = false;  
  off_t ofs; 

  ASSERT (dir != NULL);  // Ensure the directory is not NULL
  ASSERT (name != NULL);  // Ensure the name is not NULL

  // Lookup the directory entry by name and get its offset
  if (!lookup(dir, name, &e, &ofs)){
    return false;
  }

  // Open the inode corresponding to the directory entry
  inode = inode_open(e.inode_sector);
  if (inode == NULL)
    return false;

  // Ensure the directory is empty before removal
  if (inode_is_dir(inode)) {
    struct dir *sub_dir = dir_open(inode);  // Open the directory to check its contents
    if (!dir_is_empty(sub_dir)) {
      dir_close(sub_dir);
      inode_close(inode);
      return false;  // Return false if the directory is not empty
    }
    dir_close(sub_dir);
  }

  // Mark the directory entry as not in use
  e.in_use = false;
  if (inode_write_at(dir->inode, &e, sizeof e, ofs) != sizeof e) {
    inode_close(inode);
    return false;
  }

  // Mark the inode as removed
  inode_remove(inode);
  success = true;

  // Close the inode
  inode_close(inode);

  if (success) {
    buffer_cache_close(); // Ensure buffer cache is flushed to disk //change
  }

  return success;
}




/** Reads the next directory entry in DIR and stores the name in
    NAME.  Returns true if successful, false if the directory
    contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
    struct dir_entry e;

  while (inode_read_at(dir->inode, &e, sizeof e, dir->pos) == sizeof e) {
    dir->pos += sizeof e;
    if (e.in_use && strcmp(e.name, ".") != 0 && strcmp(e.name, "..") != 0) {
      strlcpy(name, e.name, NAME_MAX + 1);
      return true;
    }
  }
  return false;
}