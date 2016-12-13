#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"

#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/directory.h"
#include "filesys/inode.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "threads/malloc.h"
#include "userprog/syscall.h"
#include "lib/user/syscall.h"
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "devices/input.h"

#define SYSCALL_NTH_ARG(f, n, type) (*((type *)((f)->esp + (n) * 4)))

#define USERASSERT( COND ) { if ( !(COND) ) syscall_exit(-1); } 

#define CHECK_VALID_FD(fd) USERASSERT(file_of_fd(fd))

#define CHECK_VALID_WRITE_FD(fd) USERASSERT(file_of_fd(fd) || fd == STDOUT_FILENO)

#define CHECK_VALID_READ_FD(fd) USERASSERT(file_of_fd(fd) || fd == STDIN_FILENO)

#define USER_BASE_ADDR ((void *)0x08048000)

// note that vaddr must not be func(args)
#define CHECK_VALID_USERADDR(vaddr) USERASSERT((is_user_vaddr(vaddr) && USER_BASE_ADDR < (vaddr)) && pagedir_get_page(thread_current()->pagedir, vaddr) != NULL )



static void syscall_handler (struct intr_frame *);

// lock used by allocate_fd()
static struct lock fd_lock;

  struct file*
file_of_fd(int fd)
{
  struct thread* cur = thread_current();
  struct list_elem* pos;
  struct fd_elem* pos_fd;

  if(list_empty(&cur->fd_list))
    return NULL;

  for(pos = list_begin (&cur->fd_list) ; 
      pos != list_end (&cur->fd_list) ; pos = pos->next){
    pos_fd = list_entry(pos, struct fd_elem, elem);
    if(pos_fd->fd == fd)
      return pos_fd->this_file;
  }
  return NULL;
}

// Check current thread have fd in it's fd list.

  bool
close_with_fd(int fd)
{
  struct thread* cur = thread_current();
  struct list_elem* pos;
  struct fd_elem* pos_fd;

  if(list_empty(&cur->fd_list)) return false;

  for(pos = list_begin (&cur->fd_list) ; 
      pos != list_end (&cur->fd_list) ; pos = pos->next){
    pos_fd = list_entry(pos, struct fd_elem, elem);
    if(pos_fd->fd == fd){
      file_close(pos_fd->this_file); // it has file_allow_write in it's content
      list_remove(pos);
      free(pos_fd);
      return true;
    }
  }
  return false;
}

  void
close_all_fd(struct list* fd_list)
{
  struct list_elem* pos, *next;
  struct fd_elem* pos_fd;

  if(list_empty(fd_list)) return;

  for(pos = list_begin (fd_list) ; 
      pos != list_end (fd_list) ; pos = next){
    pos_fd = list_entry(pos, struct fd_elem, elem);
    next = pos->next;

    file_close(pos_fd->this_file); // it has file_allow_write in it's content
    list_remove(pos);
    free(pos_fd);
  }
  list_init(fd_list);
}

  static int
allocate_fd(void)
{ 
  static int next_fd = 2;
  int fd;
  lock_acquire(&fd_lock);
  fd = next_fd++;
  lock_release(&fd_lock);

  return fd;
}

  void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&fd_lock);
}

  static void
