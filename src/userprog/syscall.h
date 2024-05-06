#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include <debug.h>

void syscall_init (void);

// using code from lib/user/syscall.h


typedef int pid_t;
#define PID_ERROR ((pid_t) -1)

/** Map region identifier. */
typedef int mapid_t;
#define MAP_FAILED ((mapid_t) -1)

/** Maximum characters in a filename written by readdir(). */
#define READDIR_MAX_LEN 14

/** Typical return values from main() and arguments to exit(). */
#define EXIT_SUCCESS 0          /**< Successful execution. */
#define EXIT_FAILURE 1          /**< Unsuccessful execution. */

/** Projects 2 and later. */
void halt (void);
void exit (int status);
pid_t exec (const char *file);
int wait (pid_t);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned length);
int write (int fd, const void *buffer, unsigned length);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);


#endif /**< userprog/syscall.h */
