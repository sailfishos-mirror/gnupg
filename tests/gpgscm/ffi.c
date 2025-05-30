/* FFI interface for TinySCHEME.
 *
 * Copyright (C) 2016 g10 code GmbH
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
 */

#include <config.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gpg-error.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#if HAVE_LIBREADLINE
#define GNUPG_LIBREADLINE_H_INCLUDED
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include "../../common/util.h"
#include "../../common/sysutils.h"
#include "../../common/exechelp.h"

#ifdef HAVE_W32_SYSTEM
#include <windows.h>
#endif

#include "private.h"
#include "ffi.h"
#include "ffi-private.h"

/* For use in nice error messages.  */
static const char *
ordinal_suffix (int n)
{
  switch (n)
    {
    case 1: return "st";
    case 2: return "nd";
    case 3: return "rd";
    default: return "th";
    }
  assert (! "reached");
}



int
ffi_bool_value (scheme *sc, pointer p)
{
  return ! (p == sc->F);
}



static pointer
do_logand (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  unsigned int v, acc = ~0;
  while (args != sc->NIL)
    {
      FFI_ARG_OR_RETURN (sc, unsigned int, v, number, args);
      acc &= v;
    }
  FFI_RETURN_INT (sc, acc);
}

static pointer
do_logior (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  unsigned int v, acc = 0;
  while (args != sc->NIL)
    {
      FFI_ARG_OR_RETURN (sc, unsigned int, v, number, args);
      acc |= v;
    }
  FFI_RETURN_INT (sc, acc);
}

static pointer
do_logxor (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  unsigned int v, acc = 0;
  while (args != sc->NIL)
    {
      FFI_ARG_OR_RETURN (sc, unsigned int, v, number, args);
      acc ^= v;
    }
  FFI_RETURN_INT (sc, acc);
}

