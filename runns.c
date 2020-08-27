/*
 * vim:et:sw=2:
 *
 * Copyright (c) 2019, 2020 Nikita (sh1r4s3) Ermakov <sh1r4s3@mail.si-head.nl>
 * SPDX-License-Identifier: MIT
 */

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <pwd.h>
#include <grp.h>
#include <syslog.h>

#include <fcntl.h>
#include "runns.h"

#include <sched.h>

// Emit log message
#define ERR(format, ...) \
    { \
      syslog(LOG_INFO | LOG_DAEMON, "runns.c:%d / errno=%d / " format "\n", __LINE__, errno, ##__VA_ARGS__); \
      stop_daemon(0); \
    }

#define WARN(format, ...) \
      syslog(LOG_INFO | LOG_DAEMON, "runns.c:%d / warning / " format "\n", __LINE__, ##__VA_ARGS__);

#define INFO(format, ...) \
      syslog(LOG_INFO | LOG_DAEMON, "runns.c:%d / info / " format "\n", __LINE__, ##__VA_ARGS__);

struct runns_header hdr = {0};
int sockfd = 0;
struct runns_child childs[MAX_CHILDS] = {0};
unsigned int childs_run = 0;
char *program = 0;
char *netns = 0;
char **args = 0;
char **envs = 0;
int *glob_pid = 0;

int
drop_priv(uid_t _uid, struct passwd **pw);

void
stop_daemon(int flag);

int
clean_pids();

void
free_tvars();

int
create_ptms();

int
main(int argc, char **argv)
{
  struct passwd *pw = NULL;
  struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = defsock};

  // Check the root
  if (getuid() != 0)
  {
    extern FILE *stderr;
    fputs("Please run as a root user\n", stderr);
    ERR("Only root can run the runns daemon");
  }

  glob_pid = mmap(NULL, sizeof(glob_pid), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (!glob_pid)
    ERR("Can't allocate memory");

  if (daemon(0, 0))
    ERR("Can't daemonize the process");

  // Set safe umask and create directory.
  umask(0022);
  if (mkdir(RUNNS_DIR, 0755) < 0)
    ERR("Can't create directory %s", RUNNS_DIR);

  // Up daemon socket.
  sockfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sockfd == -1)
    ERR("Something gone very wrong, socket = %d", sockfd);
  if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    ERR("Can't bind socket to %s", addr.sun_path);
  // Switch permissions and group.
  struct group *group;
  group = getgrnam("runns");
  if (!group)
    ERR("Can't get runns group\n");
  if (chown(addr.sun_path, 0, group->gr_gid) ||
      chmod(addr.sun_path, 0775))
  {
    ERR("Can't chown/chmod");
  }

  INFO("runns daemon has started");

  while (1)
  {
    if (listen(sockfd, 16) == -1)
      ERR("Can't start listen socket %d (%s)", sockfd, addr.sun_path);

    int data_sockfd = accept(sockfd, 0, 0);
    if (data_sockfd == -1)
      ERR("Can't accept connection");
    struct ucred cred;
    socklen_t cred_len = (socklen_t)sizeof(struct ucred);
    if (getsockopt(data_sockfd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == -1)
    {
      WARN("Can't get user credentials");
      close(data_sockfd);
      continue;
    }

    int ret = read(data_sockfd, (void *)&hdr, sizeof(hdr));
    if (ret == -1)
      WARN("Can't read data");

    // Stop daemon on demand.
    if (hdr.flag & RUNNS_STOP)
    {
      if (cred.uid == 0)
      {
        INFO("closing");
        close(data_sockfd);
        stop_daemon(hdr.flag);
      }
      else
      {
        WARN("Client with %d UID tried to kill the daemon ", cred.uid);
        continue;
      }
    }
    // Transfer list of childs
    if (hdr.flag & RUNNS_LIST)
    { // TODO rets
      INFO("uid=%d ask for pid list", cred.uid);
      clean_pids();
      // Count the number of jobs for uid
      unsigned int jobs = 0;
      for (int i = 0; i < childs_run; i++)
      {
        if (childs[i].uid == cred.uid)
          ++jobs;
      }

      if (write(data_sockfd, (void *)&jobs, sizeof(jobs)) == -1)
        ERR("Can't send number of jobs to the client %d", cred.uid);
      for (unsigned int i = 0; i < jobs; i++)
      {
        if (write(data_sockfd, (void *)&childs[i], sizeof(struct runns_child)) == -1)
          ERR("Can't send child info to the client %d", cred.uid);
      }
      close(data_sockfd);
      continue;
    }

    // Read program name and network namespace name
    program = (char *)malloc(hdr.prog_sz);
    netns = (char *)malloc(hdr.netns_sz);
    if (!program || !netns)
      ERR("Can't allocate memory (program=%p, netns=%p", program, netns);
    ret = read(data_sockfd, (void *)program, hdr.prog_sz);
    ret = read(data_sockfd, (void *)netns, hdr.netns_sz);
    INFO("uid=%d program=%s netns=%s", cred.uid, program, netns);

    // Read argv for the program
    if (hdr.args_sz)
    {
      hdr.args_sz += 2; // program name + null at the end
      args = (char **)malloc(hdr.args_sz*sizeof(char *));
      if (!args)
        ERR("Can't allocate memory (args = %p)", args);
      args[0] = program;
      args[hdr.args_sz - 1] = 0;
      for (int i = 1; i < hdr.args_sz - 1; i++)
      {
        size_t sz;
        ret = read(data_sockfd, (void *)&sz, sizeof(size_t));
        args[i] = (char *)malloc(sz);
        ret = read(data_sockfd, (void *)args[i], sz);
      }
    }
    else
      args = 0;

    // Read environment variables
    if (hdr.env_sz)
    {
      envs = (char **)malloc(++hdr.env_sz*sizeof(char *));
      for (int i = 0; i < hdr.env_sz - 1; i++)
      {
        size_t env_sz;
        ret = read(data_sockfd, (void *)&env_sz, sizeof(size_t));
        envs[i] = (char *)malloc(env_sz);
        ret = read(data_sockfd, (void *)envs[i], env_sz);
      }
      envs[hdr.env_sz - 1] = 0;
    }
    else
      envs = 0;

    close(data_sockfd);

    clean_pids();
    if (childs_run < MAX_CHILDS)
    {
      // Make fork
      pid_t child = fork();
      if (child == -1)
        ERR("Fail on fork");

      // Child
      if (child == 0)
      {
        child = fork();
        if (child != 0)
        {
          *glob_pid = child;
          exit(0);
        }

        // Un-map shared memory from the parent.
        munmap(glob_pid, sizeof(glob_pid));

        // Detach child from parent
        setsid();

        // Redirect stdin, stdout, stderr to new PTS
        if (hdr.flag & RUNNS_NPTMS)
        {
          int err;
          if (err = create_ptms())
          {
            exit(err);
          }
        }

        int netfd = open(netns, 0);
        setns(netfd, CLONE_NEWNET);
        drop_priv(cred.uid, &pw);
        if (execve(program, (char * const *)args, (char * const *)envs) == -1)
        {
          WARN("Can not run %s, execve failed with errno=%d", program, errno);
          return EXIT_FAILURE;
        }
      }
      else
        waitpid(child, 0, 0);

      // Save child.
      INFO("Forked %d", *glob_pid);
      childs[childs_run].uid = cred.uid;
      childs[childs_run].pid = *glob_pid;
      ++childs_run;
      free_tvars();
    }
    else
      INFO("Maximum number of childs has been reached.");
  }

  return 0;
}

int
drop_priv(uid_t _uid, struct passwd **pw)
{
  *pw = getpwuid(_uid);
  if (*pw) {
    uid_t uid = (*pw)->pw_uid;
    gid_t gid = (*pw)->pw_gid;

    if (initgroups((*pw)->pw_name, gid) != 0)
      ERR("Couldn't initialize the supplementary group list");
    endpwent();

    if (setgid(gid) != 0 || setuid(uid) != 0) {
      ERR("Couldn't change to '%.32s' uid=%lu gid=%lu",
          (*pw)->pw_name,
          (unsigned long)uid,
          (unsigned long)gid);
    }

    if (chdir((*pw)->pw_dir))
    {
      ERR("Couldn't chdir to %s for '%.32s' uid=%lu gid=%lu",
          (*pw)->pw_dir,
          (*pw)->pw_name,
          (unsigned long)uid,
          (unsigned long)gid);
    }
  }
  else
    ERR("Couldn't find user '%.32s'", (*pw)->pw_name);
}

void
stop_daemon(int flag)
{
  INFO("runns daemon going down");
  if (sockfd)
  {
    close(sockfd);
    unlink(defsock);
    rmdir(RUNNS_DIR);
  }
  free_tvars();
  munmap(glob_pid, sizeof(glob_pid));

  int ret = flag ? flag & RUNNS_STOP : EXIT_FAILURE;
  exit(ret);
}

int
clean_pids()
{
  for (unsigned int i = childs_run; i > 0 ; i--)
  {
    if (kill(childs[i - 1].pid, 0))
    {
      if (i != childs_run)
      {
        memcpy(childs + i - 1, childs + childs_run - 1, sizeof(struct runns_child));
      }
      --childs_run;
    }
  }
}

void
free_tvars()
{
  free(program);
  free(netns);
  if (args)
    for (size_t i = 1; i < hdr.args_sz - 3; free(args[i++]));
  if (envs)
    for (size_t i = 0; i < hdr.env_sz - 2; free(envs[i++]));
  free(envs);
  free(args);
  memset((void *)&hdr, 1, sizeof(hdr));
}

int
create_ptms()
{
  int ptmfd = open("/dev/ptmx", O_RDWR);
  char ptsname[0xff];
  if (ptsname_r(ptmfd, ptsname, 0xff))
  {
    WARN("Fail to get ptsname, errno=%d", errno);
    return errno;
  }
  if (grantpt(ptmfd))
  {
    WARN("Fail to grant access to the slave pt, errno=%d", errno);
    return errno;
  }
  if (unlockpt(ptmfd))
  {
    WARN("Fail to unlock a pt, errno=%d", errno);
    return errno;
  }
  int ptsfd = open(ptsname, O_RDWR);
  tcsetattr(ptsfd, TCSANOW, &hdr.tmode);
  if (dup2(ptsfd, STDIN_FILENO) == -1)
  {
    WARN("Fail to dup2 pt for stdin, errno=%d", errno);
    return errno;
  }
  if (dup2(ptsfd, STDOUT_FILENO) == -1)
  {
    WARN("Fail to dup2 pt for stdout, errno=%d", errno);
    return errno;
  }
  if (dup2(ptsfd, STDERR_FILENO) == -1)
  {
    WARN("Fail to dup2 pt for stderr, errno=%d", errno);
    return errno;
  }

  return 0;
}
