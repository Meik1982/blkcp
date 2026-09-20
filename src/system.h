/* System definitions for modular dd
   Consolidated and trimmed from GNU Coreutils for dd.  */

#ifndef DD_SYSTEM_H
#define DD_SYSTEM_H

#include <attribute.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#if defined __has_include && __has_include (<stdckdint.h>)
# include <stdckdint.h>
#else
# include "stdckdint.h"
#endif

#include "configmake.h"
#include "propername.h"
#include "minmax.h"
#include "timespec.h"
#include "idx.h"
#include "xalloc.h"
#include "quotearg.h"
#include "quote.h"
#include "gettext.h"
#include "version-etc.h"
#include "intprops.h"

/* File permissions for output creation */
#define MODE_RW_UGO (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)

/* Limits */
#ifndef SSIZE_MAX
# define SSIZE_MAX TYPE_MAXIMUM (ssize_t)
#endif

#ifndef OFF_T_MAX
# define OFF_T_MAX TYPE_MAXIMUM (off_t)
#endif

#if ! HAVE_SYNC
# define sync() /* empty */
#endif

#ifndef initialize_main
# define initialize_main(ac, av)
#endif

/* Internationalization / gettext */
#if ! ENABLE_NLS
# undef textdomain
# define textdomain(Domainname) /* empty */
# undef bindtextdomain
# define bindtextdomain(Domainname, Dirname) /* empty */
#endif

#define _(msgid) gettext (msgid)
#define N_(msgid) msgid

static inline unsigned long int
select_plural (uintmax_t n)
{
  enum { PLURAL_REDUCER = 1000000 };
  return (n <= ULONG_MAX ? n : n % PLURAL_REDUCER + PLURAL_REDUCER);
}

/* String macros */
#define STREQ(a, b) (strcmp (a, b) == 0)
#define STRPREFIX(a, b) (strncmp (a, b, strlen (b)) == 0)

/* Author quoting */
#define proper_name(x) proper_name_lite (x, x)

/* Help and Version descriptions */
#define HELP_OPTION_DESCRIPTION \
  _("      --help        display this help and exit\n")
#define VERSION_OPTION_DESCRIPTION \
  _("      --version     output version information and exit\n")

#define emit_try_help() \
  do \
    { \
      fprintf (stderr, _("Try '%s --help' for more information.\n"), \
               program_name); \
    } \
  while (0)

static inline void
emit_ancillary_info (char const *program)
{
  printf (_("\n%s online help: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
  printf (_("Full documentation <%s%s>\n"), PACKAGE_URL, program);
  printf (_("or available locally via: info '(coreutils) %s invocation'\n"), program);
}

/* Filename quoting for error messages */
#define quotef(arg) \
  quotearg_n_style_colon (0, shell_escape_quoting_style, arg)
#define quotef_n(n, arg) \
  quotearg_n_style_colon (n, shell_escape_quoting_style, arg)
#define quoteaf(arg) \
  quotearg_style (shell_escape_always_quoting_style, arg)
#define quoteaf_n(n, arg) \
  quotearg_n_style (n, shell_escape_always_quoting_style, arg)

/* Highly optimized buffer zero-check (CCAN memeqzero via vector memcmp) */
ATTRIBUTE_PURE
static inline bool
is_nul (void const *buf, size_t length)
{
  const unsigned char *p = (const unsigned char *) buf;

  if (!length)
    return true;

  /* Check up to first 16 bytes individually */
  size_t head = length < 16 ? length : 16;
  for (size_t i = 0; i < head; i++)
    {
      if (p[i])
        return false;
    }

  if (length <= 16)
    return true;

  /* First 16 bytes are confirmed NUL: use glibc vectorized AVX2/SSE memcmp against itself */
  return memcmp (buf, p + 16, length - 16) == 0;
}

#endif /* DD_SYSTEM_H */
