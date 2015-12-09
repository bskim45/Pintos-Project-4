#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/init.h"
#include "userprog/process.h"
#include <kernel/console.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "threads/vaddr.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

#define under_phys_base(addr) if((void*)addr >= PHYS_BASE) sys_exit(-1);
#define esp_under_phys_base(f, args_num) under_phys_base(((int*)(f->esp)+args_num+1))
#define check_fd(fd, fail, f) if(fd < 0 || fd >= FD_MAX) {f->eax = fail; break;}
static void syscall_handler (struct intr_frame *f);
static void sys_halt (void);
static tid_t sys_exec(void *cmd_line, struct intr_frame *f);
static int sys_write (int fd, void *buffer_, unsigned size, struct intr_frame *f);
static int sys_read (int fd, void *buffer_, unsigned size, struct intr_frame *f);
static int sys_wait (tid_t pid, struct intr_frame *f);
static bool sys_create (void *file_, unsigned initial_size, struct intr_frame *f);
static bool sys_remove (void *file_, struct intr_frame *f);
static int sys_open (void *file_, struct intr_frame *f);
static int sys_filesize (int fd, struct intr_frame *f);
static void sys_seek (int fd, unsigned position, struct intr_frame *f);
static unsigned sys_tell (int fd, struct intr_frame *f);
static void sys_close (int fd, struct intr_frame *f);

bool sys_chdir(char *path, struct intr_frame *f);
bool sys_mkdir(char *path, struct intr_frame *f);
bool sys_readdir(int fd, char *path, struct intr_frame *f);
bool sys_isdir(int fd, struct intr_frame *f);
int sys_inumber(int fd, struct intr_frame *f);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  int sys_vector;
  sys_vector=*(int *)(f->esp);
  switch(sys_vector)
  {
  case SYS_HALT:
    esp_under_phys_base(f, 0);
    sys_halt ();
    break;
  case SYS_EXIT:
    if ((void *)((int *)f->esp + 1) >= PHYS_BASE)
      sys_exit (-1);
    else
      sys_exit (*((int *)f->esp + 1));
    break;
  case SYS_EXEC:
    esp_under_phys_base(f, 1);
    under_phys_base (*((int **)f->esp + 1));
    sys_exec (*((int **)f->esp + 1), f);
    break;
  case SYS_WAIT:
    esp_under_phys_base(f, 1);
    sys_wait (*((int *)f->esp + 1), f);
    break;
  case SYS_CREATE:
    if ((void*)*((int **)f->esp + 1) == NULL)
      sys_exit(-1);
    esp_under_phys_base(f, 2);
    under_phys_base (*((int **)f->esp + 1));
    sys_create (*((int **)f->esp + 1), *((int *)f->esp + 2), f);
    break;
  case SYS_REMOVE:
    esp_under_phys_base(f, 1);
    under_phys_base (*((int **)f->esp + 1));
    sys_remove (*((int **)f->esp + 1), f);
    break;
  case SYS_OPEN:
    if ((void*)*((int **)f->esp + 1) == NULL)
      sys_exit(-1);
    esp_under_phys_base(f, 1);
    under_phys_base (*((int **)f->esp + 1));
    sys_open (*((int **)f->esp + 1), f);
    break;
  case SYS_FILESIZE:
    esp_under_phys_base(f, 1);
    check_fd(*((int *)f->esp + 1), -1, f);
    sys_filesize (*((int *)f->esp + 1), f);
    break;
  case SYS_READ:
    esp_under_phys_base(f, 3);
    under_phys_base (*((int **)f->esp + 2));
    check_fd(*((int *)f->esp + 1), -1, f)
    sys_read (*((int *)f->esp + 1), *((int **)f->esp + 2), *((int *)f->esp + 3), f);
    break;
  case SYS_WRITE:
    esp_under_phys_base(f, 3);
    under_phys_base (*((int **)f->esp + 2));
    check_fd(*((int *)f->esp + 1), -1, f)
    sys_write (*((int *)f->esp + 1), *((int **)f->esp + 2), *((int *)f->esp + 3), f);
    break;
  case SYS_SEEK:
    esp_under_phys_base(f, 2);
    check_fd(*((int *)f->esp + 1), 0, f)
    sys_seek (*((int *)f->esp + 1), *((int *)f->esp + 2), f);
    break;
  case SYS_TELL:
    esp_under_phys_base(f, 1);
    check_fd(*((int *)f->esp + 1), 0, f)
    sys_tell (*((int *)f->esp + 1), f);
    break;
  case SYS_CLOSE:
    esp_under_phys_base(f, 1);
    check_fd(*((int *)f->esp + 1), 0, f)
    sys_close (*((int *)f->esp + 1), f);
    break;

  case SYS_CHDIR:
    esp_under_phys_base(f, 1);
    sys_chdir((char*)*((int *)f->esp + 1), f);
    break;
  case SYS_MKDIR:
    esp_under_phys_base(f, 1);
    sys_mkdir((char*)*((int *)f->esp + 1), f);
    break;
  case SYS_READDIR:
    esp_under_phys_base(f, 2);
    check_fd(*((int *)f->esp + 1), false, f);
    sys_readdir(*((int *)f->esp + 1), (char*)*((int *)f->esp + 2), f);
    break;
  case SYS_ISDIR:
    esp_under_phys_base(f, 1);
    check_fd(*((int *)f->esp + 1), false, f);
    sys_isdir(*((int *)f->esp + 1), f);
    break;
  case SYS_INUMBER:
    esp_under_phys_base(f, 1);
    check_fd(*((int *)f->esp + 1), false, f);
    sys_inumber(*((int *)f->esp + 1), f);
  }
}

