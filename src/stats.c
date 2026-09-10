#include <config.h>
#include <stdio.h>
#include <string.h>
#include "stats.h"
#include "system.h"
#include "human.h"
#include "gethrxtime.h"
#include "xtime.h"

static int const human_opts =
  (human_autoscale | human_round_to_nearest
   | human_space_before_unit | human_SI | human_B);

static inline bool
abbreviation_lacks_prefix (char const *message)
{
  return message[strlen (message) - 2] == ' ';
}

void
dd_print_xfer_stats (const dd_stats_t *stats, int *progress_len, xtime_t progress_time)
{
  xtime_t now = progress_time ? progress_time : gethrxtime ();
  static char const slash_s[] = "/s";
  char hbuf[3][LONGEST_HUMAN_READABLE + sizeof slash_s];
  double delta_s;
  char const *bytes_per_second;
  char const *si = human_readable (stats->w_bytes, hbuf[0], human_opts, 1, 1);
  char const *iec = human_readable (stats->w_bytes, hbuf[1],
                                    human_opts | human_base_1024, 1, 1);

  char *bpsbuf = hbuf[2];
  int bpsbufsize = sizeof hbuf[2];
  if (stats->start_time < now)
    {
      double XTIME_PRECISIONe0 = XTIME_PRECISION;
      xtime_t delta_xtime = now - stats->start_time;
      delta_s = delta_xtime / XTIME_PRECISIONe0;
      bytes_per_second = human_readable (stats->w_bytes, bpsbuf, human_opts,
                                         XTIME_PRECISION, delta_xtime);
      strcat (bytes_per_second - bpsbuf + bpsbuf, slash_s);
    }
  else
    {
      delta_s = 0;
      snprintf (bpsbuf, bpsbufsize, "%s B/s", _("Infinity"));
      bytes_per_second = bpsbuf;
    }

  if (progress_time)
    fputc ('\r', stderr);

  char delta_s_buf[24];
  snprintf (delta_s_buf, sizeof delta_s_buf,
            progress_time ? "%.0f s" : "%g s", delta_s);

  int stats_len
    = (abbreviation_lacks_prefix (si)
       ? fprintf (stderr,
                  ngettext ("%jd byte copied, %s, %s",
                            "%jd bytes copied, %s, %s",
                            select_plural (stats->w_bytes)),
                  stats->w_bytes, delta_s_buf, bytes_per_second)
       : abbreviation_lacks_prefix (iec)
       ? fprintf (stderr,
                  _("%jd bytes (%s) copied, %s, %s"),
                  stats->w_bytes, si, delta_s_buf, bytes_per_second)
       : fprintf (stderr,
                  _("%jd bytes (%s, %s) copied, %s, %s"),
                  stats->w_bytes, si, iec, delta_s_buf, bytes_per_second));

  if (progress_time)
    {
      if (progress_len && 0 <= stats_len && stats_len < *progress_len)
        fprintf (stderr, "%*s", *progress_len - stats_len, "");
      if (progress_len)
        *progress_len = stats_len;
    }
  else
    fputc ('\n', stderr);
}

void
dd_print_stats (const dd_stats_t *stats, int status_level, int *progress_len)
{
  if (status_level == STATUS_NONE)
    return;

  if (progress_len && 0 < *progress_len)
    {
      fputc ('\n', stderr);
      *progress_len = 0;
    }

  fprintf (stderr,
           _("%jd+%jd records in\n"
             "%jd+%jd records out\n"),
           stats->r_full, stats->r_partial, stats->w_full, stats->w_partial);

  if (stats->r_truncate != 0)
    fprintf (stderr,
             ngettext ("%jd truncated record\n",
                       "%jd truncated records\n",
                       select_plural (stats->r_truncate)),
             stats->r_truncate);

  if (status_level == STATUS_NOXFER)
    return;

  dd_print_xfer_stats (stats, progress_len, 0);
}

void
dd_print_hash (const unsigned char *digest)
{
  char hex[65];
  for (int i = 0; i < 32; i++)
    sprintf (hex + i * 2, "%02x", digest[i]);
  hex[64] = '\0';
  fprintf (stderr, "sha256: %s\n", hex);
}
