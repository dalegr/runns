/*
 * Copyright (c) 2019, 2020 Nikita (sh1r4s3) Ermakov <sh1r4s3@mail.si-head.nl>
 * SPDX-License-Identifier: MIT
 */

#include "runns.h"

// Emit error message and exit
#define ERR(format, ...) \
      fprintf(stderr, "client.c:%d / errno=%d / " format "\n", __LINE__, errno, ##__VA_ARGS__); \
      ret = EXIT_FAILURE;

enum wide_opts {OPT_SET_NETNS = 0xFF01};

struct option opts[] =
{
  { .name = "help", .has_arg = 0, .flag = 0, .val = 'h' },
  { .name = "program", .has_arg = 1, .flag = 0, .val = 'p' },
  { .name = "verbose", .has_arg = 0, .flag = 0, .val = 'v' },
  { .name = "stop", .has_arg = 0, .flag = 0, .val = 's' },
  { .name = "list", .has_arg = 0, .flag = 0, .val = 'l' },
  { .name = "create-ptms", .has_arg = 0, .flag = 0, .val = 't' },
  { .name = "set-netns", .has_arg = 1, .flag = 0, .val = OPT_SET_NETNS },
  { 0, 0, 0, 0 }
};

extern char **environ;

void
help_me()
{
  const char *hstr = \
"client [options]\n"                                            \
"Options:\n"                                                    \
"-h|--help             help\n"                                  \
"-s|--stop             stop daemon (only root)\n"               \
"-l|--list             list childs\n"                           \
"-p|--program <path>   program to run in desired netns\n"       \
"-t|--create-ptms      create control terminal\n"               \
"--set-netns <path>    network namespace to switch\n"           \
"-v|--verbose          be verbose\n";

  puts(hstr);
  exit(EXIT_SUCCESS);
}

int
main(int argc, char **argv)
{
  struct runns_header hdr = {0};
  struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = defsock};
  const char *prog = 0, *netns = 0, *args = 0;
  const char *optstring = "hp:vslt";
  int opt;
  char verbose = 0;
  int sockfd = 0;
  int ret = EXIT_SUCCESS;

  if (argc <= 1)
  {
    ERR("For the help message try: runnsctl --help");
    goto _exit;
  }

  while ((opt = getopt_long(argc, argv, optstring, opts, 0)) != -1)
  {
    switch (opt)
    {
      case 'h':
        help_me();
        break;
      case 's':
        hdr.flag |= RUNNS_STOP;
        break;
      case 'l':
        hdr.flag |= RUNNS_LIST;
        break;
      case 'p':
        prog = optarg;
        hdr.prog_sz = strlen(prog) + 1;
        break;
      case 't':
        hdr.flag |= RUNNS_NPTMS;
        break;
      case 'v':
        verbose = 1;
        break;
    case OPT_SET_NETNS:
        netns = optarg;
        hdr.netns_sz = strlen(netns) + 1;
        break;
      default:
        ERR("Wrong option: %c", (char)opt);
        goto _exit;
    }
  }

  // Not allow empty strings
  if (!hdr.flag && (!netns || !prog))
  {
    ERR("Please check that you set network namespace and program");
    goto _exit;
  }

  // Output parameters in the case of verbose option
  if (verbose && !hdr.flag)
  {
    printf("network namespace to switch is: %s\n" \
           "program to run: %s\n", \
           netns, prog);
  }

  // Count number of environment variables
  for (hdr.env_sz = 0; environ[hdr.env_sz] != 0; ++hdr.env_sz);

  // Up socket
  sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockfd == -1)
  {
    ERR("Something gone very wrong, socket = %d", sockfd);
    goto _exit;
  }
  if (connect(sockfd, (const struct sockaddr *)&addr, sizeof(addr)) == -1)
  {
    ERR("Can't connect to runns daemon");
    goto _exit;
  }
  // Calculate number of non-options
  hdr.args_sz = argc - optind;

  // Get termios
  tcgetattr(STDIN_FILENO, &hdr.tmode);

  if (write(sockfd, (void *)&hdr, sizeof(hdr)) == -1)
    ERR("Can't send header to the daemon");
  // Stop daemon
  if (hdr.flag & RUNNS_STOP)
    goto _exit;
  // Print list of childs and exit
  if (hdr.flag & RUNNS_LIST)
  {
    unsigned int childs_run;
    struct runns_child child;
    if (read(sockfd, (void *)&childs_run, sizeof(childs_run)) == -1)
      ERR("Can't read number of childs from the daemon");
    for (int i = 0; i < childs_run; i++)
    {
      if (read(sockfd, (void *)&child, sizeof(child)) == -1)
        ERR("Can't read child info from the daemon");
      printf("%d\n", child.pid);
    }
    goto _exit;
  }

  if (write(sockfd, (void *)prog, hdr.prog_sz) == -1 ||
      write(sockfd, (void *)netns, hdr.netns_sz) == -1)
  {
    ERR("Can't send program name or network namespace name to the daemon");
  }
  // Transfer argv
  if (hdr.args_sz > 0)
  {
    for (int i = optind; i < argc; i++)
    {
      size_t sz = strlen(argv[i]) + 1; // strlen + \0
      if (write(sockfd, (void *)&sz, sizeof(size_t)) == -1 ||
          write(sockfd, (void *)argv[i], sz) == -1)
      {
        ERR("Can't send argv to the daemon");
      }
    }
  }

  // Transfer environment variables
  for (int i = 0; i < hdr.env_sz; i++)
  {
    size_t sz = strlen(environ[i]) + 1; // strlen + \0
    if (write(sockfd, (void *)&sz, sizeof(size_t)) == -1 ||
        write(sockfd, (void *)environ[i], sz) == -1)
    {
      ERR("Can't send envs to the daemon");
    }
  }
  int eof = 0;
  if (write(sockfd, &eof, sizeof(int)) == -1)
    ERR("Can't send EOF to the daemon");

_exit:
  if (sockfd)
    close(sockfd);
  return ret;
}
