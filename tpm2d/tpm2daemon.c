/* tpm2daemon.c  -  The GnuPG tpm2 Daemon
 * Copyright (C) 2001-2002, 2004-2005, 2007-2009 Free Software Foundation, Inc.
 * Copyright (C) 2001-2002, 2004-2005, 2007-2014 Werner Koch
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <fcntl.h>
#ifndef HAVE_W32_SYSTEM
#include <sys/socket.h>
#include <sys/un.h>
#endif /*HAVE_W32_SYSTEM*/
#include <unistd.h>
#include <signal.h>
#include <npth.h>

#define INCLUDED_BY_MAIN_MODULE 1
#define GNUPG_COMMON_NEED_AFLOCAL
#include "tpm2daemon.h"
#include <gcrypt.h>

#include <assuan.h> /* malloc hooks */

#include "../common/i18n.h"
#include "../common/sysutils.h"
#include "../common/gc-opt-flags.h"
#include "../common/asshelp.h"
#include "../common/exechelp.h"
#include "../common/init.h"

#ifndef ENAMETOOLONG
# define ENAMETOOLONG EINVAL
#endif

enum cmd_and_opt_values
{ aNull = 0,
  oCsh            = 'c',
  oQuiet          = 'q',
  oSh             = 's',
  oVerbose        = 'v',

  oNoVerbose = 500,
  aGPGConfList,
  aGPGConfTest,
  oOptions,
  oDebug,
  oDebugAll,
  oDebugLevel,
  oDebugWait,
  oDebugAllowCoreDump,
  oDebugLogTid,
  oDebugAssuanLogCats,
  oNoGreeting,
  oNoOptions,
  oHomedir,
  oNoDetach,
  oNoGrab,
  oLogFile,
  oServer,
  oMultiServer,
  oDaemon,
  oListenBacklog,
  oParent
};



static gpgrt_opt_t opts[] = {
  ARGPARSE_c (aGPGConfList, "gpgconf-list", "@"),
  ARGPARSE_c (aGPGConfTest, "gpgconf-test", "@"),

  ARGPARSE_group (301, N_("@Options:\n ")),

  ARGPARSE_s_n (oServer,"server", N_("run in server mode (foreground)")),
  ARGPARSE_s_n (oMultiServer, "multi-server",
                N_("run in multi server mode (foreground)")),
  ARGPARSE_s_n (oDaemon, "daemon", N_("run in daemon mode (background)")),
  ARGPARSE_s_n (oVerbose, "verbose", N_("verbose")),
  ARGPARSE_s_n (oQuiet, "quiet", N_("be somewhat more quiet")),
  ARGPARSE_s_n (oSh,    "sh", N_("sh-style command output")),
  ARGPARSE_s_n (oCsh,   "csh", N_("csh-style command output")),
  ARGPARSE_s_s (oOptions, "options", N_("|FILE|read options from FILE")),
  ARGPARSE_s_s (oDebug, "debug", "@"),
  ARGPARSE_s_n (oDebugAll, "debug-all", "@"),
  ARGPARSE_s_s (oDebugLevel, "debug-level" ,
                N_("|LEVEL|set the debugging level to LEVEL")),
  ARGPARSE_s_i (oDebugWait, "debug-wait", "@"),
  ARGPARSE_s_n (oDebugAllowCoreDump, "debug-allow-core-dump", "@"),
  ARGPARSE_s_n (oDebugLogTid, "debug-log-tid", "@"),
  ARGPARSE_p_u (oDebugAssuanLogCats, "debug-assuan-log-cats", "@"),
  ARGPARSE_s_n (oNoDetach, "no-detach", N_("do not detach from the console")),
  ARGPARSE_s_s (oLogFile,  "log-file", N_("|FILE|write a log to FILE")),
  ARGPARSE_s_s (oHomedir,    "homedir",      "@"),
  ARGPARSE_s_i (oListenBacklog, "listen-backlog", "@"),
  ARGPARSE_p_u (oParent, "tpm2-parent",
		N_("Specify tpm2 parent for key")),

  ARGPARSE_end ()
};


/* The list of supported debug flags.  */
static struct debug_flags_s debug_flags [] =
  {
    { DBG_MPI_VALUE    , "mpi"     },
    { DBG_CRYPTO_VALUE , "crypto"  },
    { DBG_IPC_VALUE    , "ipc"     },
    { 0, NULL }
  };


/* The timer tick used to check card removal.

   We poll every 500ms to let the user immediately know a status
   change.

   For a card reader with an interrupt endpoint, this timer is not
   used with the internal CCID driver.

   This is not too good for power saving but given that there is no
   easy way to block on card status changes it is the best we can do.
   For PC/SC we could in theory use an extra thread to wait for status
   changes but that requires a native thread because there is no way
   to make the underlying PC/SC card change function block using a Npth
   mechanism.  Given that a native thread could only be used under W32
   we don't do that at all.  */
#define TIMERTICK_INTERVAL_SEC     (0)
#define TIMERTICK_INTERVAL_USEC    (500000)