syscall_handler (struct intr_frame *f) 
{

  // have to check f is valid (128MB)
  CHECK_VALID_USERADDR(f->esp);

  //	sysno = *(int*)(f->esp);
  //	ASSERT(SYS_HALT <= sysno && sysno <= SYS_INUMBER);


  //	if(sysno == 9)
  //		printf("%s : [[[[[[[%d]]]]]]], f->esp = %p [fd %d] [void* %p] [size %d]\n", thread_current()->name, sysno, f->esp, SYSCALL_NTH_ARG(f, 5, int), SYSCALL_NTH_ARG(f, 6, void*), SYSCALL_NTH_ARG(f, 7, int));
  switch(SYSCALL_NTH_ARG(f, 0, int)){
    case SYS_HALT:
      syscall_halt();
      break;		// NOT_REACHED
    case SYS_EXIT:
      USERASSERT(is_user_vaddr(f->esp+4));
      syscall_exit(SYSCALL_NTH_ARG(f, 1, int));
      break;
    case SYS_EXEC:                   /* Start another process. */
      USERASSERT(is_user_vaddr(f->esp + 4));
      f->eax = syscall_exec(SYSCALL_NTH_ARG(f, 1, char*));
      break;
    case SYS_WAIT:                   /* Wait for a child process to die. */
      USERASSERT(is_user_vaddr(f->esp + 4));
      f->eax = syscall_wait(SYSCALL_NTH_ARG(f, 1, pid_t));
      break;
    case SYS_CREATE:                 /* Create a file. */
      USERASSERT(is_user_vaddr(f->esp + 8));
      f->eax = syscall_create(SYSCALL_NTH_ARG(f, 1, char*), SYSCALL_NTH_ARG(f, 2, unsigned));
      break;
    case SYS_REMOVE:                 /* Delete a file. */
      USERASSERT(is_user_vaddr(f->esp + 4));
      f->eax = syscall_remove(SYSCALL_NTH_ARG(f, 1, char*));
      break;
    case SYS_OPEN:                   /* Open a file. */
      USERASSERT(is_user_vaddr(f->esp + 4));
      f->eax = syscall_open(SYSCALL_NTH_ARG(f, 1, char*));
      break;
    case SYS_FILESIZE:               /* Obtain a file's size. */
      USERASSERT(is_user_vaddr(f->esp + 4));
      f->eax = syscall_filesize(SYSCALL_NTH_ARG(f, 1, int));
      break;
    case SYS_READ:                   /* Read from a file. */		
      USERASSERT(is_user_vaddr(f->esp + 12));
      CHECK_VALID_USERADDR(SYSCALL_NTH_ARG(f, 2, void*));
      f->eax = syscall_read(SYSCALL_NTH_ARG(f, 1, int), SYSCALL_NTH_ARG(f, 2, void*), SYSCALL_NTH_ARG(f, 3, unsigned));
      break;
    case SYS_WRITE:                  /* Write to a file. */
      // check buffer is in valid space	
      USERASSERT(is_user_vaddr(f->esp + 12));
      CHECK_VALID_USERADDR(SYSCALL_NTH_ARG(f, 2, void*));
      f->eax = syscall_write(SYSCALL_NTH_ARG(f, 1, int), SYSCALL_NTH_ARG(f, 2, void*), SYSCALL_NTH_ARG(f, 3, unsigned));
      break;
    case SYS_SEEK:                   /* Change position in a file. */
      USERASSERT(is_user_vaddr(f->esp + 8));
      syscall_seek(SYSCALL_NTH_ARG(f, 1, int), SYSCALL_NTH_ARG(f, 2, unsigned));
      break;
    case SYS_TELL:                  /* Report current position in a file. */
      USERASSERT(is_user_vaddr(f->esp + 4));
      f->eax = syscall_tell(SYSCALL_NTH_ARG(f, 1, int));
      break;
    case SYS_CLOSE:                  /* Close a file. */
      USERASSERT(is_user_vaddr(f->esp + 4));
      syscall_close(SYSCALL_NTH_ARG(f, 1, int));
      break;
      // NO VM
    case SYS_MMAP:
    case SYS_MUNMAP:
      break;
    case SYS_CHDIR:
      USERASSERT(is_user_vaddr(f->esp + 4));
      CHECK_VALID_USERADDR(SYSCALL_NTH_ARG(f, 1, void*));
      f->eax = syscall_chdir(SYSCALL_NTH_ARG(f, 1, char*));
      break;
    case SYS_MKDIR:
      USERASSERT(is_user_vaddr(f->esp + 4));
      CHECK_VALID_USERADDR(SYSCALL_NTH_ARG(f, 1, void*));
      f->eax = syscall_mkdir(SYSCALL_NTH_ARG(f, 1, char*));
      break;
    case SYS_READDIR:
      USERASSERT(is_user_vaddr(f->esp + 8));
      CHECK_VALID_USERADDR(SYSCALL_NTH_ARG(f, 2, void*));
      f->eax = syscall_readdir(SYSCALL_NTH_ARG(f, 1, int), SYSCALL_NTH_ARG(f, 2, char*));
      break;
    case SYS_ISDIR: 
      USERASSERT(is_user_vaddr(f->esp + 4));
      f->eax = syscall_isdir(SYSCALL_NTH_ARG(f, 1, int));
      break;
    case SYS_INUMBER: 
      USERASSERT(is_user_vaddr(f->esp + 4));
      f->eax = syscall_inumber(SYSCALL_NTH_ARG(f, 1, int));
      break;
    default:
      printf ("Unknown System-Call");
      break;
  }
}

  void 
syscall_halt (void)
{
  shutdown_power_off();
  NOT_REACHED();
}

  void 
syscall_exit (int status)
{
  struct thread* cur = thread_current();	
  struct finished_elem* f;
  cur->exit = status;
  if(cur->parent){
    list_remove(&cur->child_elem);
    f = (struct finished_elem*) malloc(sizeof(struct finished_elem)); // free at paren die
    f->tid = cur->tid;
    f->status = cur->exit;
    list_push_back(&cur->parent->finished_list, &f->elem);
  }
  printf ("%s: exit(%d)\n", cur->name, cur->exit);
  thread_exit();
}

pid_t syscall_exec (const char *file)
{
  USERASSERT(file);
  return process_execute(file);
}

