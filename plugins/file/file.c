/* nbdkit
 * Copyright (C) 2013 Red Hat Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of Red Hat nor the names of its contributors may be
 * used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY RED HAT AND CONTRIBUTORS ''AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL RED HAT OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

#include <nbdkit-plugin.h>

static char *filename = NULL;
static int rdelayms = 0;        /* read delay (milliseconds) */
static int wdelayms = 0;        /* write delay (milliseconds) */

static void
file_unload (void)
{
  free (filename);
}

static int
parse_delay (const char *value)
{
  size_t len = strlen (value);
  int r;

  if (len > 2 && strcmp (&value[len-2], "ms") == 0) {
    if (sscanf (value, "%d", &r) == 1)
      return r;
    else {
      nbdkit_error ("cannot parse rdelay/wdelay milliseconds parameter: %s",
                    value);
      return -1;
    }
  }
  else {
    if (sscanf (value, "%d", &r) == 1)
      return r * 1000;
    else {
      nbdkit_error ("cannot parse rdelay/wdelay seconds parameter: %s",
                    value);
      return -1;
    }
  }
}

/* Called for each key=value passed on the command line.  This plugin
 * only accepts file=<filename>, which is required.
 */
static int
file_config (const char *key, const char *value)
{
  if (strcmp (key, "file") == 0) {
    /* See FILENAMES AND PATHS in nbdkit-plugin(3). */
    filename = nbdkit_absolute_path (value);
    if (!filename)
      return -1;
  }
  else if (strcmp (key, "rdelay") == 0) {
    rdelayms = parse_delay (value);
    if (rdelayms == -1)
      return -1;
  }
  else if (strcmp (key, "wdelay") == 0) {
    wdelayms = parse_delay (value);
    if (wdelayms == -1)
      return -1;
  }
  else {
    nbdkit_error ("unknown parameter '%s'", key);
    return -1;
  }

  return 0;
}

/* Check the user did pass a file=<FILENAME> parameter. */
static int
file_config_complete (void)
{
  if (filename == NULL) {
    nbdkit_error ("you must supply the file=<FILENAME> parameter after the plugin name on the command line");
    return -1;
  }

  return 0;
}

#define file_config_help \
  "file=<FILENAME>     (required) The filename to serve.\n" \
  "rdelay=<NN>[ms]                Read delay in seconds/milliseconds.\n" \
  "wdelay=<NN>[ms]                Write delay in seconds/milliseconds." \

/* The per-connection handle. */
struct handle {
  int fd;
};

/* Create the per-connection handle. */
static void *
file_open (int readonly)
{
  struct handle *h;
  int flags;

  h = malloc (sizeof *h);
  if (h == NULL) {
    nbdkit_error ("malloc: %m");
    return NULL;
  }

  flags = O_CLOEXEC|O_NOCTTY;
  if (readonly)
    flags |= O_RDONLY;
  else
    flags |= O_RDWR;

  h->fd = open (filename, flags);
  if (h->fd == -1) {
    nbdkit_error ("open: %s: %m", filename);
    free (h);
    return NULL;
  }

  return h;
}

/* Free up the per-connection handle. */
static void
file_close (void *handle)
{
  struct handle *h = handle;

  close (h->fd);
  free (h);
}

#define THREAD_MODEL NBDKIT_THREAD_MODEL_SERIALIZE_REQUESTS

/* Get the file size. */
static int64_t
file_get_size (void *handle)
{
  struct handle *h = handle;
  struct stat statbuf;

  if (fstat (h->fd, &statbuf) == -1) {
    nbdkit_error ("stat: %m");
    return -1;
  }

  return statbuf.st_size;
}

/* Read data from the file. */
static int
file_pread (void *handle, void *buf, uint32_t count, uint64_t offset)
{
  struct handle *h = handle;

  if (rdelayms > 0) {
    const struct timespec ts = {
      .tv_sec = rdelayms / 1000,
      .tv_nsec = (rdelayms * 1000000) % 1000000000
    };
    nanosleep (&ts, NULL);
  }

  while (count > 0) {
    ssize_t r = pread (h->fd, buf, count, offset);
    if (r == -1) {
      nbdkit_error ("pread: %m");
      return -1;
    }
    if (r == 0) {
      nbdkit_error ("pread: unexpected end of file");
      return -1;
    }
    buf += r;
    count -= r;
    offset += r;
  }

  return 0;
}

/* Write data to the file. */
static int
file_pwrite (void *handle, const void *buf, uint32_t count, uint64_t offset)
{
  struct handle *h = handle;

  if (wdelayms > 0) {
    const struct timespec ts = {
      .tv_sec = wdelayms / 1000,
      .tv_nsec = (wdelayms * 1000000) % 1000000000
    };
    nanosleep (&ts, NULL);
  }

  while (count > 0) {
    ssize_t r = pwrite (h->fd, buf, count, offset);
    if (r == -1) {
      nbdkit_error ("pwrite: %m");
      return -1;
    }
    buf += r;
    count -= r;
    offset += r;
  }

  return 0;
}

/* Flush the file to disk. */
static int
file_flush (void *handle)
{
  struct handle *h = handle;

  if (fdatasync (h->fd) == -1) {
    nbdkit_error ("fdatasync: %m");
    return -1;
  }

  return 0;
}

static struct nbdkit_plugin plugin = {
  .name              = "file",
  .longname          = "nbdkit file plugin",
  .version           = PACKAGE_VERSION,
  .unload            = file_unload,
  .config            = file_config,
  .config_complete   = file_config_complete,
  .config_help       = file_config_help,
  .open              = file_open,
  .close             = file_close,
  .get_size          = file_get_size,
  .pread             = file_pread,
  .pwrite            = file_pwrite,
  .flush             = file_flush,
};

NBDKIT_REGISTER_PLUGIN(plugin)