/* Flag to indicate that a shutdown was requested. */
static int shutdown_pending;

/* It is possible that we are currently running under setuid permissions */
static int maybe_setuid = 1;

/* Flag telling whether we are running as a pipe server.  */
static int pipe_server;

/* Name of the communication socket */
static char *socket_name;
/* Name of the redirected socket or NULL.  */
static char *redir_socket_name;

/* We need to keep track of the server's nonces (these are dummies for
   POSIX systems). */
static assuan_sock_nonce_t socket_nonce;

/* Value for the listen() backlog argument.  Change at runtime with
 * --listen-backlog.  */
static int listen_backlog = 64;

#ifdef HAVE_W32_SYSTEM
static HANDLE the_event;
#else
/* PID to notify update of usb devices.  */
static pid_t main_thread_pid;
#endif
#ifdef HAVE_PSELECT_NO_EINTR
/* FD to notify changes.  */
static int notify_fd;
#endif

static char *create_socket_name (char *standard_name);
static gnupg_fd_t create_server_socket (const char *name,
                                        char **r_redir_name,
                                        assuan_sock_nonce_t *nonce);

static void *start_connection_thread (void *arg);
static void handle_connections (gnupg_fd_t listen_fd);

static int active_connections;


static char *
make_libversion (const char *libname, const char *(*getfnc)(const char*))
{
  const char *s;
  char *result;

  if (maybe_setuid)
    {
      gcry_control (GCRYCTL_INIT_SECMEM, 0, 0);  /* Drop setuid. */
      maybe_setuid = 0;
    }
  s = getfnc (NULL);
  result = xmalloc (strlen (libname) + 1 + strlen (s) + 1);
  strcpy (stpcpy (stpcpy (result, libname), " "), s);
  return result;
}


static const char *
my_strusage (int level)
{
  static char *ver_gcry;
  const char *p;

  switch (level)
    {
    case 11: p = "@TPM2DAEMON@ (@GNUPG@)";
      break;
    case 13: p = VERSION; break;
    case 17: p = PRINTABLE_OS_NAME; break;
    case 19: p = _("Please report bugs to <@EMAIL@>.\n"); break;

    case 20:
      if (!ver_gcry)
        ver_gcry = make_libversion ("libgcrypt", gcry_check_version);
      p = ver_gcry;
      break;
    case 1:
    case 40: p =  _("Usage: @TPM2DAEMON@ [options] (-h for help)");
      break;
    case 41: p =  _("Syntax: tpm2daemon [options] [command [args]]\n"
                    "TPM2 daemon for @GNUPG@\n");
    break;

    default: p = NULL;
    }
  return p;
}


static int
tid_log_callback (unsigned long *rvalue)
{
  int len = sizeof (*rvalue);
  npth_t thread;

  thread = npth_self ();
  if (sizeof (thread) < len)
    len = sizeof (thread);
  memcpy (rvalue, &thread, len);

  return 2; /* Use use hex representation.  */
}


/* Setup the debugging.  With a LEVEL of NULL only the active debug
   flags are propagated to the subsystems.  With LEVEL set, a specific
   set of debug flags is set; thus overriding all flags already
   set. */
static void
set_debug (const char *level)
{
  int numok = (level && digitp (level));
  int numlvl = numok? atoi (level) : 0;

  if (!level)
    ;
  else if (!strcmp (level, "none") || (numok && numlvl < 1))
    opt.debug = 0;
  else if (!strcmp (level, "basic") || (numok && numlvl <= 2))
    opt.debug = DBG_IPC_VALUE;
  else if (!strcmp (level, "advanced") || (numok && numlvl <= 5))
    opt.debug = DBG_IPC_VALUE;
  else if (!strcmp (level, "expert") || (numok && numlvl <= 8))
    opt.debug = DBG_IPC_VALUE;
  else if (!strcmp (level, "guru") || numok)
      opt.debug = ~0;
  else
    {
      log_error (_("invalid debug-level '%s' given\n"), level);
      tpm2d_exit (2);
    }


  if (opt.debug && !opt.verbose)
    opt.verbose = 1;
  if (opt.debug && opt.quiet)
    opt.quiet = 0;

  if (opt.debug & DBG_MPI_VALUE)
    gcry_control (GCRYCTL_SET_DEBUG_FLAGS, 2);
  if (opt.debug & DBG_CRYPTO_VALUE )
    gcry_control (GCRYCTL_SET_DEBUG_FLAGS, 1);
  gcry_control (GCRYCTL_SET_VERBOSITY, (int)opt.verbose);

  if (opt.debug)
    parse_debug_flag (NULL, &opt.debug, debug_flags);
}



static void
cleanup (void)
{
  if (socket_name && *socket_name)
    {
      char *name;

      name = redir_socket_name? redir_socket_name : socket_name;

      gnupg_remove (name);
      *socket_name = 0;
    }
}



