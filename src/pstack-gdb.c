/*
 * pstack-gdb.c
 *
 * Simple stack trace print wrapper around gdb
 *
 * Copyright (C) 2004 Oracle.  All rights reserved.
 *
 * Author: Manish Singh <manish.singh@oracle.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have recieved a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>


#define REAP_DELAY       1000
#define REAP_RETRY_COUNT 5

#define BUF_SIZE         1024


typedef enum
{
  STATE_START,
  STATE_ATTACH,
  STATE_CHECK_THREADS,
  STATE_BACKTRACE,
  STATE_PRINT_BACKTRACE,
  STATE_DETACH,
  STATE_DONE
} State;


struct _App
{
  GMainLoop  *loop;

  GSList     *pids;
  GSList     *threads;

  GIOChannel *in, *out;

  gint        gdb_pid;
  gint        reap_try;

  State       state;
};

typedef struct _App App;


static void        print_version          (void);
static void        usage                  (gchar         *prgname);

static GIOChannel *new_io_channel         (gint           fd);

static GSList     *get_pid_list           (gint           argc,
                                           gchar        **argv);

static void        redirect_stderr        (gpointer       user_data);

static gboolean    start_gdb              (App           *app);

static gboolean    reap_gdb               (GIOChannel    *channel,
                                           GIOCondition   condition,
				           App           *app);
static gboolean    waitpid_timeout        (App           *app);

static gchar      **read_lines            (GIOChannel    *channel);
static void         print_lines           (gchar        **lines);

static gboolean     attach_ok             (gchar        **lines,
					   gchar        **errbuf,
					   App           *app);

static GSList      *get_thread_ids        (gchar        **lines);

static void         send_command          (gchar         *command,
					   App           *app);
static void         send_thread_backtrace (App           *app);

static gboolean     process_gdb_output    (GIOChannel    *channel,
                                           GIOCondition   condition,
					   App           *app);


static void
print_version (void)
{
  g_print ("pstack-gdb version " PSTACK_VERSION "\n");
  g_print ("Written by Manish Singh.\n\n");
  g_print ("Copyright (C) 2004 Oracle.\n");
}

static void
usage (gchar *prgname)
{
  g_print ("Usage: %s [OPTION] pid [...]\n\n", prgname);
  g_print ("Specify one or more pids to print a stack trace for each.\n\n");
  g_print ("Options:\n");
  g_print ("  -V, --version  print version information and exit\n");
  g_print ("      --help     display this help and exit\n");
}

static GIOChannel *
new_io_channel (gint fd)
{
  GIOChannel *channel;

  channel = g_io_channel_unix_new (fd);

  g_io_channel_set_encoding (channel, NULL, NULL);
  g_io_channel_set_buffered (channel, FALSE);
  g_io_channel_set_close_on_unref (channel, TRUE);

  return channel;
}

static guint
add_watch (GIOChannel   *channel,
           GIOCondition  condition,
	   GIOFunc       func,
	   gpointer      user_data)
{
  guint ret;

  ret = g_io_add_watch (channel, condition, func, user_data);
  g_io_channel_unref (channel);

  return ret;
}

static GSList *
get_pid_list (gint    argc,
              gchar **argv)
{
  GSList *pids = NULL;
  gint    i;

  if (argc < 2)
    {
      g_printerr ("No valid pids given\n\n");
      return NULL;
    }

  for (i = 1; i < argc; i++)
    {
      gint   pid;
      gchar *endptr;

      pid = strtoul (argv[i], &endptr, 10);

      if (*endptr != '\0' || pid <= 0 || pid > G_MAXINT || errno != 0)
        {
          g_printerr ("Invalid pid: %s\n\n", argv[i]);
          g_slist_free (pids);
          return NULL;
        }

      pids = g_slist_prepend (pids, GINT_TO_POINTER (pid));
    }

  return g_slist_reverse (pids);
}

static void
redirect_stderr (gpointer user_data)
{
  gint ret;

retry:
  ret = dup2 (1, 2);
  if (ret < 0 && errno == EINTR)
    goto retry;
}

static gboolean
start_gdb (App *app)
{
  gchar       *argv[] = { "gdb", "--nx", NULL };
  GSpawnFlags  flags = G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH;
  gint         in, out;
  GError      *error = NULL;

  if (!g_spawn_async_with_pipes (NULL, argv, NULL, flags,
                                 redirect_stderr, NULL, &app->gdb_pid,
                                 &in, &out, NULL, &error))
    {
      g_printerr ("Unable to start gdb: %s\n", error->message);
      g_error_free (error);

      return FALSE;
    }

  app->in  = new_io_channel (in);
  app->out = new_io_channel (out);

  return TRUE;
}

static gboolean
reap_gdb (GIOChannel   *channel,
          GIOCondition  condition,
	  App          *app)
{
  if (app->state != STATE_DONE)
    g_printerr ("gdb unexpectedly died!\n");

  app->reap_try = REAP_RETRY_COUNT;

  if (waitpid_timeout (app))
    g_timeout_add (REAP_DELAY, (GSourceFunc) waitpid_timeout, app);

  return FALSE;
}

static gboolean
waitpid_timeout (App *app)
{
  gint pid;
  gint status;

  app->reap_try--;

  pid = waitpid (app->gdb_pid, &status, WNOHANG);

  if (pid == -1)
    {
      /* ERROR MESSAGE */
    }
  else if (pid == 0)
    {
      if (app->reap_try == 0)
	kill (app->gdb_pid, SIGTERM);
      else if (app->reap_try < 0)
	kill (app->gdb_pid, SIGKILL);

      return TRUE;
    }

  g_main_loop_quit (app->loop);

  return FALSE;
}

