#include "threads/thread.h"
#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
#ifdef FILESYS
  cache_flush();
#endif
}

struct dir* dir_of_name(char* s)
{
  char* save_ptr, *token;
  struct dir* result_dir;
  struct dir* assoc_dir = thread_current()->current_dir;

  bool success = false;

  if(!s && !assoc_dir)
    return dir_open_root();
  else if(!s)
    return dir_reopen(assoc_dir);
  
  if(assoc_dir && strlen(s) && s[0] != '/')
    result_dir = dir_reopen(assoc_dir);
  else
    result_dir = dir_open_root();

  if(!result_dir) 
    goto done;

  token = strtok_r(s, "/", &save_ptr);
  while(token){
    if(strlen(token) != 0){
      struct inode* inode = NULL;
      if (strcmp(token, ".") == 0){
        // do nothing
      }
      else if(strcmp(token, "..") == 0){
        if(directory_get_inumber(result_dir) != (ROOT_DIR_SECTOR))
        {
          block_sector_t parent = directory_get_parent(result_dir);
          dir_close(result_dir);
          if(!(result_dir = dir_open(inode = inode_open(parent))))
            inode_close(inode);
        }
      }
      // TODO: If token is . or .. ,
      // Add . and .. in inode(. : self, .. : parent)
      // or, string compare in this part  
      else if (!dir_lookup (result_dir, token, &inode)
          || !inode
          || !inode_is_dir(inode)){
        dir_close(result_dir);
        inode_close(inode);
        goto done;
      }
      else {// TODO: check inode is dir
        dir_close(result_dir);
        result_dir = dir_open(inode);
      }
    }
    if(result_dir == NULL) 
      goto done;
    // case double slash
    token = strtok_r(NULL, "/", &save_ptr);
  }
  success = true;
done:
  return success ? result_dir : NULL;
}

void div_part(char* name, char** dir, char** filename)
{
  int i = strlen(name);
  while(i--){
    if(name[i] == '/'){
      name[i] = 0;
      *filename = name + i + 1;
      *dir = name;
      return;
    }
  }
  *filename = name;
  *dir = NULL;
}

static int get_filename_length(char* name)
{
  int i = strlen(name);
  int res = 0;
  if(name[i-1] == '/')
    i--;

  while(i--){
    if(name[i] == '/')
      break;
    res++;
  }
  return res;
}




/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size, bool is_dir) 
{
  if(!strlen(name)) return false;

  block_sector_t inode_sector = 0;
  if(get_filename_length(name) > NAME_MAX)
    return false;
  char* temp = (char*)malloc(sizeof(strlen(name) + 1));
  strlcpy(temp, name, strlen(name) + 1);

  char* direct, *filename;
  div_part(temp, &direct, &filename);

  struct dir *dir = dir_of_name (direct);
 
  bool success;

  if(is_dir)
    success = (dir != NULL
                && free_map_allocate(1, &inode_sector)
                && dir_create(inode_sector, 4, directory_get_inumber(dir))
                && dir_add(dir, filename, inode_sector));

  else
    success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, -1)
                  && dir_add (dir, filename, inode_sector));


  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (dir);

  free(temp);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */


struct dir *
filesys_chdir (const char *name)
{
  if(strlen(name) == 0) return NULL;
  if(get_filename_length(name) > NAME_MAX)
    return false;
  char* temp = (char*)malloc(sizeof(strlen(name) + 1));
  strlcpy(temp, name, strlen(name) + 1);

  char* direct, *filename;
  div_part(temp, &direct, &filename);

  struct dir *dir = dir_of_name (direct);
  struct inode *inode = NULL;

  if (dir != NULL){
    if(!strcmp(filename, "..")){
      inode = inode_open(directory_get_parent(dir));
      dir_close(dir);
      dir = dir_open(inode);
      free(temp);
      return dir;
    }
    else if(!strcmp(filename, ".")){
      free(temp);
      return dir;
    }
    else if(strlen(filename)){
      dir_lookup (dir, filename, &inode);
      dir_close (dir);
      free(temp);
      return dir_open(inode);
    }
    else{
      free(temp);
      return dir;
    }
  }
  return NULL;
}

struct file *
filesys_open (const char *name)
{
  if(strlen(name) == 0) return NULL;
  if(get_filename_length(name) > NAME_MAX)
    return false;
  char* temp = (char*)malloc(sizeof(strlen(name) + 1));
  strlcpy(temp, name, strlen(name) + 1);

  char* direct, *filename;
  div_part(temp, &direct, &filename);

  struct dir *dir = dir_of_name (direct);
  struct inode *inode = NULL;
  if (dir != NULL){
    if(!strcmp(filename, "..")){
      inode = inode_open(directory_get_parent(dir));
      dir_close(dir);
    }
    else if(!strcmp(filename, "."))
      inode = dir_get_inode(dir);
    else if(strlen(filename)){
      dir_lookup (dir, filename, &inode);
      dir_close (dir);
    }
    else
      inode = dir_get_inode(dir);
  }

  free(temp);
  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  if(strlen(name) == 0) return false;
  if(get_filename_length(name) > NAME_MAX)
    return false;

  bool success = false;
  char* temp = (char*)malloc(sizeof(strlen(name) + 1));
  strlcpy(temp, name, strlen(name) + 1);

  char* direct, *filename;
  div_part(temp, &direct, &filename);

  while(strlen(filename) == 0){
    char* temp2;
    div_part(direct, &temp2, filename);
    direct = temp2;
    if(!direct){
      if(!strlen(filename)) 
        goto done;
    }
    else {
      if(!strlen(direct) && !strlen(filename))
        goto done;
    }
  }
  if(!strcmp(filename, "..") 
      || !strcmp(filename, "."))
    goto done;

  struct dir *dir = dir_of_name (direct);

  success = dir != NULL && dir_remove (dir, filename);
  dir_close (dir); 

done:
  free(temp);
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