int
main (int argc, char **argv )
{
  gpgrt_argparse_t pargs;
  int orig_argc;
  char **orig_argv;
  char *last_configname = NULL;
  const char *configname = NULL;
  const char *shell;
  int parse_debug = 0;
  const char *debug_level = NULL;
  int greeting = 0;
  int nogreeting = 0;
  int multi_server = 0;
  int is_daemon = 0;
  int nodetach = 0;
  int csh_style = 0;
  char *logfile = NULL;
  int debug_wait = 0;
  int gpgconf_list = 0;
  char *config_filename = NULL;
  int allow_coredump = 0;
  struct assuan_malloc_hooks malloc_hooks;
  int res;
  npth_t pipecon_handler;

  early_system_init ();
  gpgrt_set_strusage (my_strusage);
  gcry_control (GCRYCTL_SUSPEND_SECMEM_WARN);
  /* Please note that we may running SUID(ROOT), so be very CAREFUL
     when adding any stuff between here and the call to INIT_SECMEM()
     somewhere after the option parsing */
  log_set_prefix ("tpm2daemon", GPGRT_LOG_WITH_PREFIX | GPGRT_LOG_WITH_PID);

  /* Make sure that our subsystems are ready.  */
  i18n_init ();
  init_common_subsystems (&argc, &argv);

  malloc_hooks.malloc = gcry_malloc;
  malloc_hooks.realloc = gcry_realloc;
  malloc_hooks.free = gcry_free;
  assuan_set_malloc_hooks (&malloc_hooks);
  assuan_set_gpg_err_source (GPG_ERR_SOURCE_DEFAULT);
  assuan_sock_init ();
  setup_libassuan_logging (&opt.debug, NULL);

  setup_libgcrypt_logging ();
  gcry_control (GCRYCTL_USE_SECURE_RNDPOOL);

  disable_core_dumps ();

  /* Set default options. */
  opt.parent = 0;		/* 0 means TPM uses default */

  shell = getenv ("SHELL");
  if (shell && strlen (shell) >= 3 && !strcmp (shell+strlen (shell)-3, "csh") )
    csh_style = 1;

  /* Check whether we have a config file on the commandline */
  orig_argc = argc;
  orig_argv = argv;
  pargs.argc = &argc;
  pargs.argv = &argv;
  pargs.flags= (ARGPARSE_FLAG_KEEP | ARGPARSE_FLAG_NOVERSION);
  while (gpgrt_argparse (NULL, &pargs, opts))
    {
      switch (pargs.r_opt)
        {
        case oDebug:
        case oDebugAll:
          parse_debug++;
          break;
        case oHomedir:
          gnupg_set_homedir (pargs.r.ret_str);
          break;
        }
    }
  /* Reset the flags.  */
  pargs.flags &= ~(ARGPARSE_FLAG_KEEP | ARGPARSE_FLAG_NOVERSION);

  /* initialize the secure memory. */
  gcry_control (GCRYCTL_INIT_SECMEM, 16384, 0);
  maybe_setuid = 0;

  /*
     Now we are working under our real uid
  */


  /* The configuration directories for use by gpgrt_argparser.  */
  gpgrt_set_confdir (GPGRT_CONFDIR_SYS, gnupg_sysconfdir ());
  gpgrt_set_confdir (GPGRT_CONFDIR_USER, gnupg_homedir ());

  /* We are re-using the struct, thus the reset flag.  We OR the
   * flags so that the internal initialized flag won't be cleared. */
  argc = orig_argc;
  argv = orig_argv;
  pargs.argc = &argc;
  pargs.argv = &argv;
  pargs.flags |=  (ARGPARSE_FLAG_RESET
                   | ARGPARSE_FLAG_KEEP
                   | ARGPARSE_FLAG_SYS
                   | ARGPARSE_FLAG_USER);
  while (gpgrt_argparser (&pargs, opts, TPM2DAEMON_NAME EXTSEP_S "conf"))
    {
      switch (pargs.r_opt)
        {
        case ARGPARSE_CONFFILE:
          if (parse_debug)
            log_info (_("reading options from '%s'\n"),
                      pargs.r_type? pargs.r.ret_str: "[cmdline]");
          if (pargs.r_type)
            {
              xfree (last_configname);
              last_configname = xstrdup (pargs.r.ret_str);
              configname = last_configname;
            }
          else
            configname = NULL;
          break;

        case aGPGConfList: gpgconf_list = 1; break;
        case aGPGConfTest: gpgconf_list = 2; break;
        case oQuiet: opt.quiet = 1; break;
        case oVerbose: opt.verbose++; break;

        case oDebug:
          if (parse_debug_flag (pargs.r.ret_str, &opt.debug, debug_flags))
            {
              pargs.r_opt = ARGPARSE_INVALID_ARG;
              pargs.err = ARGPARSE_PRINT_ERROR;
            }
          break;
        case oDebugAll: opt.debug = ~0; break;
        case oDebugLevel: debug_level = pargs.r.ret_str; break;
        case oDebugWait: debug_wait = pargs.r.ret_int; break;
        case oDebugAllowCoreDump:
          enable_core_dumps ();
          allow_coredump = 1;
          break;
        case oDebugLogTid:
          log_set_pid_suffix_cb (tid_log_callback);
          break;
        case oDebugAssuanLogCats:
          set_libassuan_log_cats (pargs.r.ret_ulong);
          break;

        case oNoGreeting: nogreeting = 1; break;
        case oNoVerbose: opt.verbose = 0; break;
        case oNoOptions: break; /* no-options */
        case oHomedir: gnupg_set_homedir (pargs.r.ret_str); break;
        case oNoDetach: nodetach = 1; break;
        case oLogFile: logfile = pargs.r.ret_str; break;
        case oCsh: csh_style = 1; break;
        case oSh: csh_style = 0; break;
        case oServer: pipe_server = 1; break;
        case oMultiServer: pipe_server = 1; multi_server = 1; break;
        case oDaemon: is_daemon = 1; break;

        case oListenBacklog:
          listen_backlog = pargs.r.ret_int;
          break;

	case oParent:
	  opt.parent = pargs.r.ret_ulong;
	  break;

        default:
          if (configname)
            pargs.err = ARGPARSE_PRINT_WARNING;
          else
            pargs.err = ARGPARSE_PRINT_ERROR;
          break;
        }
    }
  gpgrt_argparse (NULL, &pargs, NULL);  /* Release internal state.  */

  if (!last_configname)
    config_filename = gpgrt_fnameconcat (gnupg_homedir (),
                                         TPM2DAEMON_NAME EXTSEP_S "conf",
                                         NULL);
  else
    {
      config_filename = last_configname;
      last_configname = NULL;
    }

  if (log_get_errorcount (0))
    exit (2);
  if (nogreeting )
    greeting = 0;

  if (greeting)
    {
      es_fprintf (es_stderr, "%s %s; %s\n",
                  gpgrt_strusage (11), gpgrt_strusage (13),
		  gpgrt_strusage (14) );
      es_fprintf (es_stderr, "%s\n", gpgrt_strusage (15) );
    }
#ifdef IS_DEVELOPMENT_VERSION
  log_info ("NOTE: this is a development version!\n");
#endif

  /* Print a warning if an argument looks like an option.  */
  if (!opt.quiet && !(pargs.flags & ARGPARSE_FLAG_STOP_SEEN))
    {
      int i;

      for (i=0; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] == '-')
          log_info (_("Note: '%s' is not considered an option\n"), argv[i]);
    }

  if (atexit (cleanup))
    {
      log_error ("atexit failed\n");
      cleanup ();
      exit (1);
    }

  set_debug (debug_level);

  if (gpgconf_list == 2)
    tpm2d_exit (0);
  if (gpgconf_list)
    {
      es_printf ("verbose:%lu:\n"
                 "quiet:%lu:\n"
                 "debug-level:%lu:\"none:\n"
                 "log-file:%lu:\n",
                 GC_OPT_FLAG_NONE,
                 GC_OPT_FLAG_NONE,
                 GC_OPT_FLAG_DEFAULT,
                 GC_OPT_FLAG_NONE );

      tpm2d_exit (0);
    }

  /* Now start with logging to a file if this is desired.  */
  if (logfile)
    {
      log_set_file (logfile);
      log_set_prefix (NULL, GPGRT_LOG_WITH_PREFIX | GPGRT_LOG_WITH_TIME | GPGRT_LOG_WITH_PID);
    }

  if (debug_wait && pipe_server)
    {
      log_debug ("waiting for debugger - my pid is %u .....\n",
                 (unsigned int)getpid ());
      gnupg_sleep (debug_wait);
      log_debug ("... okay\n");
    }

  if (pipe_server)
    {
      /* This is the simple pipe based server */
      ctrl_t ctrl;
      npth_attr_t tattr;
      gnupg_fd_t fd = GNUPG_INVALID_FD;

#ifndef HAVE_W32_SYSTEM
      {
        struct sigaction sa;

        sa.sa_handler = SIG_IGN;
        sigemptyset (&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction (SIGPIPE, &sa, NULL);
      }
#endif

      npth_init ();
      gpgrt_set_syscall_clamp (npth_unprotect, npth_protect);
      assuan_control (ASSUAN_CONTROL_REINIT_SYSCALL_CLAMP, NULL);

      /* If --debug-allow-core-dump has been given we also need to
         switch the working directory to a place where we can actually
         write. */
      if (allow_coredump)
        {
          if (chdir ("/tmp"))
            log_debug ("chdir to '/tmp' failed: %s\n", strerror (errno));
          else
            log_debug ("changed working directory to '/tmp'\n");
        }

      /* In multi server mode we need to listen on an additional
         socket.  Create that socket now before starting the handler
         for the pipe connection.  This allows that handler to send
         back the name of that socket. */
      if (multi_server)
        {
          socket_name = create_socket_name (TPM2DAEMON_SOCK_NAME);
          fd = create_server_socket (socket_name,
                                     &redir_socket_name,
                                     &socket_nonce);
        }

      res = npth_attr_init (&tattr);
      if (res)
        {
          log_error ("error allocating thread attributes: %s\n",
                     strerror (res));
          tpm2d_exit (2);
        }
      npth_attr_setdetachstate (&tattr, NPTH_CREATE_DETACHED);

      ctrl = xtrycalloc (1, sizeof *ctrl);
      if ( !ctrl )
        {
          log_error ("error allocating connection control data: %s\n",
                     strerror (errno) );
          tpm2d_exit (2);
        }
      ctrl->thread_startup.fd = GNUPG_INVALID_FD;
      res = npth_create (&pipecon_handler, &tattr, start_connection_thread, ctrl);
      if (res)
        {
          log_error ("error spawning pipe connection handler: %s\n",
                     strerror (res) );
          xfree (ctrl);
          tpm2d_exit (2);
        }
      npth_setname_np (pipecon_handler, "pipe-connection");
      npth_attr_destroy (&tattr);

      /* We run handle_connection to wait for the shutdown signal and
         to run the ticker stuff.  */
      handle_connections (fd);
      if (fd != GNUPG_INVALID_FD)
        assuan_sock_close (fd);
    }
  else if (!is_daemon)
    {
      log_info (_("please use the option '--daemon'"
                  " to run the program in the background\n"));
    }
  else
    { /* Regular server mode */
      gnupg_fd_t fd;
#ifndef HAVE_W32_SYSTEM
      pid_t pid;
      int i;
#endif

      /* Create the socket.  */
      socket_name = create_socket_name (TPM2DAEMON_SOCK_NAME);
      fd = create_server_socket (socket_name,
                                 &redir_socket_name, &socket_nonce);


      fflush (NULL);
#ifdef HAVE_W32_SYSTEM
      (void)csh_style;
      (void)nodetach;
#else
      pid = fork ();
      if (pid == (pid_t)-1)
        {
          log_fatal ("fork failed: %s\n", strerror (errno) );
          exit (1);
        }
      else if (pid)
        { /* we are the parent */
          char *infostr;

          close (fd);

          /* create the info string: <name>:<pid>:<protocol_version> */
          if (gpgrt_asprintf (&infostr, "TPM2DAEMON_INFO=%s:%lu:1",
                              socket_name, (ulong) pid) < 0)
            {
              log_error ("out of core\n");
              kill (pid, SIGTERM);
              exit (1);
            }
          *socket_name = 0; /* don't let cleanup() remove the socket -
                               the child should do this from now on */
          if (argc)
            { /* run the program given on the commandline */
              if (putenv (infostr))
                {
                  log_error ("failed to set environment: %s\n",
                             strerror (errno) );
                  kill (pid, SIGTERM );
                  exit (1);
                }
              execvp (argv[0], argv);
              log_error ("failed to run the command: %s\n", strerror (errno));
              kill (pid, SIGTERM);
              exit (1);
            }
          else
            {
              /* Print the environment string, so that the caller can use
                 shell's eval to set it */
              if (csh_style)
                {
                  *strchr (infostr, '=') = ' ';
                  es_printf ( "setenv %s;\n", infostr);
                }
              else
                {
                  es_printf ( "%s; export TPM2DAEMON_INFO;\n", infostr);
                }
              xfree (infostr);
              exit (0);
            }
          /* NOTREACHED */
        } /* end parent */

      /* This is the child. */

      npth_init ();
      gpgrt_set_syscall_clamp (npth_unprotect, npth_protect);
      assuan_control (ASSUAN_CONTROL_REINIT_SYSCALL_CLAMP, NULL);

      /* Detach from tty and put process into a new session. */
      if (!nodetach )
        {
          /* Close stdin, stdout and stderr unless it is the log stream. */
          for (i=0; i <= 2; i++)
            {
              if (!log_test_fd (i) && i != fd )
                {
                  if ( !close (i)
                       && open ("/dev/null", i? O_WRONLY : O_RDONLY) == -1)
                    {
                      log_error ("failed to open '%s': %s\n",
                                 "/dev/null", strerror (errno));
                      cleanup ();
                      exit (1);
                    }
                }
            }

          if (setsid () == -1)
            {
              log_error ("setsid() failed: %s\n", strerror (errno) );
              cleanup ();
              exit (1);
            }
        }

      {
        struct sigaction sa;

        sa.sa_handler = SIG_IGN;
        sigemptyset (&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction (SIGPIPE, &sa, NULL);
      }

#endif /*!HAVE_W32_SYSTEM*/

      if (gnupg_chdir (gnupg_daemon_rootdir ()))
        {
          log_error ("chdir to '%s' failed: %s\n",
                     gnupg_daemon_rootdir (), strerror (errno));
          exit (1);
        }

      handle_connections (fd);

      assuan_sock_close (fd);
    }

  xfree (config_filename);
  return 0;
}

void
tpm2d_exit (int rc)
{
  gcry_control (GCRYCTL_TERM_SECMEM );
  rc = rc? rc : log_get_errorcount (0)? 2 : 0;
  exit (rc);
}


static void
tpm2d_init_default_ctrl (ctrl_t ctrl)
{
  (void)ctrl;
}

static void
tpm2d_deinit_default_ctrl (ctrl_t ctrl)
{
  if (!ctrl)
    return;
  xfree (ctrl->in_data.value);
  ctrl->in_data.value = NULL;
  ctrl->in_data.valuelen = 0;
}


/* Return the name of the socket to be used to connect to this
   process.  If no socket is available, return NULL. */
const char *
tpm2d_get_socket_name (void)
{
  if (socket_name && *socket_name)
    return socket_name;
  return NULL;
}


#ifndef HAVE_W32_SYSTEM
static void
handle_signal (int signo)
{
  switch (signo)
    {
    case SIGHUP:
      log_info ("SIGHUP received - "
                "re-reading configuration\n");
/*       reread_configuration (); */
      break;

    case SIGUSR1:
      log_info ("SIGUSR1 received - printing internal information:\n");
      /* Fixme: We need to see how to integrate pth dumping into our
         logging system.  */
      /* pth_ctrl (PTH_CTRL_DUMPSTATE, log_get_stream ()); */
#if 0
      app_dump_state ();
#endif
      break;

    case SIGUSR2:
      log_info ("SIGUSR2 received - no action defined\n");
      break;

    case SIGCONT:
      /* Nothing.  */
      log_debug ("SIGCONT received - breaking select\n");
      break;

    case SIGTERM:
      if (!shutdown_pending)
        log_info ("SIGTERM received - shutting down ...\n");
      else
        log_info ("SIGTERM received - still %i running threads\n",
                  active_connections);
      shutdown_pending++;
      if (shutdown_pending > 2)
        {
          log_info ("shutdown forced\n");
          log_info ("%s %s stopped\n", gpgrt_strusage (11),
		    gpgrt_strusage (13) );
          cleanup ();
          tpm2d_exit (0);
        }
      break;

    case SIGINT:
      log_info ("SIGINT received - immediate shutdown\n");
      log_info ( "%s %s stopped\n", gpgrt_strusage (11), gpgrt_strusage (13));
      cleanup ();
      tpm2d_exit (0);
      break;

    default:
      log_info ("signal %d received - no action defined\n", signo);
    }
}
#endif /*!HAVE_W32_SYSTEM*/


/* Create a name for the socket.  We check for valid characters as
   well as against a maximum allowed length for a unix domain socket
   is done.  The function terminates the process in case of an error.
   Returns: Pointer to an allocated string with the absolute name of
   the socket used.  */
static char *
create_socket_name (char *standard_name)
{
  char *name;

  name = make_filename (gnupg_socketdir (), standard_name, NULL);
  if (strchr (name, PATHSEP_C))
    {
      log_error (("'%s' are not allowed in the socket name\n"), PATHSEP_S);
      tpm2d_exit (2);
    }
  return name;
}



/* Create a Unix domain socket with NAME.  Returns the file descriptor
   or terminates the process in case of an error.  If the socket has
   been redirected the name of the real socket is stored as a malloced
   string at R_REDIR_NAME. */
static gnupg_fd_t
create_server_socket (const char *name, char **r_redir_name,
                      assuan_sock_nonce_t *nonce)
{
  struct sockaddr *addr;
  struct sockaddr_un *unaddr;
  socklen_t len;
  gnupg_fd_t fd;
  int rc;

  xfree (*r_redir_name);
  *r_redir_name = NULL;

  fd = assuan_sock_new (AF_UNIX, SOCK_STREAM, 0);
  if (fd == GNUPG_INVALID_FD)
    {
      log_error (_("can't create socket: %s\n"), strerror (errno));
      tpm2d_exit (2);
    }

  unaddr = xmalloc (sizeof (*unaddr));
  addr = (struct sockaddr*)unaddr;

  {
    int redirected;

    if (assuan_sock_set_sockaddr_un (name, addr, &redirected))
      {
        if (errno == ENAMETOOLONG)
          log_error (_("socket name '%s' is too long\n"), name);
        else
          log_error ("error preparing socket '%s': %s\n",
                     name, gpg_strerror (gpg_error_from_syserror ()));
        tpm2d_exit (2);
      }
    if (redirected)
      {
        *r_redir_name = xstrdup (unaddr->sun_path);
        if (opt.verbose)
          log_info ("redirecting socket '%s' to '%s'\n", name, *r_redir_name);
      }
  }

  len = SUN_LEN (unaddr);

  rc = assuan_sock_bind (fd, addr, len);
  if (rc == -1 && errno == EADDRINUSE)
    {
      gnupg_remove (unaddr->sun_path);
      rc = assuan_sock_bind (fd, addr, len);
    }
  if (rc != -1
      && (rc=assuan_sock_get_nonce (addr, len, nonce)))
    log_error (_("error getting nonce for the socket\n"));
 if (rc == -1)
    {
      gpg_error_t myerr = gpg_error_from_syserror ();
      log_libassuan_system_error (fd);
      log_error (_("error binding socket to '%s': %s\n"),
                 unaddr->sun_path, gpg_strerror (myerr));
      assuan_sock_close (fd);
      tpm2d_exit (2);
    }

  if (gnupg_chmod (unaddr->sun_path, "-rwx"))
    log_error (_("can't set permissions of '%s': %s\n"),
               unaddr->sun_path, strerror (errno));

  if (listen (FD2INT (fd), listen_backlog) == -1)
    {
      log_error ("listen(fd, %d) failed: %s\n",
                 listen_backlog, gpg_strerror (gpg_error_from_syserror ()));
      assuan_sock_close (fd);
      tpm2d_exit (2);
    }

  if (opt.verbose)
    log_info (_("listening on socket '%s'\n"), unaddr->sun_path);

  return fd;
}



/* This is the standard connection thread's main function.  */
static void *
start_connection_thread (void *arg)
{
  ctrl_t ctrl = arg;

  if (ctrl->thread_startup.fd != GNUPG_INVALID_FD
      && assuan_sock_check_nonce (ctrl->thread_startup.fd, &socket_nonce))
    {
      log_info (_("error reading nonce on fd %d: %s\n"),
                FD_DBG (ctrl->thread_startup.fd), strerror (errno));
      assuan_sock_close (ctrl->thread_startup.fd);
      xfree (ctrl);
      return NULL;
    }

  active_connections++;

  tpm2d_init_default_ctrl (ctrl);
  if (opt.verbose)
    log_info (_("handler for fd %d started\n"),
              FD_DBG (ctrl->thread_startup.fd));

  /* If this is a pipe server, we request a shutdown if the command
     handler asked for it.  With the next ticker event and given that
     no other connections are running the shutdown will then
     happen.  */
  if (tpm2d_command_handler (ctrl, ctrl->thread_startup.fd)
      && pipe_server)
    shutdown_pending = 1;

  if (opt.verbose)
    log_info (_("handler for fd %d terminated\n"),
              FD_DBG (ctrl->thread_startup.fd));

  tpm2d_deinit_default_ctrl (ctrl);
  xfree (ctrl);

  if (--active_connections == 0)
    tpm2d_kick_the_loop ();

  return NULL;
}


void
tpm2d_kick_the_loop (void)
{
#ifdef HAVE_W32_SYSTEM
  int ret;

  /* Kick the select loop.  */
  ret = SetEvent (the_event);
  if (ret == 0)
    log_error ("SetEvent for tpm2d_kick_the_loop failed: %s\n",
               w32_strerror (-1));
#elif defined(HAVE_PSELECT_NO_EINTR)
  write (notify_fd, "", 1);
#else
  int ret;

  ret = kill (main_thread_pid, SIGCONT);
  if (ret < 0)
    log_error ("SetEvent for tpm2d_kick_the_loop failed: %s\n",
               gpg_strerror (gpg_error_from_syserror ()));
#endif
}

/* Connection handler loop.  Wait for connection requests and spawn a
   thread after accepting a connection.  LISTEN_FD is allowed to be -1
   in which case this code will only do regular timeouts and handle
   signals. */
static void
handle_connections (gnupg_fd_t listen_fd)
{
  npth_attr_t tattr;
  struct sockaddr_un paddr;
  socklen_t plen;
  fd_set fdset, read_fdset;
  int nfd;
  int ret;
  struct timespec timeout;
  struct timespec *t;
  int saved_errno;
#ifdef HAVE_W32_SYSTEM
  HANDLE events[2];
  unsigned int events_set;
#else
  int signo;
#endif
#ifdef HAVE_PSELECT_NO_EINTR
  int pipe_fd[2];

  ret = gnupg_create_pipe (pipe_fd, 0);
  if (ret)
    {
      log_error ("pipe creation failed: %s\n", gpg_strerror (ret));
      return;
    }
  notify_fd = pipe_fd[1];
#endif

  ret = npth_attr_init (&tattr);
  if (ret)
    {
      log_error ("npth_attr_init failed: %s\n", strerror (ret));
      return;
    }

  npth_attr_setdetachstate (&tattr, NPTH_CREATE_DETACHED);

#ifdef HAVE_W32_SYSTEM
  {
    HANDLE h, h2;
    SECURITY_ATTRIBUTES sa = { sizeof (SECURITY_ATTRIBUTES), NULL, TRUE};

    events[0] = the_event = INVALID_HANDLE_VALUE;
    events[1] = INVALID_HANDLE_VALUE;
    h = CreateEvent (&sa, TRUE, FALSE, NULL);
    if (!h)
      log_error ("can't create tpm2d event: %s\n", w32_strerror (-1) );
    else if (!DuplicateHandle (GetCurrentProcess (), h,
                               GetCurrentProcess (), &h2,
                               EVENT_MODIFY_STATE|SYNCHRONIZE, TRUE, 0))
      {
        log_error ("setting synchronize for tpm2d_kick_the_loop failed: %s\n",
                   w32_strerror (-1) );
        CloseHandle (h);
      }
    else
      {
        CloseHandle (h);
        events[0] = the_event = h2;
      }
  }
#else
  npth_sigev_init ();
  npth_sigev_add (SIGHUP);
  npth_sigev_add (SIGUSR1);
  npth_sigev_add (SIGUSR2);
  npth_sigev_add (SIGINT);
  npth_sigev_add (SIGCONT);
  npth_sigev_add (SIGTERM);
  npth_sigev_fini ();
  main_thread_pid = getpid ();
#endif

  FD_ZERO (&fdset);
  nfd = 0;
  if (listen_fd != GNUPG_INVALID_FD)
    {
      FD_SET (FD2INT (listen_fd), &fdset);
      nfd = FD2NUM (listen_fd);
    }

  for (;;)
    {
      int periodical_check;
      int max_fd = nfd;

      if (shutdown_pending)
        {
          if (active_connections == 0)
            break; /* ready */

          /* Do not accept anymore connections but wait for existing
             connections to terminate. We do this by clearing out all
             file descriptors to wait for, so that the select will be
             used to just wait on a signal or timeout event. */
          FD_ZERO (&fdset);
          listen_fd = GNUPG_INVALID_FD;
        }

      periodical_check = 0;

      timeout.tv_sec = TIMERTICK_INTERVAL_SEC;
      timeout.tv_nsec = TIMERTICK_INTERVAL_USEC * 1000;

      if (shutdown_pending || periodical_check)
        t = &timeout;
      else
        t = NULL;

      /* POSIX says that fd_set should be implemented as a structure,
         thus a simple assignment is fine to copy the entire set.  */
      read_fdset = fdset;

#ifdef HAVE_PSELECT_NO_EINTR
      FD_SET (pipe_fd[0], &read_fdset);
      if (max_fd < pipe_fd[0])
        max_fd = pipe_fd[0];
#endif

#ifndef HAVE_W32_SYSTEM
      ret = npth_pselect (max_fd+1, &read_fdset, NULL, NULL, t,
                          npth_sigev_sigmask ());
      saved_errno = errno;

      while (npth_sigev_get_pending (&signo))
        handle_signal (signo);
#else
      ret = npth_eselect (nfd+1, &read_fdset, NULL, NULL, t,
                          events, &events_set);
      saved_errno = errno;
      if (events_set & 1)
        continue;
#endif

      if (ret == -1 && saved_errno != EINTR)
        {
          log_error (_("npth_pselect failed: %s - waiting 1s\n"),
                     strerror (saved_errno));
          gnupg_sleep (1);
          continue;
        }

      if (ret <= 0)
        /* Timeout.  Will be handled when calculating the next timeout.  */
        continue;

#ifdef HAVE_PSELECT_NO_EINTR
      if (FD_ISSET (pipe_fd[0], &read_fdset))
        {
          char buf[256];

          read (pipe_fd[0], buf, sizeof buf);
        }
#endif

      if (listen_fd != GNUPG_INVALID_FD
          && FD_ISSET (FD2INT (listen_fd), &read_fdset))
        {
          ctrl_t ctrl;
          gnupg_fd_t fd;

          plen = sizeof paddr;
          fd = assuan_sock_accept (listen_fd,
                                   (struct sockaddr *)&paddr, &plen);
          if (fd == GNUPG_INVALID_FD)
            {
              gpg_error_t myerr = gpg_error_from_syserror ();
              log_libassuan_system_error (listen_fd);
              log_error ("accept failed: %s\n", gpg_strerror (myerr));
            }
          else if ( !(ctrl = xtrycalloc (1, sizeof *ctrl)) )
            {
              log_error ("error allocating connection control data: %s\n",
                         strerror (errno) );
              assuan_sock_close (fd);
            }
          else
            {
              char threadname[50];
              npth_t thread;

              snprintf (threadname, sizeof threadname, "conn fd=%d", FD_DBG (fd));
              ctrl->thread_startup.fd = fd;
              ret = npth_create (&thread, &tattr, start_connection_thread, ctrl);
              if (ret)
                {
                  log_error ("error spawning connection handler: %s\n",
                             strerror (ret));
                  xfree (ctrl);
                  assuan_sock_close (fd);
                }
              else
                npth_setname_np (thread, threadname);
            }
        }
    }

#ifdef HAVE_W32_SYSTEM
  if (the_event != INVALID_HANDLE_VALUE)
    CloseHandle (the_event);
#endif
#ifdef HAVE_PSELECT_NO_EINTR
  close (pipe_fd[0]);
  close (pipe_fd[1]);
#endif
  cleanup ();
  log_info (_("%s %s stopped\n"), gpgrt_strusage (11), gpgrt_strusage (13));
  npth_attr_destroy (&tattr);
}

/* Return the number of active connections. */
int
get_active_connection_count (void)
{
  return active_connections;
}