static char **
read_lines (GIOChannel *channel)
{
  GString    *buf;
  gchar       buffer[BUF_SIZE];
  GIOStatus   status;
  gsize       bytes;
  GError     *error = NULL;
  gboolean    done = FALSE;
  gchar     **lines = NULL, **iter;

  buf = g_string_sized_new (BUF_SIZE);

  while (!done)
    {
      bytes = 0;
      status = g_io_channel_read_chars (channel, buffer, BUF_SIZE, &bytes,
					&error);

      buffer[bytes] = '\0';
      g_string_append (buf, buffer);

      switch (status)
	{
	case G_IO_STATUS_NORMAL:
	  break;
	
	case G_IO_STATUS_AGAIN:
	case G_IO_STATUS_EOF:
	  done = TRUE;
	  break;

	default:
	  if (error)
	    {
	      g_printerr ("gdb read error: %s", error->message);
	      g_error_free (error);
	    }
	  else
	    {
	      g_printerr ("gdb read error");
	    }

	  done = TRUE;
	  break;
	}

      if (g_str_has_suffix (buf->str, "(gdb) "))
	{
	  lines = g_strsplit (buf->str, "\n", -1);

	  for (iter = lines; *iter; iter++);

          iter--;
	  g_free (*iter);
	  *iter = NULL;

	  done = TRUE;
	}
    }

  g_string_free (buf, TRUE);
  return lines;
}

static void
print_lines (gchar **lines)
{
  gchar **iter;

  for (iter = lines; *iter; iter++)
    g_print ("%s\n", *iter);
}

static gboolean
attach_ok (gchar **lines,
           gchar **errbuf,
           App    *app)
{
  gchar **iter;

  for (iter = lines; *iter; iter++)
    {
      if (strncmp (*iter, "ptrace:", strlen ("ptrace:"))  == 0)
	{
	  *errbuf = g_strdup (*iter);
	  return FALSE;
	}
    }

  return TRUE;
}

static GSList *
get_thread_ids (gchar **lines)
{
  gchar   **iter;
  GSList   *threads = NULL;
  gchar    *s;
  gboolean  found;

  for (iter = lines; *iter; iter++)
    {
      if (!strstr (*iter, "Thread"))
	continue;

      found = FALSE;

      for (s = *iter; *s; s++)
	{
	  if (g_ascii_isdigit (*s))
	    {
	      found = TRUE;
	    }
	  else if (found)
	    {
	      *s = '\0';
	      break;
	    }
	}

      if (found)
	threads = g_slist_prepend (threads, g_strdup (*iter));
    }

  return threads;
}

static void
send_command (gchar *command,
	      App   *app)
{
  gsize  bytes;
  gchar *buf;

  buf = g_strconcat (command, "\n", NULL);
  g_io_channel_write_chars (app->in, buf, strlen (buf), &bytes, NULL);
  g_free (buf);
}

