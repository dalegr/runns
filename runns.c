#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>

#include "runns.h"
#include <fcntl.h>

#define __USE_GNU
#include <sched.h>

int sockfd;
pid_t *childs_pid = 0;
size_t childs_run = 0;

int
drop_priv(const char *username, struct passwd **pw);

void
stop_daemon(int stopbit);

int
main(int argc, char **argv)
{
  char *username;
  char *program;
  char *netns;
  struct passwd *pw = NULL;
  struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = defsock};
  char **envs;
  struct runns_header hdr;

  // Set safe permissions and create directory.
  umask(0022);
  if (mkdir(RUNNS_DIR, 0755) < 0)
    ERR(0, "runns.c", "Can't create directory %s", RUNNS_DIR);

  // Up daemon socket.
  sockfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sockfd == -1)
    ERR(sockfd, "runns.c", "Something gone very wrong, socket = %d", sockfd);
  if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    ERR(sockfd, "runns.c", "Can't bind socket to %s", addr.sun_path);

  struct group *group;
  group = getgrnam("users");
  chown(addr.sun_path, 0, group->gr_gid);
  chmod(addr.sun_path, 0775);

  daemon(0, 0);
  while (1)
  {
    if (listen(sockfd, 16) == -1)
      ERR(sockfd, "runns.c", "Can't start listen socket %d (%s)", sockfd, addr.sun_path);

    int data_sockfd = accept(sockfd, 0, 0);
    if (data_sockfd == -1)
      ERR(sockfd, "runns.c", "Can't accept connection");

    int ret = read(data_sockfd, (void *)&hdr, sizeof(hdr));
    if (ret == -1)
      WARN("Can't read data");

    // Stop daemon on demand.
    if (hdr.stopbit)
    {
      close(data_sockfd);
      stop_daemon(hdr.stopbit);
    }

    // Read username, program name and network namespace name
    username = (char *)malloc(hdr.user_sz);
    program = (char *)malloc(hdr.prog_sz);
    netns = (char *)malloc(hdr.netns_sz);
    ret = read(data_sockfd, (void *)username, hdr.user_sz);
    ret = read(data_sockfd, (void *)program, hdr.prog_sz);
    ret = read(data_sockfd, (void *)netns, hdr.netns_sz);

    // Read environment variables
    envs = (char **)malloc(++hdr.env_sz*sizeof(char *));
    for (int i = 0; i < hdr.env_sz - 1; i++)
    {
      size_t env_sz;
      ret = read(data_sockfd, (void *)&env_sz, sizeof(size_t));
      envs[i] = (char *)malloc(env_sz);
      ret = read(data_sockfd, (void *)envs[i], env_sz);
      puts(envs[i]);
    }
    envs[hdr.env_sz - 1] = 0;

    close(data_sockfd);

    // Make fork
    pid_t child = fork();
    if (child == -1)
      ERR(sockfd, "runns.c", "Fail on fork");

    // Child
    if (child == 0)
    {
      int netfd = open(netns, 0);
      setns(netfd, CLONE_NEWNET);
      drop_priv(username, &pw);
      if (execve(program, 0, (char * const *)envs) == -1)
        perror(0);
    }

    // Save child.
    childs_pid = realloc(childs_pid, sizeof(pid_t)*(childs_run + 1));
    childs_pid[childs_run++] = child;
  }

  return 0;
}

int
drop_priv(const char *username, struct passwd **pw)
{
  *pw = getpwnam(username);
  if (*pw) {
    uid_t uid = (*pw)->pw_uid;
    gid_t gid = (*pw)->pw_gid;

    if (initgroups((*pw)->pw_name, gid) != 0)
      ERR(sockfd, "runns.c", "Couldn't initialize the supplementary group list");
    endpwent();

    if (setgid(gid) != 0 || setuid(uid) != 0) {
      ERR(sockfd, "runns.c", "Couldn't change to '%.32s' uid=%lu gid=%lu",
             username,
             (unsigned long)uid,
             (unsigned long)gid);
    }
    else
      fprintf(stderr, "dropped privs to %s\n", username);
  }
  else
    ERR(sockfd, "runns.c", "Couldn't find user '%.32s'", username);
}

void
stop_daemon(int stopbit)
{
  switch (stopbit)
  {
    case RUNNS_STOP:
      for (pid_t i = 0; i < childs_run; i++)
      {
        int wstatus;
        int pid = childs_pid[i];
        if (waitpid(pid, &wstatus, 0) < 0)
          WARN("Can't wait for child with PID %u", pid);
        else if (!WIFEXITED(wstatus))
          WARN("Child terminated with error, exit code: %u", WEXITSTATUS(wstatus));
      }
      free(childs_pid);
    case RUNNS_FORCE_STOP:
      close(sockfd);
      unlink(defsock);
      rmdir(RUNNS_DIR);
      exit(stopbit & RUNNS_NORMALIZE);
      break;
  }
}