bool sys_chdir(char* path, struct intr_frame *f)
{
    bool success = filesys_chdir(path);
    f->eax = success;
    return success;
}

bool sys_mkdir(char* path, struct intr_frame *f)
{
    bool success = filesys_create(path, 0, true);
    f->eax = success;
    return success;
  }

bool sys_readdir(int fd, char* path, struct intr_frame *f)
{
    f->eax = false;
    ASSERT (fd >= 0 && fd < FD_MAX);
    
    struct file* file = thread_current()->fd_list[fd];
    if (file == NULL) return false;
    
    struct inode* inode = file_get_inode(file);
    if(inode == NULL) return false;
    if(!inode_is_dir(inode)) return false;
    
    // struct dir* dir = dir_open(inode);
    struct dir* dir = (struct dir*) file;
    // if(dir == NULL) return false;
    if(!dir_readdir(dir, path)) return false;
    
    f->eax = true;
    return true;
}

bool sys_isdir(int fd, struct intr_frame *f)
{
    f->eax = false;
    ASSERT (fd >= 0 && fd < FD_MAX);

    struct file* file = thread_current()->fd_list[fd];
    if (file == NULL) return false;

    struct inode* inode = file_get_inode(file);
    if(inode == NULL) return false;
    if(!inode_is_dir(inode)) return false;
    
    f->eax = true;
    return true;
}

int sys_inumber(int fd, struct intr_frame *f)
{
    f->eax = -1;
    ASSERT (fd >= 0 && fd < FD_MAX);

    struct file* file = thread_current()->fd_list[fd];
    if (file == NULL) return -1;

    struct inode* inode = file_get_inode(file);
    if(inode == NULL) return -1;

    block_sector_t inumber = inode_get_inumber(inode);
    f->eax = inumber;
    return inumber;
}

static void
sys_halt (void)
{
  shutdown_power_off();
}

void
sys_exit (int status)
{
  struct thread *t = thread_current();
  int fd;
  
  t->exit_code = status;
  t->end = true;

  for (fd=0; fd < t->fd_num; fd++){ // close all open fd
    if (t->fd_list[fd] != NULL)
    {
      struct inode* inode = file_get_inode(t->fd_list[fd]);

      if(inode == NULL)
        continue;

      if(inode_is_dir(inode))
        dir_close(t->fd_list[fd]);
      else
        file_close (t->fd_list[fd]);

      t->fd_list[fd] = 0;
    }
  }

  // close current working dir
  if (thread_current()->dir)
    dir_close(thread_current()->dir);

  printf("%s: exit(%d)\n", t->process_name, status);

  sema_up (&t->wait_this);
  //unblock parent
  sema_down (&t->kill_this);
  //kill_this will up when parent get exit_code succefully

  file_allow_write (t->open_file); 
  file_close (t->open_file); 
  thread_exit();
}