int syscall_wait (pid_t t)
{
  bool find_succ = false, find_succ2 = false;
  struct thread* cur = thread_current();
  struct list_elem* pos;
  struct thread* child;
  struct finished_elem* finished;
  // check t is in child list.
  if(cur->is_waiting || (list_empty(&cur->child_list) && list_empty(&cur->finished_list)))
    return -1;	

  if(!list_empty(&cur->child_list)){
    for(pos = list_begin(&cur->child_list);
	pos != list_end(&cur->child_list) ;
	pos = pos->next){
      child = list_entry(pos, struct thread, child_elem);
      if(t == child->tid){
	find_succ = true;
	break;
      }
    }
  }
  if(!find_succ && !list_empty(&cur->finished_list)){
    for(pos = list_begin(&cur->finished_list);
	pos != list_end(&cur->finished_list) ;
	pos = pos->next){
      finished = list_entry(pos, struct finished_elem, elem);
      if(t == finished->tid){
	find_succ2 = true;
	break;
      }
    }
  }
  if(!find_succ && !find_succ2)
    return -1;

  if(find_succ){
    cur->waiting_now = child->tid;
    cur->is_waiting = true;
    // sema_down and wait
    sema_down(&cur->wait_sema);

    cur->waiting_now = -1;
    cur->is_waiting = false;
    // reset
  }
  else{
    cur->wait_exit = finished->status;
    list_remove(&finished->elem);
    free(finished);
  }

  return cur->wait_exit;
}


bool syscall_create (const char *file, unsigned initial_size)
{
  // Now, we have to treat file == NULL case, since filesys_create call ASSERT(file != NULL), ASSERT(length >= 0).
  // User Process do not want to abort all system to create NULL file. Only one(caller) die.	
  USERASSERT(file);
  return filesys_create(file, initial_size, false);
}

  bool 
syscall_remove (const char *file)
{	
  USERASSERT(file);
  //TODO: remove 
  //    if given string is empty directory
  //       or it is file
  //
  //       do not remove if it's not empty directory (rmdir)
  return filesys_remove(file);
}

  int 
syscall_open (const char *file)
{
  struct file* f;
  struct fd_elem* fdelem;
  USERASSERT(file);

  if(!(f = filesys_open(file))) 
    return -1;


  // TODO: It must be free when fd is close or something.
  // Only fd owner could act with fd.
  fdelem =  malloc(sizeof (struct fd_elem));
  fdelem->fd = allocate_fd();
  fdelem->this_file = f;
  list_push_back(&thread_current()->fd_list, &fdelem->elem);

  return fdelem->fd;
}

  int
syscall_filesize (int fd)
{
  // TODO: check fd is valid
  // stdin, stdout, stderr
  CHECK_VALID_FD(fd);
  return file_length(file_of_fd(fd));
}


  int 
syscall_read (int fd, void *buffer, unsigned length)
{
  int t;
  uint8_t temp;
  int i = 0;
  CHECK_VALID_READ_FD(fd);
  if(fd == STDIN_FILENO){
    while(length--){
      temp = input_getc();
      ((uint8_t*)buffer)[i++] = temp;
    }
    return i;
  }
  t = file_read(file_of_fd(fd), buffer, length);
  return t;
}

  int 
syscall_write (int fd, const void *buffer, unsigned length)
{
  int t;
  CHECK_VALID_WRITE_FD(fd);
  if(fd == STDOUT_FILENO){
    putbuf(buffer, length);
    return length;
  }
  t = file_write(file_of_fd(fd), buffer, length);
//printf ("write_t: %d\n",t);
  return t;
}

  void 
syscall_seek (int fd, unsigned position)
{
  CHECK_VALID_FD(fd);
  file_seek(file_of_fd(fd), position);
}

  unsigned 
syscall_tell (int fd)
{
  unsigned t;
  CHECK_VALID_FD(fd);
  t = file_tell(file_of_fd(fd));
  return t;
}


  void 
syscall_close (int fd)
{
  CHECK_VALID_FD(fd);
  USERASSERT(close_with_fd(fd));
}



bool 
syscall_chdir(const char* dir)
{
  USERASSERT(dir);
  struct dir* this_dir = filesys_chdir(dir);
  if(!this_dir)
    return false;
  else{
    if(thread_current()->current_dir)
      dir_close(thread_current()->current_dir);
    thread_current()->current_dir = this_dir;
  }
  return true; 
}

bool
syscall_mkdir(const char* dir)
{
  USERASSERT(dir);
  return filesys_create(dir, 0, true);
}

bool
syscall_readdir(int fd, char* name)
{
  CHECK_VALID_FD(fd);
  struct dir* dir = (struct dir*)file_of_fd(fd);
  if(dir)
    return dir_readdir(dir, name);
  else
    return false;
}

bool 
syscall_isdir(int fd)
{
  CHECK_VALID_FD(fd);
  return inode_is_dir(file_get_inode(file_of_fd(fd)));
}

int 
syscall_inumber(int fd)
{
  CHECK_VALID_FD(fd);
  return inode_get_inumber(file_get_inode(file_of_fd(fd))); 
}