static pointer
do_lognot (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  unsigned int v;
  FFI_ARG_OR_RETURN (sc, unsigned int, v, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  FFI_RETURN_INT (sc, ~v);
}

/* User interface.  */

static pointer
do_flush_stdio (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  fflush (stdout);
  fflush (stderr);
  FFI_RETURN (sc);
}


int use_libreadline;

/* Read a string, and return a pointer to it.  Returns NULL on EOF. */
char *
rl_gets (const char *prompt)
{
  static char *line = NULL;
  char *p;
  xfree (line);

#if HAVE_LIBREADLINE
    {
      line = readline (prompt);
      if (line && *line)
        add_history (line);
    }
#else
    {
      size_t max_size = 0xff;
      printf ("%s", prompt);
      fflush (stdout);
      line = xtrymalloc (max_size);
      if (line != NULL)
        fgets (line, max_size, stdin);
    }
#endif

  /* Strip trailing whitespace.  */
  if (line && strlen (line) > 0)
    for (p = &line[strlen (line) - 1]; isspace (*p); p--)
      *p = 0;

  return line;
}

static pointer
do_prompt (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  const char *prompt;
  const char *line;
  FFI_ARG_OR_RETURN (sc, const char *, prompt, string, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  line = rl_gets (prompt);
  if (! line)
    FFI_RETURN_POINTER (sc, sc->EOF_OBJ);

  FFI_RETURN_STRING (sc, line);
}

static pointer
do_sleep (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  unsigned int seconds;
  FFI_ARG_OR_RETURN (sc, unsigned int, seconds, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  sleep (seconds);
  FFI_RETURN (sc);
}

static pointer
do_usleep (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  useconds_t microseconds;
  FFI_ARG_OR_RETURN (sc, useconds_t, microseconds, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  usleep (microseconds);
  FFI_RETURN (sc);
}

static pointer
do_chdir (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *name;
  FFI_ARG_OR_RETURN (sc, char *, name, path, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  if (chdir (name))
    FFI_RETURN_ERR (sc, gpg_error_from_syserror ());
  FFI_RETURN (sc);
}

static pointer
do_strerror (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int error;
  FFI_ARG_OR_RETURN (sc, int, error, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  FFI_RETURN_STRING (sc, gpg_strerror (error));
}

static pointer
do_getenv (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *name;
  char *value;
  FFI_ARG_OR_RETURN (sc, char *, name, string, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  value = getenv (name);
  FFI_RETURN_STRING (sc, value ? value : "");
}

static pointer
do_setenv (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *name;
  char *value;
  int overwrite;
  FFI_ARG_OR_RETURN (sc, char *, name, string, args);
  FFI_ARG_OR_RETURN (sc, char *, value, string, args);
  FFI_ARG_OR_RETURN (sc, int, overwrite, bool, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  if (gnupg_setenv (name, value, overwrite))
    FFI_RETURN_ERR (sc, gpg_error_from_syserror ());
  FFI_RETURN (sc);
}

static pointer
do_exit (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int retcode;
  FFI_ARG_OR_RETURN (sc, int, retcode, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  exit (retcode);
}

/* XXX: use gnupgs variant b/c mode as string */
static pointer
do_open (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int fd;
  char *pathname;
  int flags;
  mode_t mode = 0;
  FFI_ARG_OR_RETURN (sc, char *, pathname, path, args);
  FFI_ARG_OR_RETURN (sc, int, flags, number, args);
  if (args != sc->NIL)
    FFI_ARG_OR_RETURN (sc, mode_t, mode, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  fd = open (pathname, flags, mode);
  if (fd == -1)
    FFI_RETURN_ERR (sc, gpg_error_from_syserror ());
  FFI_RETURN_INT (sc, fd);
}

static pointer
do_fdopen (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  FILE *stream;
  int fd;
  char *mode;
  int kind;
  FFI_ARG_OR_RETURN (sc, int, fd, number, args);
  FFI_ARG_OR_RETURN (sc, char *, mode, string, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  stream = fdopen (fd, mode);
  if (stream == NULL)
    FFI_RETURN_ERR (sc, gpg_error_from_syserror ());

  if (setvbuf (stream, NULL, _IONBF, 0) != 0)
    FFI_RETURN_ERR (sc, gpg_error_from_syserror ());

  kind = 0;
  if (strchr (mode, 'r'))
    kind |= port_input;
  if (strchr (mode, 'w'))
    kind |= port_output;

  FFI_RETURN_POINTER (sc, sc->vptr->mk_port_from_file (sc, stream, kind));
}

static pointer
do_close (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int fd;
  FFI_ARG_OR_RETURN (sc, int, fd, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  FFI_RETURN_ERR (sc, close (fd) == 0 ? 0 : gpg_error_from_syserror ());
}

static pointer
do_seek (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int fd;
  off_t offset;
  int whence;
  FFI_ARG_OR_RETURN (sc, int, fd, number, args);
  FFI_ARG_OR_RETURN (sc, off_t, offset, number, args);
  FFI_ARG_OR_RETURN (sc, int, whence, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  FFI_RETURN_ERR (sc, lseek (fd, offset, whence) == (off_t) -1
                  ? gpg_error_from_syserror () : 0);
}

static pointer
do_get_temp_path (scheme *sc, pointer args)
{
  FFI_PROLOG ();
#ifdef HAVE_W32_SYSTEM
  char buffer[MAX_PATH+1];
#endif
  FFI_ARGS_DONE_OR_RETURN (sc, args);

#ifdef HAVE_W32_SYSTEM
  if (GetTempPath (MAX_PATH+1, buffer) == 0)
    FFI_RETURN_STRING (sc, "/temp");
  else
    {
      size_t len = strlen (buffer);
      buffer[len-1] = 0;
    }
  FFI_RETURN_STRING (sc, buffer);
#else
  FFI_RETURN_STRING (sc, "/tmp");
#endif
}

static pointer
do_mkdtemp (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *template;
#ifdef PATH_MAX
  char buffer[PATH_MAX];
#else
  char buffer[1024];
#endif
  char *name;
  FFI_ARG_OR_RETURN (sc, char *, template, string, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  if (strlen (template) > sizeof buffer - 1)
    FFI_RETURN_ERR (sc, EINVAL);
  strncpy (buffer, template, sizeof buffer);

  name = gnupg_mkdtemp (buffer);
  if (name == NULL)
    FFI_RETURN_ERR (sc, gpg_error_from_syserror ());
  FFI_RETURN_STRING (sc, name);
}

static pointer
do_unlink (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *name;
  FFI_ARG_OR_RETURN (sc, char *, name, string, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  if (unlink (name) == -1)
    FFI_RETURN_ERR (sc, gpg_error_from_syserror ());
  FFI_RETURN (sc);
}

static gpg_error_t
unlink_recursively (const char *name)
{
  gpg_error_t err = 0;
  struct stat st;

  if (stat (name, &st) == -1)
    return gpg_error_from_syserror ();

  if (S_ISDIR (st.st_mode))
    {
      DIR *dir;
      struct dirent *dent;

      dir = opendir (name);
      if (dir == NULL)
        return gpg_error_from_syserror ();

      while ((dent = readdir (dir)))
        {
          char *child;

          if (strcmp (dent->d_name, ".") == 0
              || strcmp (dent->d_name, "..") == 0)
            continue;

          child = xtryasprintf ("%s/%s", name, dent->d_name);
          if (child == NULL)
            {
              err = gpg_error_from_syserror ();
              goto leave;
            }

          err = unlink_recursively (child);
          xfree (child);
          if (err == gpg_error_from_errno (ENOENT))
            err = 0;
          if (err)
            goto leave;
        }

    leave:
      closedir (dir);
      if (! err)
        rmdir (name);
      return err;
    }
  else
    if (unlink (name) == -1)
      return gpg_error_from_syserror ();
  return 0;
}

static pointer
do_unlink_recursively (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *name;
  FFI_ARG_OR_RETURN (sc, char *, name, string, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  err = unlink_recursively (name);
  FFI_RETURN (sc);
}

static pointer
do_rename (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *old;
  char *new;
  FFI_ARG_OR_RETURN (sc, char *, old, string, args);
  FFI_ARG_OR_RETURN (sc, char *, new, string, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  if (rename (old, new) == -1)
    FFI_RETURN_ERR (sc, gpg_error_from_syserror ());
  FFI_RETURN (sc);
}

static pointer
do_getcwd (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  pointer result;
  char *cwd;
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  cwd = gnupg_getcwd ();
  if (cwd == NULL)
    FFI_RETURN_ERR (sc, gpg_error_from_syserror ());
  result = sc->vptr->mk_string (sc, cwd);
  xfree (cwd);
  FFI_RETURN_POINTER (sc, result);
}

static pointer
do_mkdir (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *name;
  char *mode;
  FFI_ARG_OR_RETURN (sc, char *, name, string, args);
  FFI_ARG_OR_RETURN (sc, char *, mode, string, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  if (gnupg_mkdir (name, mode) == -1)
    FFI_RETURN_ERR (sc, gpg_error_from_syserror ());
  FFI_RETURN (sc);
}

static pointer
do_rmdir (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *name;
  FFI_ARG_OR_RETURN (sc, char *, name, string, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  if (rmdir (name) == -1)
    FFI_RETURN_ERR (sc, gpg_error_from_syserror ());
  FFI_RETURN (sc);
}

static pointer
do_get_isotime (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  gnupg_isotime_t timebuf;
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  gnupg_get_isotime (timebuf);
  FFI_RETURN_STRING (sc, timebuf);
}

static pointer
do_get_time (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  FFI_RETURN_INT (sc, gnupg_get_time ());
}

static pointer
do_getpid (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  FFI_RETURN_INT (sc, getpid ());
}

static pointer
do_srandom (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int seed;
  FFI_ARG_OR_RETURN (sc, int, seed, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  srand (seed);
  FFI_RETURN (sc);
}

static int
random_scaled (int scale)
{
  int v;
#ifdef HAVE_RAND
  v = rand ();
#else
  v = random ();
#endif

#ifndef RAND_MAX   /* for SunOS */
#define RAND_MAX 32767
#endif

  return ((int) (1 + (int) ((float) scale * v / (RAND_MAX + 1.0))) - 1);
}

static pointer
do_random (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int scale;
  FFI_ARG_OR_RETURN (sc, int, scale, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  FFI_RETURN_INT (sc, random_scaled (scale));
}

static pointer
do_make_random_string (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int size;
  pointer chunk;
  char *p;
  FFI_ARG_OR_RETURN (sc, int, size, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  if (size < 0)
    return ffi_sprintf (sc, "size must be positive");

  chunk = sc->vptr->mk_counted_string (sc, NULL, size);
  if (sc->no_memory)
    FFI_RETURN_ERR (sc, ENOMEM);

  for (p = sc->vptr->string_value (chunk); size; p++, size--)
    *p = (char) random_scaled (256);
  FFI_RETURN_POINTER (sc, chunk);
}



/* estream functions.  */

struct es_object_box
{
  estream_t stream;
  int closed;
};

static void
es_object_finalize (scheme *sc, void *data)
{
  struct es_object_box *box = data;
  (void) sc;

  if (! box->closed)
    es_fclose (box->stream);
  xfree (box);
}

static void
es_object_to_string (scheme *sc, char *out, size_t size, void *data)
{
  struct es_object_box *box = data;
  (void) sc;

  snprintf (out, size, "#estream %p", box->stream);
}

static struct foreign_object_vtable es_object_vtable =
  {
    es_object_finalize,
    es_object_to_string,
  };

#if 0
static pointer
es_wrap (scheme *sc, estream_t stream)
{
  struct es_object_box *box = xmalloc (sizeof *box);
  if (box == NULL)
    return sc->NIL;

  box->stream = stream;
  box->closed = 0;
  return sc->vptr->mk_foreign_object (sc, &es_object_vtable, box);
}
#endif

static struct es_object_box *
es_unwrap (scheme *sc, pointer object)
{
  (void) sc;

  if (! is_foreign_object (object))
    return NULL;

  if (sc->vptr->get_foreign_object_vtable (object) != &es_object_vtable)
    return NULL;

  return sc->vptr->get_foreign_object_data (object);
}

#define CONVERSION_estream(SC, X)	es_unwrap (SC, X)
#define IS_A_estream(SC, X)		es_unwrap (SC, X)

static pointer
do_es_fclose (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  struct es_object_box *box;
  FFI_ARG_OR_RETURN (sc, struct es_object_box *, box, estream, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  err = es_fclose (box->stream);
  if (! err)
    box->closed = 1;
  FFI_RETURN (sc);
}

static pointer
do_es_read (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  struct es_object_box *box;
  size_t bytes_to_read;

  pointer result;
  void *buffer;
  size_t bytes_read;

  FFI_ARG_OR_RETURN (sc, struct es_object_box *, box, estream, args);
  FFI_ARG_OR_RETURN (sc, size_t, bytes_to_read, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  buffer = xtrymalloc (bytes_to_read);
  if (buffer == NULL)
    FFI_RETURN_ERR (sc, ENOMEM);

  err = es_read (box->stream, buffer, bytes_to_read, &bytes_read);
  if (err)
    FFI_RETURN_ERR (sc, err);

  result = sc->vptr->mk_counted_string (sc, buffer, bytes_read);
  xfree (buffer);
  FFI_RETURN_POINTER (sc, result);
}

static pointer
do_es_feof (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  struct es_object_box *box;
  FFI_ARG_OR_RETURN (sc, struct es_object_box *, box, estream, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  FFI_RETURN_POINTER (sc, es_feof (box->stream) ? sc->T : sc->F);
}

static pointer
do_es_write (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  struct es_object_box *box;
  const char *buffer;
  size_t bytes_to_write, bytes_written;

  FFI_ARG_OR_RETURN (sc, struct es_object_box *, box, estream, args);
  /* XXX how to get the length of the string buffer?  scheme strings
     may contain \0.  */
  FFI_ARG_OR_RETURN (sc, const char *, buffer, string, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  bytes_to_write = strlen (buffer);
  while (bytes_to_write > 0)
    {
      err = es_write (box->stream, buffer, bytes_to_write, &bytes_written);
      if (err)
        break;
      bytes_to_write -= bytes_written;
      buffer += bytes_written;
    }

  FFI_RETURN (sc);
}


/* Process handling.  */

struct proc_object_box
{
  gpgrt_process_t proc;
};

static void
proc_object_finalize (scheme *sc, void *data)
{
  struct proc_object_box *box = data;
  (void) sc;

  if (!box->proc)
    gpgrt_process_release (box->proc);
  xfree (box);
}

static void
proc_object_to_string (scheme *sc, char *out, size_t size, void *data)
{
  struct proc_object_box *box = data;
  (void) sc;

  snprintf (out, size, "#proc %p", box->proc);
}

static struct foreign_object_vtable proc_object_vtable =
  {
    proc_object_finalize,
    proc_object_to_string,
  };

static pointer
proc_wrap (scheme *sc, gpgrt_process_t proc)
{
  struct proc_object_box *box = xmalloc (sizeof *box);
  if (box == NULL)
    return sc->NIL;

  box->proc = proc;
  return sc->vptr->mk_foreign_object (sc, &proc_object_vtable, box);
}

static struct proc_object_box *
proc_unwrap (scheme *sc, pointer object)
{
  (void) sc;

  if (! is_foreign_object (object))
    return NULL;

  if (sc->vptr->get_foreign_object_vtable (object) != &proc_object_vtable)
    return NULL;

  return sc->vptr->get_foreign_object_data (object);
}

#define CONVERSION_proc(SC, X)	proc_unwrap (SC, X)
#define IS_A_proc(SC, X)		proc_unwrap (SC, X)


#define SPAWN_IO_BUFSIZE 4096

#ifdef HAVE_W32_SYSTEM
struct rfp {
  HANDLE hd;
  char *buf;
  size_t len;
  off_t off;
};

static DWORD __attribute__((stdcall))
read_from_pipe (void *arg)
{
  struct rfp *rfp = arg;
  DWORD bytes_read;

  if (rfp->hd == INVALID_HANDLE_VALUE)
    goto errout;

  while (1)
    {
      if (!ReadFile (rfp->hd, rfp->buf + rfp->off, rfp->len - rfp->off,
                     &bytes_read, NULL))
        {
          DWORD ec = GetLastError ();

          if (ec == ERROR_BROKEN_PIPE)
            {
              CloseHandle (rfp->hd);
              rfp->hd = INVALID_HANDLE_VALUE;
              break;
            }

          goto errout;
        }

      if (bytes_read == 0)
        /* It may occur, when it writes WriteFile with zero-byte on
           the other end of the pipe. */
        continue;
      else
        {
          rfp->off += bytes_read;
          if (rfp->off == rfp->len)
            {
              rfp->len += SPAWN_IO_BUFSIZE;
              rfp->buf = xtryrealloc (rfp->buf, rfp->len);
              if (rfp->buf == NULL)
                goto errout;
            }
        }
    }

  return 0;

 errout:
  if (rfp->hd != INVALID_HANDLE_VALUE)
    {
      CloseHandle (rfp->hd);
      rfp->hd = INVALID_HANDLE_VALUE;
    }
  xfree (rfp->buf);
  rfp->buf = NULL;
  return 1;
}
#endif


static pointer
do_process_spawn_io (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  pointer arguments;
  char *a_input;
  char **argv;
  size_t len;
  unsigned int flags;
  gpgrt_process_t proc = NULL;
  estream_t infp;
#ifdef HAVE_W32_SYSTEM
  HANDLE out_hd, err_hd;
#else
  int out_fd, err_fd;
#endif
  char *out_string = NULL;
  char *err_string = NULL;
  size_t out_len = SPAWN_IO_BUFSIZE;
  size_t err_len = SPAWN_IO_BUFSIZE;
  off_t out_off = 0;
  off_t err_off = 0;
  int retcode = -1;
  pointer p0, p1, p2;

  FFI_ARG_OR_RETURN (sc, pointer, arguments, list, args);
  FFI_ARG_OR_RETURN (sc, char *, a_input, string, args);
  flags = (GPGRT_PROCESS_STDIN_PIPE
           | GPGRT_PROCESS_STDOUT_PIPE
           | GPGRT_PROCESS_STDERR_PIPE);
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  err = ffi_list2argv (sc, arguments, &argv, &len);
  if (err == gpg_error (GPG_ERR_INV_VALUE))
    return ffi_sprintf (sc, "%luth element of first argument is "
                        "neither string nor symbol",
                        (unsigned long) len);
  if (err)
    FFI_RETURN_ERR (sc, err);

  if (verbose > 1)
    {
      char **p;
      fprintf (stderr, "Executing:");
      for (p = argv; *p; p++)
        fprintf (stderr, " '%s'", *p);
      fprintf (stderr, "\n");
    }

  err = gpgrt_process_spawn (argv[0], (const char **) &argv[1],
                             flags, NULL, &proc);
  if (err)
    {
      xfree (argv);
      FFI_RETURN_ERR (sc, err);
    }

  err = gpgrt_process_get_streams (proc, 0, &infp, NULL, NULL);
  if (!err)
    {
      err = es_write (infp, a_input, strlen (a_input), NULL);
      es_fclose (infp);
    }
  if (err)
    {
      gpgrt_process_release (proc);
      xfree (argv);
      FFI_RETURN_ERR (sc, err);
    }

#ifdef HAVE_W32_SYSTEM
  err = gpgrt_process_ctl (proc, GPGRT_PROCESS_GET_HANDLES,
                           NULL, &out_hd, &err_hd);
#else
  err = gpgrt_process_get_fds (proc, 0, NULL, &out_fd, &err_fd);
#endif
  if (err)
    {
      gpgrt_process_release (proc);
      xfree (argv);
      FFI_RETURN_ERR (sc, err);
    }

  out_string = xtrymalloc (out_len);
  if (out_string == NULL)
    goto errout;

  err_string = xtrymalloc (err_len);
  if (err_string == NULL)
    goto errout;

#ifdef HAVE_W32_SYSTEM
  {
    HANDLE h_thread_rfp_err;
    struct rfp rfp_out;
    struct rfp rfp_err;
    DWORD thread_exit_code;

    rfp_err.hd = err_hd;
    rfp_err.buf = err_string;
    rfp_err.len = err_len;
    rfp_err.off = 0;
    err_hd = INVALID_HANDLE_VALUE;
    err_string = NULL;

    h_thread_rfp_err = CreateThread (NULL, 0, read_from_pipe, (void *)&rfp_err,
                                     0, NULL);
    if (h_thread_rfp_err == NULL)
      {
        xfree (rfp_err.buf);
        CloseHandle (rfp_err.hd);
        goto errout;
      }

    rfp_out.hd = out_hd;
    rfp_out.buf = out_string;
    rfp_out.len = out_len;
    rfp_out.off = 0;
    out_hd = INVALID_HANDLE_VALUE;
    out_string = NULL;

    if (read_from_pipe (&rfp_out))
      {
        CloseHandle (h_thread_rfp_err);
        xfree (rfp_err.buf);
        goto errout;
      }

    out_string = rfp_out.buf;
    out_off = rfp_out.off;

    WaitForSingleObject (h_thread_rfp_err, INFINITE);
    GetExitCodeThread (h_thread_rfp_err, &thread_exit_code);
    CloseHandle (h_thread_rfp_err);
    if (thread_exit_code)
      goto errout;

    err_string = rfp_err.buf;
    err_off = rfp_err.off;
  }
#else
  {
    fd_set read_fdset;
    ssize_t bytes_read;

    while (1)
      {
        int nfd;
        int ret;

        FD_ZERO (&read_fdset);

        if (out_fd >= 0)
          FD_SET (out_fd, &read_fdset);

        if (err_fd >= 0)
          FD_SET (err_fd, &read_fdset);

        if (out_fd > err_fd)
          nfd = out_fd;
        else
          nfd = err_fd;

        if (nfd == -1)
          break;

        ret = select (nfd+1, &read_fdset, NULL, NULL, NULL);
        if (ret < 0)
          break;

        if (out_fd >= 0 && FD_ISSET (out_fd, &read_fdset))
          {
            bytes_read = read (out_fd, out_string + out_off,
                               out_len - out_off);
            if (bytes_read == 0)
              {
                close (out_fd);
                out_fd = -1;
              }
            else if (bytes_read < 0)
              goto errout;
            else
              {
                out_off += bytes_read;
                if (out_off == out_len)
                  {
                    out_len += SPAWN_IO_BUFSIZE;
                    out_string = xtryrealloc (out_string, out_len);
                    if (out_string == NULL)
                      goto errout;
                  }
              }
          }

        if (err_fd >= 0 && FD_ISSET (err_fd, &read_fdset))
          {
            bytes_read = read (err_fd, err_string + err_off,
                               err_len - err_off);
            if (bytes_read == 0)
              {
                close (err_fd);
                err_fd = -1;
              }
            else if (bytes_read < 0)
              goto errout;
            else
              {
                err_off += bytes_read;
                if (err_off == err_len)
                  {
                    err_len += SPAWN_IO_BUFSIZE;
                    err_string = xtryrealloc (err_string, err_len);
                    if (err_string == NULL)
                      goto errout;
                  }
              }
          }
      }
  }
#endif

  err = gpgrt_process_wait (proc, 1);
  if (!err)
    err = gpgrt_process_ctl (proc, GPGRT_PROCESS_GET_EXIT_ID, &retcode);

  gpgrt_process_release (proc);
  xfree (argv);

  p0 = sc->vptr->mk_integer (sc, (unsigned long)retcode);
  p1 = sc->vptr->mk_counted_string (sc, out_string, out_off);
  p2 = sc->vptr->mk_counted_string (sc, err_string, err_off);

  xfree (out_string);
  xfree (err_string);

  FFI_RETURN_POINTER (sc, _cons (sc, p0,
                                 _cons (sc, p1,
                                        _cons (sc, p2, sc->NIL, 1), 1), 1));
 errout:
  xfree (out_string);
  xfree (err_string);
#ifdef HAVE_W32_SYSTEM
  if (out_hd != INVALID_HANDLE_VALUE)
    CloseHandle (out_hd);
  if (err_hd != INVALID_HANDLE_VALUE)
    CloseHandle (err_hd);
#else
  if (out_fd >= 0)
    close (out_fd);
  if (err_fd >= 0)
    close (err_fd);
#endif
  gpgrt_process_release (proc);
  xfree (argv);
  FFI_RETURN_ERR (sc, err);
}

static pointer
do_process_spawn_fd (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  pointer arguments;
  char **argv;
  size_t len;
  int std_fds[3];
  gpgrt_process_t proc = NULL;
  gpgrt_spawn_actions_t act = NULL;

  FFI_ARG_OR_RETURN (sc, pointer, arguments, list, args);
  FFI_ARG_OR_RETURN (sc, int, std_fds[0], number, args);
  FFI_ARG_OR_RETURN (sc, int, std_fds[1], number, args);
  FFI_ARG_OR_RETURN (sc, int, std_fds[2], number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  err = ffi_list2argv (sc, arguments, &argv, &len);
  if (err == gpg_error (GPG_ERR_INV_VALUE))
    return ffi_sprintf (sc, "%luth element of first argument is "
                        "neither string nor symbol",
                        (unsigned long) len);
  if (err)
    FFI_RETURN_ERR (sc, err);

  if (verbose > 1)
    {
      char **p;
      fprintf (stderr, "Executing:");
      for (p = argv; *p; p++)
        fprintf (stderr, " '%s'", *p);
      fprintf (stderr, " (%d %d %d)\n", std_fds[0], std_fds[1], std_fds[2]);
    }

  err = gpgrt_spawn_actions_new (&act);
  if (err)
    {
      FFI_RETURN_ERR (sc, err);
    }
#ifdef HAVE_W32_SYSTEM
  {
    HANDLE std_in, std_out, std_err;

    if (std_fds[0] == -1)
      std_in = INVALID_HANDLE_VALUE;
    else
      std_in = (HANDLE)_get_osfhandle (std_fds[0]);
    if (std_fds[1] == -1)
      std_out = INVALID_HANDLE_VALUE;
    else
      std_out = (HANDLE)_get_osfhandle (std_fds[1]);
    if (std_fds[2] == -1)
      std_err = INVALID_HANDLE_VALUE;
    else
      std_err = (HANDLE)_get_osfhandle (std_fds[2]);

    gpgrt_spawn_actions_set_redirect (act, std_in, std_out, std_err);
  }
#else
  gpgrt_spawn_actions_set_redirect (act, std_fds[0], std_fds[1], std_fds[2]);
#endif
  err = gpgrt_process_spawn (argv[0], (const char **)&argv[1], 0, act, &proc);
  gpgrt_spawn_actions_release (act);
  xfree (argv);
  if (err)
    FFI_RETURN_ERR (sc, err);
  FFI_RETURN_POINTER (sc, proc_wrap (sc, proc));
}

static pointer
do_process_wait (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  struct proc_object_box *box;
  int hang;
  int retcode = -1;

  FFI_ARG_OR_RETURN (sc, struct proc_object_box *, box, proc, args);
  FFI_ARG_OR_RETURN (sc, int, hang, bool, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  if (!box->proc)
    {
      if (verbose)
        fprintf (stderr, "caught already (%p)\n", box);
    }
  else
    {
      err = gpgrt_process_wait (box->proc, hang);
      if (!err)
        {
          err = gpgrt_process_ctl (box->proc, GPGRT_PROCESS_GET_EXIT_ID, &retcode);
          gpgrt_process_release (box->proc);
          box->proc = NULL;
        }
      if (err == GPG_ERR_TIMEOUT)
        err = 0;
    }
  FFI_RETURN_INT (sc, retcode);
}

static pointer
do_pipe (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int filedes[2];
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  err = gnupg_create_pipe (filedes, GNUPG_PIPE_BOTH);
#define IMC(A, B)                                                       \
  _cons (sc, sc->vptr->mk_integer (sc, (unsigned long) (A)), (B), 1)
  FFI_RETURN_POINTER (sc, IMC (filedes[0],
                              IMC (filedes[1], sc->NIL)));
#undef IMC
}

static pointer
do_inbound_pipe (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int filedes[2];
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  err = gnupg_create_pipe (filedes, GNUPG_PIPE_INBOUND);
#define IMC(A, B)                                                       \
  _cons (sc, sc->vptr->mk_integer (sc, (unsigned long) (A)), (B), 1)
  FFI_RETURN_POINTER (sc, IMC (filedes[0],
                              IMC (filedes[1], sc->NIL)));
#undef IMC
}

static pointer
do_outbound_pipe (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int filedes[2];
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  err = gnupg_create_pipe (filedes, GNUPG_PIPE_OUTBOUND);
#define IMC(A, B)                                                       \
  _cons (sc, sc->vptr->mk_integer (sc, (unsigned long) (A)), (B), 1)
  FFI_RETURN_POINTER (sc, IMC (filedes[0],
                              IMC (filedes[1], sc->NIL)));
#undef IMC
}



/* Test helper functions.  */
static pointer
do_file_equal (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  pointer result = sc->F;
  char *a_name, *b_name;
  int binary;
  const char *mode;
  FILE *a_stream = NULL, *b_stream = NULL;
  struct stat a_stat, b_stat;
#define BUFFER_SIZE	1024
  char a_buf[BUFFER_SIZE], b_buf[BUFFER_SIZE];
#undef BUFFER_SIZE
  size_t chunk;

  FFI_ARG_OR_RETURN (sc, char *, a_name, string, args);
  FFI_ARG_OR_RETURN (sc, char *, b_name, string, args);
  FFI_ARG_OR_RETURN (sc, int, binary, bool, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  mode = binary ? "rb" : "r";
  a_stream = fopen (a_name, mode);
  if (a_stream == NULL)
    goto errout;

  b_stream = fopen (b_name, mode);
  if (b_stream == NULL)
    goto errout;

  if (fstat (fileno (a_stream), &a_stat) < 0)
    goto errout;

  if (fstat (fileno (b_stream), &b_stat) < 0)
    goto errout;

  if (binary && a_stat.st_size != b_stat.st_size)
    {
      if (verbose)
        fprintf (stderr, "Files %s and %s differ in size %lu != %lu\n",
                 a_name, b_name, (unsigned long) a_stat.st_size,
                 (unsigned long) b_stat.st_size);

      goto out;
    }

  while (! feof (a_stream))
    {
      chunk = sizeof a_buf;

      chunk = fread (a_buf, 1, chunk, a_stream);
      if (chunk == 0 && ferror (a_stream))
        goto errout;	/* some error */

      if (fread (b_buf, 1, chunk, b_stream) < chunk)
        {
          if (feof (b_stream))
            goto out;	/* short read */
          goto errout;	/* some error */
        }

      if (chunk > 0 && memcmp (a_buf, b_buf, chunk) != 0)
        goto out;
    }

  fread (b_buf, 1, 1, b_stream);
  if (! feof (b_stream))
    goto out;	/* b is longer */

  /* They match.  */
  result = sc->T;

 out:
  if (a_stream)
    fclose (a_stream);
  if (b_stream)
    fclose (b_stream);
  FFI_RETURN_POINTER (sc, result);
 errout:
  err = gpg_error_from_syserror ();
  goto out;
}

static pointer
do_splice (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int source;
  char buffer[1024];
  ssize_t bytes_read;
  pointer sinks, sink;
  FFI_ARG_OR_RETURN (sc, int, source, number, args);
  sinks = args;
  if (sinks == sc->NIL)
    return ffi_sprintf (sc, "need at least one sink");
  for (sink = sinks; sink != sc->NIL; sink = pair_cdr (sink), ffi_arg_index++)
    if (! sc->vptr->is_number (pair_car (sink)))
      return ffi_sprintf (sc, "%d%s argument is not a number",
                          ffi_arg_index, ordinal_suffix (ffi_arg_index));

  while (1)
    {
      bytes_read = read (source, buffer, sizeof buffer);
      if (bytes_read == 0)
        break;
      if (bytes_read < 0)
        FFI_RETURN_ERR (sc, gpg_error_from_syserror ());

      for (sink = sinks; sink != sc->NIL; sink = pair_cdr (sink))
        {
          int fd = sc->vptr->ivalue (pair_car (sink));
          char *p = buffer;
          ssize_t left = bytes_read;

          while (left)
            {
              ssize_t written = write (fd, p, left);
              if (written < 0)
                FFI_RETURN_ERR (sc, gpg_error_from_syserror ());
              assert (written <= left);
              left -= written;
              p += written;
            }
        }
    }
  FFI_RETURN (sc);
}

static pointer
do_string_index (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *haystack;
  char needle;
  ssize_t offset = 0;
  char *position;
  FFI_ARG_OR_RETURN (sc, char *, haystack, string, args);
  FFI_ARG_OR_RETURN (sc, char, needle, character, args);
  if (args != sc->NIL)
    {
      FFI_ARG_OR_RETURN (sc, ssize_t, offset, number, args);
      if (offset < 0)
        return ffi_sprintf (sc, "offset must be positive");
      if (offset > strlen (haystack))
        return ffi_sprintf (sc, "offset exceeds haystack");
    }
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  position = strchr (haystack+offset, needle);
  if (position)
    FFI_RETURN_INT (sc, position - haystack);
  else
    FFI_RETURN_POINTER (sc, sc->F);
}

static pointer
do_string_rindex (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *haystack;
  char needle;
  ssize_t offset = 0;
  char *position;
  FFI_ARG_OR_RETURN (sc, char *, haystack, string, args);
  FFI_ARG_OR_RETURN (sc, char, needle, character, args);
  if (args != sc->NIL)
    {
      FFI_ARG_OR_RETURN (sc, ssize_t, offset, number, args);
      if (offset < 0)
        return ffi_sprintf (sc, "offset must be positive");
      if (offset > strlen (haystack))
        return ffi_sprintf (sc, "offset exceeds haystack");
    }
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  position = strrchr (haystack+offset, needle);
  if (position)
    FFI_RETURN_INT (sc, position - haystack);
  else
    FFI_RETURN_POINTER (sc, sc->F);
}

static pointer
do_string_contains (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  char *haystack;
  char *needle;
  FFI_ARG_OR_RETURN (sc, char *, haystack, string, args);
  FFI_ARG_OR_RETURN (sc, char *, needle, string, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  FFI_RETURN_POINTER (sc, strstr (haystack, needle) ? sc->T : sc->F);
}



static pointer
do_get_verbose (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  FFI_ARGS_DONE_OR_RETURN (sc, args);
  FFI_RETURN_INT (sc, verbose);
}

static pointer
do_set_verbose (scheme *sc, pointer args)
{
  FFI_PROLOG ();
  int new_verbosity, old;
  FFI_ARG_OR_RETURN (sc, int, new_verbosity, number, args);
  FFI_ARGS_DONE_OR_RETURN (sc, args);

  old = verbose;
  verbose = new_verbosity;

  FFI_RETURN_INT (sc, old);
}


gpg_error_t
ffi_list2argv (scheme *sc, pointer list, char ***argv, size_t *len)
{
  int i;

  *len = sc->vptr->list_length (sc, list);
  *argv = xtrycalloc (*len + 1, sizeof **argv);
  if (*argv == NULL)
    return gpg_error_from_syserror ();

  for (i = 0; sc->vptr->is_pair (list); list = sc->vptr->pair_cdr (list))
    {
      if (sc->vptr->is_string (sc->vptr->pair_car (list)))
        (*argv)[i++] = sc->vptr->string_value (sc->vptr->pair_car (list));
      else if (sc->vptr->is_symbol (sc->vptr->pair_car (list)))
        (*argv)[i++] = sc->vptr->symname (sc->vptr->pair_car (list));
      else
        {
          xfree (*argv);
          *argv = NULL;
          *len = i;
          return gpg_error (GPG_ERR_INV_VALUE);
        }
    }
  (*argv)[i] = NULL;
  return 0;
}

gpg_error_t
ffi_list2intv (scheme *sc, pointer list, int **intv, size_t *len)
{
  int i;

  *len = sc->vptr->list_length (sc, list);
  *intv = xtrycalloc (*len, sizeof **intv);
  if (*intv == NULL)
    return gpg_error_from_syserror ();

  for (i = 0; sc->vptr->is_pair (list); list = sc->vptr->pair_cdr (list))
    {
      if (sc->vptr->is_number (sc->vptr->pair_car (list)))
        (*intv)[i++] = sc->vptr->ivalue (sc->vptr->pair_car (list));
      else
        {
          xfree (*intv);
          *intv = NULL;
          *len = i;
          return gpg_error (GPG_ERR_INV_VALUE);
        }
    }

  return 0;
}


char *
ffi_schemify_name (const char *s, int macro)
{
  /* Fixme: We should use xtrystrdup and return NULL.  However, this
   * requires a lot more changes.  Simply returning S as done
   * originally is not an option.  */
  char *n = xstrdup (s), *p;
  /* if (n == NULL) */
  /*   return s; */

  for (p = n; *p; p++)
    {
      *p = (char) tolower (*p);
       /* We convert _ to - in identifiers.  We allow, however, for
	  function names to start with a leading _.  The functions in
	  this namespace are not yet finalized and might change or
	  vanish without warning.  Use them with care.	*/
      if (! macro
	  && p != n
	  && *p == '_')
	*p = '-';
    }
  return n;
}

pointer
ffi_sprintf (scheme *sc, const char *format, ...)
{
  pointer result;
  va_list listp;
  char *expression;
  int size, written;

  va_start (listp, format);
  size = vsnprintf (NULL, 0, format, listp);
  va_end (listp);

  expression = xtrymalloc (size + 1);
  if (expression == NULL)
    return NULL;

  va_start (listp, format);
  written = vsnprintf (expression, size + 1, format, listp);
  va_end (listp);

  assert (size == written);

  result = sc->vptr->mk_string (sc, expression);
  xfree (expression);
  return result;
}

void
ffi_scheme_eval (scheme *sc, const char *format, ...)
{
  va_list listp;
  char *expression;
  int size, written;

  va_start (listp, format);
  size = vsnprintf (NULL, 0, format, listp);
  va_end (listp);

  expression = xtrymalloc (size + 1);
  if (expression == NULL)
    return;

  va_start (listp, format);
  written = vsnprintf (expression, size + 1, format, listp);
  va_end (listp);

  assert (size == written);

  sc->vptr->load_string (sc, expression);
  xfree (expression);
}

gpg_error_t
ffi_init (scheme *sc, const char *argv0, const char *scriptname,
          int argc, const char **argv)
{
  int i;
  pointer args = sc->NIL;

  /* bitwise arithmetic */
  ffi_define_function (sc, logand);
  ffi_define_function (sc, logior);
  ffi_define_function (sc, logxor);
  ffi_define_function (sc, lognot);

  /* libc.  */
  ffi_define_constant (sc, O_RDONLY);
  ffi_define_constant (sc, O_WRONLY);
  ffi_define_constant (sc, O_RDWR);
  ffi_define_constant (sc, O_CREAT);
  ffi_define_constant (sc, O_APPEND);
#ifndef O_BINARY
# define O_BINARY	0
#endif
#ifndef O_TEXT
# define O_TEXT		0
#endif
  ffi_define_constant (sc, O_BINARY);
  ffi_define_constant (sc, O_TEXT);
  ffi_define_constant (sc, STDIN_FILENO);
  ffi_define_constant (sc, STDOUT_FILENO);
  ffi_define_constant (sc, STDERR_FILENO);
  ffi_define_constant (sc, SEEK_SET);
  ffi_define_constant (sc, SEEK_CUR);
  ffi_define_constant (sc, SEEK_END);

  ffi_define_function (sc, sleep);
  ffi_define_function (sc, usleep);
  ffi_define_function (sc, chdir);
  ffi_define_function (sc, strerror);
  ffi_define_function (sc, getenv);
  ffi_define_function (sc, setenv);
  ffi_define_function_name (sc, "_exit", exit);
  /* AIX defines open to open64 which breaks the macro expansion to
     'do_open' if it is not undefined.  */
#ifdef open
# undef open
#endif
  ffi_define_function (sc, open);
  ffi_define_function (sc, fdopen);
  ffi_define_function (sc, close);
  ffi_define_function (sc, seek);
  ffi_define_function (sc, get_temp_path);
  ffi_define_function_name (sc, "_mkdtemp", mkdtemp);
  ffi_define_function (sc, unlink);
  ffi_define_function (sc, unlink_recursively);
  ffi_define_function (sc, rename);
  ffi_define_function (sc, getcwd);
  ffi_define_function (sc, mkdir);
  ffi_define_function (sc, rmdir);
  ffi_define_function (sc, get_isotime);
  ffi_define_function (sc, get_time);
  ffi_define_function (sc, getpid);

  /* Random numbers.  */
  ffi_define_function (sc, srandom);
  ffi_define_function (sc, random);
  ffi_define_function (sc, make_random_string);

  /* Process management.  */
  ffi_define_function (sc, pipe);
  ffi_define_function (sc, inbound_pipe);
  ffi_define_function (sc, outbound_pipe);
  ffi_define_function (sc, process_spawn_io);
  ffi_define_function (sc, process_spawn_fd);
  ffi_define_function (sc, process_wait);

  /* estream functions.  */
  ffi_define_function_name (sc, "es-fclose", es_fclose);
  ffi_define_function_name (sc, "es-read", es_read);
  ffi_define_function_name (sc, "es-feof", es_feof);
  ffi_define_function_name (sc, "es-write", es_write);

  /* Test helper functions.  */
  ffi_define_function (sc, file_equal);
  ffi_define_function (sc, splice);
  ffi_define_function (sc, string_index);
  ffi_define_function (sc, string_rindex);
  ffi_define_function_name (sc, "string-contains?", string_contains);

  /* User interface.  */
  ffi_define_function (sc, flush_stdio);
  ffi_define_function (sc, prompt);

  /* Configuration.  */
  ffi_define_function_name (sc, "*verbose*", get_verbose);
  ffi_define_function_name (sc, "*set-verbose!*", set_verbose);

  ffi_define (sc, "*argv0*", sc->vptr->mk_string (sc, argv0));
  ffi_define (sc, "*scriptname*", sc->vptr->mk_string (sc, scriptname));
  for (i = argc - 1; i >= 0; i--)
    {
      pointer value = sc->vptr->mk_string (sc, argv[i]);
      args = (sc->vptr->cons) (sc, value, args);
    }
  ffi_define (sc, "*args*", args);

#if _WIN32
  ffi_define (sc, "*pathsep*", sc->vptr->mk_character (sc, ';'));
#else
  ffi_define (sc, "*pathsep*", sc->vptr->mk_character (sc, ':'));
#endif

  ffi_define (sc, "*win32*",
#if _WIN32
              sc->T
#else
              sc->F
#endif
              );

  ffi_define (sc, "*maintainer-mode*",
#if MAINTAINER_MODE
              sc->T
#else
              sc->F
#endif
              );

  ffi_define (sc, "*run-all-tests*",
#if RUN_ALL_TESTS
              sc->T
#else
              sc->F
#endif
              );


  ffi_define (sc, "*stdin*",
              sc->vptr->mk_port_from_file (sc, stdin, port_input));
  ffi_define (sc, "*stdout*",
              sc->vptr->mk_port_from_file (sc, stdout, port_output));
  ffi_define (sc, "*stderr*",
              sc->vptr->mk_port_from_file (sc, stderr, port_output));

  return 0;
}
