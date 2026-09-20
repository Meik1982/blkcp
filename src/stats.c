#include <config.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
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
      fflush (stderr);
    }
  else
    fputc ('\n', stderr);
}

static void
format_json_double (char *buf, size_t sz, double val, int decimals)
{
  snprintf (buf, sz, "%.*f", decimals, val);
  for (char *p = buf; *p; p++)
    {
      if (*p == ',')
        *p = '.';
    }
}

void
dd_print_json_progress (const dd_stats_t *stats, intmax_t total_bytes, xtime_t progress_time)
{
  xtime_t now = progress_time ? progress_time : gethrxtime ();
  double delta_s = 0.0;
  double speed_bps = 0.0;

  if (stats->start_time < now)
    {
      xtime_t delta_xtime = now - stats->start_time;
      delta_s = (double) delta_xtime / (double) XTIME_PRECISION;
      if (delta_s > 0.000001)
        speed_bps = (double) stats->w_bytes / delta_s;
    }

  char delta_s_buf[32];
  char speed_buf[32];
  format_json_double (delta_s_buf, sizeof delta_s_buf, delta_s, 2);
  format_json_double (speed_buf, sizeof speed_buf, speed_bps, 0);

  if (total_bytes > 0)
    {
      double pct = ((double) stats->w_bytes * 100.0) / (double) total_bytes;
      if (pct > 100.0)
        pct = 100.0;

      double eta_s = 0.0;
      if (speed_bps > 0.0 && stats->w_bytes < total_bytes)
        eta_s = (double) (total_bytes - stats->w_bytes) / speed_bps;

      char pct_buf[32];
      char eta_s_buf[32];
      format_json_double (pct_buf, sizeof pct_buf, pct, 2);
      format_json_double (eta_s_buf, sizeof eta_s_buf, eta_s, 1);

      fprintf (stderr,
               "{\"event\":\"progress\",\"copied_bytes\":%jd,\"total_bytes\":%jd,\"percent\":%s,\"speed_bps\":%s,\"elapsed_s\":%s,\"eta_s\":%s}\n",
               stats->w_bytes, total_bytes, pct_buf, speed_buf, delta_s_buf, eta_s_buf);
    }
  else
    {
      fprintf (stderr,
               "{\"event\":\"progress\",\"copied_bytes\":%jd,\"total_bytes\":null,\"percent\":null,\"speed_bps\":%s,\"elapsed_s\":%s,\"eta_s\":null}\n",
               stats->w_bytes, speed_buf, delta_s_buf);
    }
  fflush (stderr);
}

void
dd_print_json_summary (const dd_stats_t *stats, const unsigned char *digest, bool has_digest)
{
  xtime_t now = gethrxtime ();
  double delta_s = 0.0;
  double speed_bps = 0.0;

  if (stats->start_time < now)
    {
      xtime_t delta_xtime = now - stats->start_time;
      delta_s = (double) delta_xtime / (double) XTIME_PRECISION;
      if (delta_s > 0.000001)
        speed_bps = (double) stats->w_bytes / delta_s;
    }

  char delta_s_buf[32];
  char speed_buf[32];
  format_json_double (delta_s_buf, sizeof delta_s_buf, delta_s, 4);
  format_json_double (speed_buf, sizeof speed_buf, speed_bps, 0);

  char hex[65];
  if (has_digest && digest)
    {
      for (int i = 0; i < 32; i++)
        sprintf (hex + i * 2, "%02x", digest[i]);
      hex[64] = '\0';
    }

  char pipeline_buf[128];
  if (stats->async_capacity > 0)
    {
      snprintf (pipeline_buf, sizeof pipeline_buf,
                "{\"capacity\":%zu,\"reader_stalls\":%" PRIu64 ",\"writer_stalls\":%" PRIu64 "}",
                stats->async_capacity, stats->async_reader_stalls, stats->async_writer_stalls);
    }
  else
    {
      snprintf (pipeline_buf, sizeof pipeline_buf, "null");
    }

  fprintf (stderr,
           "{\"event\":\"finished\",\"copied_bytes\":%jd,\"records_in\":{\"full\":%jd,\"partial\":%jd,\"truncated\":%jd},\"records_out\":{\"full\":%jd,\"partial\":%jd},\"elapsed_s\":%s,\"avg_speed_bps\":%s,\"pipeline\":%s,\"sha256\":%s%s%s}\n",
           stats->w_bytes,
           stats->r_full, stats->r_partial, stats->r_truncate,
           stats->w_full, stats->w_partial,
           delta_s_buf, speed_buf,
           pipeline_buf,
           has_digest ? "\"" : "",
           has_digest ? hex : "null",
           has_digest ? "\"" : "");
  fflush (stderr);
}

void
dd_check_progress (dd_context_t *ctx)
{
  if (!ctx)
    return;

  bool is_json = (ctx->cfg.json_output || ctx->cfg.status_level == STATUS_JSON);
  bool is_text_progress = (ctx->cfg.status_level == STATUS_PROGRESS && !is_json);

  if (!is_json && !is_text_progress)
    return;

  xtime_t now = gethrxtime ();
  if (now >= ctx->stats.next_time)
    {
      if (is_json)
        {
          intmax_t total = ctx->cfg.bytes_to_copy > 0 ? ctx->cfg.bytes_to_copy : ctx->total_input_size;
          dd_print_json_progress (&ctx->stats, total, now);
        }
      else
        {
          dd_print_xfer_stats (&ctx->stats, &ctx->stats.progress_len, now);
        }
      ctx->stats.next_time = now + XTIME_PRECISION;
    }
}

void
dd_print_stats (const dd_context_t *ctx)
{
  if (!ctx)
    return;

  if (ctx->cfg.json_output || ctx->cfg.status_level == STATUS_JSON)
    {
      dd_print_json_summary (&ctx->stats, ctx->sha_digest, ctx->sha_computed);
      return;
    }

  if (ctx->cfg.status_level != STATUS_NONE)
    {
      if (ctx->stats.progress_len > 0)
        {
          fputc ('\n', stderr);
          ((dd_context_t *) ctx)->stats.progress_len = 0;
        }

      fprintf (stderr,
               _("%jd+%jd records in\n"
                 "%jd+%jd records out\n"),
               ctx->stats.r_full, ctx->stats.r_partial, ctx->stats.w_full, ctx->stats.w_partial);

      if (ctx->stats.r_truncate != 0)
        fprintf (stderr,
                 ngettext ("%jd truncated record\n",
                           "%jd truncated records\n",
                           select_plural (ctx->stats.r_truncate)),
                 ctx->stats.r_truncate);

      if (ctx->cfg.status_level != STATUS_NOXFER)
        dd_print_xfer_stats (&ctx->stats, NULL, 0);
    }

  if (ctx->sha_computed)
    dd_print_hash (ctx->sha_digest);
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