static tid_t
sys_exec(void *cmd_line, struct intr_frame *f)
{
  int tid;
  tid=process_execute((char*)cmd_line);
  f->eax = tid;
  return tid;
}

static int
sys_wait (tid_t pid, struct intr_frame *f)
{
  int exit_code = process_wait(pid);
  f->eax = exit_code;
  return exit_code;
} 

static bool
sys_create (void *file_, unsigned initial_size, struct intr_frame *f)
{
  bool success;
  success = filesys_create ((char*)file_, initial_size, false);
  f->eax = (uint32_t) success;
  return success;
}

static bool
sys_remove (void *file_, struct intr_frame *f)
{
  bool success;
  success = filesys_remove ((char*)file_);
  f->eax = (uint32_t) success;
  return success;
}

static int
sys_open (void *file_, struct intr_frame *f)
{
  struct file *file;
  file = filesys_open ((char*)file_);
  if (file == NULL){
    f->eax = -1;
    return -1;
  }
  else{
    struct thread *t = thread_current();
    if (t->fd_num >= FD_MAX){
      f->eax = -1;
      return -1;
    }
    f->eax = t->fd_num;
    (t->fd_list)[(t->fd_num)++] = file;
    return f->eax;
  }
}

static int
sys_filesize (int fd, struct intr_frame *f)
{
  int size;
  struct thread *t = thread_current();

  ASSERT (fd >= 0 && fd < FD_MAX);
  if (t->fd_list[fd] == NULL){
    f->eax = 0;
    return 0;
  }
  else{
    size = file_length (t->fd_list[fd]);
    f->eax = size;
    return size;
  }
}

static void
sys_seek (int fd, unsigned position, struct intr_frame *f UNUSED)
{
  struct thread *t = thread_current();
  
  ASSERT (fd >= 0 && fd < FD_MAX);
  if (t->fd_list[fd] != NULL)
    file_seek (t->fd_list[fd], position);
}

static unsigned
sys_tell (int fd, struct intr_frame *f)
{
  struct thread *t = thread_current();
  unsigned position;
  
  ASSERT (fd >= 0 && fd < FD_MAX);
  if (t->fd_list[fd] == NULL){
    f->eax = 0;
    return 0;
  }
  else{
    position = file_tell (t->fd_list[fd]);
    f->eax = position;
    return position;
  }
}

static void
sys_close (int fd, struct intr_frame *f UNUSED)
{
  struct thread *t = thread_current();
  
  ASSERT (fd >= 0 && fd < FD_MAX);
  if (t->fd_list[fd] != NULL)
  {
    struct inode* inode = file_get_inode(t->fd_list[fd]);

    if(inode == NULL)
      return;

    if(inode_is_dir(inode))
    {
      dir_close(t->fd_list[fd]);
    }
    else
    {
      file_close (t->fd_list[fd]);
    }
    t->fd_list[fd] = NULL;
  }
}

static int
sys_write (int fd, void *buffer_, unsigned size, struct intr_frame *f)
{
  char *buffer = (char*)buffer_;

  ASSERT (fd >= 0 && fd < FD_MAX);
//  printf("---------------------\n%d, %s, %d---------------------\n", fd, buffer, size);
  if (fd == 1){
    putbuf(buffer, size);
    f->eax = size;
  }
  else{
    struct thread *t = thread_current();
    if (t->fd_list[fd] == NULL){
      f->eax = -1;
    }
    else{
      f->eax = file_write (t->fd_list[fd], buffer_, size); 
    }
  }
  return f->eax;
}

static int
sys_read (int fd, void *buffer_, unsigned size, struct intr_frame *f)
{
  unsigned i;
  char *buffer = (char*)buffer_;

  ASSERT (fd >= 0 && fd < FD_MAX);
  if (fd == 0){
    for (i=0; i<size; i++)
      buffer[i] = input_getc();
  }
  else{
    struct thread *t = thread_current();
    if (t->fd_list[fd] == NULL)
      f->eax = -1;
    else
      f->eax = file_read (t->fd_list[fd], buffer, size);
  }
  return f->eax;
}
