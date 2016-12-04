#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "lib/user/syscall.h"
#include "filesys/file.h"
#include "lib/kernel/list.h"

void syscall_init (void);

struct fd_elem
{
	int fd;
	struct file* this_file;
	struct list_elem elem;
};

struct finished_elem
{
	int tid;
	int status;
	struct list_elem elem;
};

struct lock filesys_lock; // lock for filesys system call

struct file* file_of_fd(int fd);

bool close_with_fd(int fd);

void close_all_fd(struct list* fd_list);



// To discriminate this with ../lib/user/syscall.c, add syscall_ prefix to system call.

void syscall_halt (void) NO_RETURN;
void syscall_exit (int status) NO_RETURN;
pid_t syscall_exec (const char *file);
int syscall_wait (pid_t);
bool syscall_create (const char *file, unsigned initial_size);
bool syscall_remove (const char *file);
int syscall_open (const char *file);
int syscall_filesize (int fd);
int syscall_read (int fd, void *buffer, unsigned length);
int syscall_write (int fd, const void *buffer, unsigned length);
void syscall_seek (int fd, unsigned position);
unsigned syscall_tell (int fd);
void syscall_close (int fd);
bool syscall_chdir(const char* dir);
bool syscall_mkdir(const char* dir);
bool syscall_readdir(int fd, char* name);
bool syscall_isdir(int fd);
int syscall_inumber(int fd);




#endif /* userprog/syscall.h */