static void
send_thread_backtrace (App *app)
{
  gchar *buf, *str;

  str = app->threads->data;

  app->threads = g_slist_remove (app->threads, str);

  buf = g_strconcat ("pstack_thread ", str, NULL);
  g_free (str);

  send_command (buf, app);
  g_free (buf);
}

static gboolean
process_gdb_output (GIOChannel   *channel,
                    GIOCondition  condition,
		    App          *app)
{
  gchar **lines;
  gint    pid = -1;
  gchar  *buf;

  if (condition & G_IO_ERR || condition & G_IO_HUP)
    return FALSE;

  if (app->pids)
    pid = GPOINTER_TO_INT (app->pids->data);
  else
    app->state = STATE_DONE;

  lines = read_lines (channel);

  if (lines == NULL)
    return FALSE;

  switch (app->state)
    {
    case STATE_START:
      buf = "define pstack_thread\nthread $arg0\nbacktrace\nend\n";
      send_command (buf, app);

      app->state = STATE_ATTACH;

      break;

    case STATE_ATTACH:
      buf = g_strdup_printf ("attach %d", pid);
      send_command (buf, app);
      g_free (buf);

      app->state = STATE_CHECK_THREADS;

      break;

    case STATE_CHECK_THREADS:
      if (attach_ok (lines, &buf, app))
	{
	  send_command ("info threads", app);
	  app->state = STATE_BACKTRACE;
	}
      else
	{
	  g_printerr ("Skipping pid %d: %s\n", pid, buf);
	  g_free (buf);

	  send_command ("p 0", app);

	  app->pids = app->pids->next;
	  app->state = STATE_ATTACH;
	}

      break;

    case STATE_BACKTRACE:
      g_print ("Backtrace for pid %d\n", pid);

      if (*lines != NULL)
	{
	  app->threads = get_thread_ids (lines);

	  if (app->threads)
	    {
	      send_thread_backtrace (app);
	      app->state = STATE_PRINT_BACKTRACE;
	    }
	  else
	    app->state = STATE_DETACH;
	}
      else
	{
	  send_command ("backtrace", app);
	  app->state = STATE_PRINT_BACKTRACE;
	}

      break;

    case STATE_PRINT_BACKTRACE:
      print_lines (lines);

      if (app->threads)
	{
	  send_thread_backtrace (app);
	  app->state = STATE_PRINT_BACKTRACE;

	  break;
	}

      /* FALLTHROUGH */

    case STATE_DETACH:
      send_command ("detach", app);

      g_slist_foreach (app->threads, (GFunc) g_free, NULL);
      g_slist_free (app->threads);
      app->threads = NULL;

      app->pids = app->pids->next;
      app->state = STATE_ATTACH;

      break;
     
    case STATE_DONE:
      send_command ("quit", app);
      break;
    }

  g_strfreev (lines);

  return TRUE;
}

int
main (int    argc,
      char **argv)
{
  App    *app;
  GSList *pids;
  gint    i;

  for (i = 1; i < argc; i++)
    {
      if ((strcmp (argv[i], "--version") == 0) ||
	  (strcmp (argv[i], "-V") == 0))
	{
	  print_version ();
	  exit (0);
	}
      else if (strcmp (argv[i], "--help") == 0)
	{
	  usage (argv[0]);
	  exit (0);
	}
      else if (*argv[i] == '-')
	{
	  usage (argv[0]);
	  exit (1);
	}
    }

  pids = get_pid_list (argc, argv);

  if (!pids)
    {
      usage (argv[0]);
      exit (1);
    }

  app = g_new0 (App, 1);

  if (!start_gdb (app))
    {
      g_free (app);
      exit (1);
    }

  app->pids = pids;

  add_watch (app->in, G_IO_ERR | G_IO_HUP,
	     (GIOFunc) reap_gdb, app);

  add_watch (app->out, G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
	     (GIOFunc) process_gdb_output, app);

  app->state = STATE_START;

  app->loop = g_main_loop_new (NULL, TRUE);
  g_main_loop_run (app->loop);
  g_main_loop_unref (app->loop);

  g_slist_free (pids);
  g_free (app);

  return 0;
}
