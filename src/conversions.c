#include <config.h>
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
# include <immintrin.h>
#endif

#include "conversions.h"

/* Conversion flags matching dd definitions */
#define C_ASCII   000001
#define C_EBCDIC  000002
#define C_IBM     000004
#define C_BLOCK   000010
#define C_UNBLOCK 000020
#define C_LCASE   000040
#define C_UCASE   000100
#define C_SWAB    000200

#define to_uchar(c) ((unsigned char) (c))

static char const ascii_to_ebcdic[] =
{
  '\000', '\001', '\002', '\003', '\067', '\055', '\056', '\057',
  '\026', '\005', '\045', '\013', '\014', '\015', '\016', '\017',
  '\020', '\021', '\022', '\023', '\074', '\075', '\062', '\046',
  '\030', '\031', '\077', '\047', '\034', '\035', '\036', '\037',
  '\100', '\132', '\177', '\173', '\133', '\154', '\120', '\175',
  '\115', '\135', '\134', '\116', '\153', '\140', '\113', '\141',
  '\360', '\361', '\362', '\363', '\364', '\365', '\366', '\367',
  '\370', '\371', '\172', '\136', '\114', '\176', '\156', '\157',
  '\174', '\301', '\302', '\303', '\304', '\305', '\306', '\307',
  '\310', '\311', '\321', '\322', '\323', '\324', '\325', '\326',
  '\327', '\330', '\331', '\342', '\343', '\344', '\345', '\346',
  '\347', '\350', '\351', '\255', '\340', '\275', '\232', '\155',
  '\171', '\201', '\202', '\203', '\204', '\205', '\206', '\207',
  '\210', '\211', '\221', '\222', '\223', '\224', '\225', '\226',
  '\227', '\230', '\231', '\242', '\243', '\244', '\245', '\246',
  '\247', '\250', '\251', '\300', '\117', '\320', '\137', '\007',
  '\040', '\041', '\042', '\043', '\044', '\025', '\006', '\027',
  '\050', '\051', '\052', '\053', '\054', '\011', '\012', '\033',
  '\060', '\061', '\032', '\063', '\064', '\065', '\066', '\010',
  '\070', '\071', '\072', '\073', '\004', '\024', '\076', '\341',
  '\101', '\102', '\103', '\104', '\105', '\106', '\107', '\110',
  '\111', '\121', '\122', '\123', '\124', '\125', '\126', '\127',
  '\130', '\131', '\142', '\143', '\144', '\145', '\146', '\147',
  '\150', '\151', '\160', '\161', '\162', '\163', '\164', '\165',
  '\166', '\167', '\170', '\200', '\212', '\213', '\214', '\215',
  '\216', '\217', '\220', '\152', '\233', '\234', '\235', '\236',
  '\237', '\240', '\252', '\253', '\254', '\112', '\256', '\257',
  '\260', '\261', '\262', '\263', '\264', '\265', '\266', '\267',
  '\270', '\271', '\272', '\273', '\274', '\241', '\276', '\277',
  '\312', '\313', '\314', '\315', '\316', '\317', '\332', '\333',
  '\334', '\335', '\336', '\337', '\352', '\353', '\354', '\355',
  '\356', '\357', '\372', '\373', '\374', '\375', '\376', '\377'
};

static char const ascii_to_ibm[] =
{
  '\000', '\001', '\002', '\003', '\067', '\055', '\056', '\057',
  '\026', '\005', '\045', '\013', '\014', '\015', '\016', '\017',
  '\020', '\021', '\022', '\023', '\074', '\075', '\062', '\046',
  '\030', '\031', '\077', '\047', '\034', '\035', '\036', '\037',
  '\100', '\132', '\177', '\173', '\133', '\154', '\120', '\175',
  '\115', '\135', '\134', '\116', '\153', '\140', '\113', '\141',
  '\360', '\361', '\362', '\363', '\364', '\365', '\366', '\367',
  '\370', '\371', '\172', '\136', '\114', '\176', '\156', '\157',
  '\174', '\301', '\302', '\303', '\304', '\305', '\306', '\307',
  '\310', '\311', '\321', '\322', '\323', '\324', '\325', '\326',
  '\327', '\330', '\331', '\342', '\343', '\344', '\345', '\346',
  '\347', '\350', '\351', '\255', '\340', '\275', '\137', '\155',
  '\171', '\201', '\202', '\203', '\204', '\205', '\206', '\207',
  '\210', '\211', '\221', '\222', '\223', '\224', '\225', '\226',
  '\227', '\230', '\231', '\242', '\243', '\244', '\245', '\246',
  '\247', '\250', '\251', '\300', '\117', '\320', '\241', '\007',
  '\040', '\041', '\042', '\043', '\044', '\025', '\006', '\027',
  '\050', '\051', '\052', '\053', '\054', '\011', '\012', '\033',
  '\060', '\061', '\032', '\063', '\064', '\065', '\066', '\010',
  '\070', '\071', '\072', '\073', '\004', '\024', '\076', '\341',
  '\101', '\102', '\103', '\104', '\105', '\106', '\107', '\110',
  '\111', '\121', '\122', '\123', '\124', '\125', '\126', '\127',
  '\130', '\131', '\142', '\143', '\144', '\145', '\146', '\147',
  '\150', '\151', '\160', '\161', '\162', '\163', '\164', '\165',
  '\166', '\167', '\170', '\200', '\212', '\213', '\214', '\215',
  '\216', '\217', '\220', '\232', '\233', '\234', '\235', '\236',
  '\237', '\240', '\252', '\253', '\254', '\255', '\256', '\257',
  '\260', '\261', '\262', '\263', '\264', '\265', '\266', '\267',
  '\270', '\271', '\272', '\273', '\274', '\275', '\276', '\277',
  '\312', '\313', '\314', '\315', '\316', '\317', '\332', '\333',
  '\334', '\335', '\336', '\337', '\352', '\353', '\354', '\355',
  '\356', '\357', '\372', '\373', '\374', '\375', '\376', '\377'
};

static char const ebcdic_to_ascii[] =
{
  '\000', '\001', '\002', '\003', '\234', '\011', '\206', '\177',
  '\227', '\215', '\216', '\013', '\014', '\015', '\016', '\017',
  '\020', '\021', '\022', '\023', '\235', '\205', '\010', '\207',
  '\030', '\031', '\222', '\217', '\034', '\035', '\036', '\037',
  '\200', '\201', '\202', '\203', '\204', '\012', '\027', '\033',
  '\210', '\211', '\212', '\213', '\214', '\005', '\006', '\007',
  '\220', '\221', '\026', '\223', '\224', '\225', '\226', '\004',
  '\230', '\231', '\232', '\233', '\024', '\025', '\236', '\032',
  '\040', '\240', '\241', '\242', '\243', '\244', '\245', '\246',
  '\247', '\250', '\325', '\056', '\074', '\050', '\053', '\174',
  '\046', '\251', '\252', '\253', '\254', '\255', '\256', '\257',
  '\260', '\261', '\041', '\044', '\052', '\051', '\073', '\176',
  '\055', '\057', '\262', '\263', '\264', '\265', '\266', '\267',
  '\270', '\271', '\313', '\054', '\045', '\137', '\076', '\077',
  '\272', '\273', '\274', '\275', '\276', '\277', '\300', '\301',
  '\302', '\303', '\304', '\305', '\306', '\307', '\310', '\311',
  '\322', '\323', '\324', '\072', '\043', '\100', '\047', '\075',
  '\042', '\330', '\141', '\142', '\143', '\144', '\145', '\146',
  '\147', '\150', '\151', '\261', '\134', '\173', '\340', '\341',
  '\342', '\152', '\153', '\154', '\155', '\156', '\157', '\160',
  '\161', '\162', '\136', '\175', '\343', '\344', '\345', '\346',
  '\347', '\176', '\163', '\164', '\165', '\166', '\167', '\170',
  '\171', '\172', '\133', '\140', '\350', '\351', '\352', '\353',
  '\354', '\355', '\356', '\357', '\360', '\361', '\362', '\363',
  '\364', '\365', '\366', '\367', '\370', '\371', '\372', '\373',
  '\374', '\375', '\376', '\377', '\173', '\101', '\102', '\103',
  '\104', '\105', '\106', '\107', '\110', '\111', '\360', '\163',
  '\145', '\151', '\143', '\144', '\175', '\121', '\122', '\123',
  '\124', '\125', '\126', '\127', '\130', '\131', '\165', '\170',
  '\160', '\157', '\162', '\164', '\134', '\237', '\123', '\124',
  '\125', '\126', '\127', '\130', '\131', '\132', '\262', '\140',
  '\043', '\247', '\260', '\270', '\060', '\061', '\062', '\063',
  '\064', '\065', '\066', '\067', '\070', '\071', '\333', '\334',
  '\335', '\336', '\337', '\032'
};

void
dd_init_translations (unsigned char *trans_table)
{
  for (int i = 0; i < 256; i++)
    trans_table[i] = (unsigned char) i;
}

static void
translate_charset (unsigned char *trans_table, char const *new_trans)
{
  for (int i = 0; i < 256; i++)
    trans_table[i] = new_trans[trans_table[i]];
}

void
dd_apply_translations (unsigned char *trans_table,
                       int conversions_mask,
                       char *newline_char,
                       char *space_char,
                       bool *translation_needed,
                       int *trans_mode)
{
  if ((conversions_mask & (C_ASCII | C_EBCDIC | C_IBM)) == 0)
    {
      if ((conversions_mask & (C_UCASE | C_LCASE)) == C_UCASE)
        {
          if (translation_needed) *translation_needed = true;
          if (trans_mode) *trans_mode = 2; /* TRANS_MODE_FAST_UCASE */
          return;
        }
      else if ((conversions_mask & (C_UCASE | C_LCASE)) == C_LCASE)
        {
          if (translation_needed) *translation_needed = true;
          if (trans_mode) *trans_mode = 3; /* TRANS_MODE_FAST_LCASE */
          return;
        }
    }

  if (trans_mode) *trans_mode = 1; /* TRANS_MODE_TABLE */

  if (conversions_mask & C_ASCII)
    {
      translate_charset (trans_table, ebcdic_to_ascii);
      if (translation_needed) *translation_needed = true;
    }

  if (conversions_mask & C_UCASE)
    {
      for (int i = 0; i < 256; i++)
        trans_table[i] = toupper (trans_table[i]);
      if (translation_needed) *translation_needed = true;
    }
  else if (conversions_mask & C_LCASE)
    {
      for (int i = 0; i < 256; i++)
        trans_table[i] = tolower (trans_table[i]);
      if (translation_needed) *translation_needed = true;
    }

  if (conversions_mask & C_EBCDIC)
    {
      translate_charset (trans_table, ascii_to_ebcdic);
      if (newline_char) *newline_char = ascii_to_ebcdic['\n'];
      if (space_char) *space_char = ascii_to_ebcdic[' '];
      if (translation_needed) *translation_needed = true;
    }
  else if (conversions_mask & C_IBM)
    {
      translate_charset (trans_table, ascii_to_ibm);
      if (newline_char) *newline_char = ascii_to_ibm['\n'];
      if (space_char) *space_char = ascii_to_ibm[' '];
      if (translation_needed) *translation_needed = true;
    }
}

/* Fast SIMD-vectorizable branchless ASCII upper-case transformation */
void
dd_vector_ucase (char *buf, idx_t nread)
{
  unsigned char *p = (unsigned char *) buf;
  for (idx_t i = 0; i < nread; i++)
    {
      unsigned char c = p[i];
      p[i] = (c >= 'a' && c <= 'z') ? (unsigned char)(c - 32) : c;
    }
}

/* Fast SIMD-vectorizable branchless ASCII lower-case transformation */
void
dd_vector_lcase (char *buf, idx_t nread)
{
  unsigned char *p = (unsigned char *) buf;
  for (idx_t i = 0; i < nread; i++)
    {
      unsigned char c = p[i];
      p[i] = (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
    }
}

void
dd_translate_buffer (unsigned char const *trans_table, char *buf, idx_t nread)
{
  char *cp = buf;
  for (idx_t i = nread; i; i--, cp++)
    *cp = trans_table[to_uchar (*cp)];
}

static void
swab_scalar (char *buf, idx_t len)
{
  for (idx_t i = 0; i + 2 <= len; i += 2)
    {
      char tmp = buf[i];
      buf[i] = buf[i + 1];
      buf[i + 1] = tmp;
    }
}

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2")))
static void
swab_avx2 (char *buf, idx_t len)
{
  const __m256i mask = _mm256_setr_epi8 (
      1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14,
      1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14
  );

  idx_t i = 0;
  /* 4x unrolled loop: 128 bytes per iteration */
  for (; i + 128 <= len; i += 128)
    {
      __m256i v0 = _mm256_loadu_si256 ((const __m256i *) (buf + i));
      __m256i v1 = _mm256_loadu_si256 ((const __m256i *) (buf + i + 32));
      __m256i v2 = _mm256_loadu_si256 ((const __m256i *) (buf + i + 64));
      __m256i v3 = _mm256_loadu_si256 ((const __m256i *) (buf + i + 96));

      v0 = _mm256_shuffle_epi8 (v0, mask);
      v1 = _mm256_shuffle_epi8 (v1, mask);
      v2 = _mm256_shuffle_epi8 (v2, mask);
      v3 = _mm256_shuffle_epi8 (v3, mask);

      _mm256_storeu_si256 ((__m256i *) (buf + i), v0);
      _mm256_storeu_si256 ((__m256i *) (buf + i + 32), v1);
      _mm256_storeu_si256 ((__m256i *) (buf + i + 64), v2);
      _mm256_storeu_si256 ((__m256i *) (buf + i + 96), v3);
    }

  for (; i + 32 <= len; i += 32)
    {
      __m256i v = _mm256_loadu_si256 ((const __m256i *) (buf + i));
      v = _mm256_shuffle_epi8 (v, mask);
      _mm256_storeu_si256 ((__m256i *) (buf + i), v);
    }

  /* 16-byte tail with SSSE3 */
  if (i + 16 <= len)
    {
      __m128i m128 = _mm_setr_epi8 (1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
      __m128i v = _mm_loadu_si128 ((const __m128i *) (buf + i));
      v = _mm_shuffle_epi8 (v, m128);
      _mm_storeu_si128 ((__m128i *) (buf + i), v);
      i += 16;
    }

  /* Tail scalar swap */
  swab_scalar (buf + i, len - i);
}
#endif

char *
dd_swab_buffer (char *buf, idx_t *nread, int *saved_byte)
{
  if (*nread == 0)
    return buf;

  int prev_saved = *saved_byte;

  /* Fast path: Standard transfer without odd carry-over byte from previous read */
  if (prev_saved < 0)
    {
      if (*nread & 1)
        {
          *saved_byte = (unsigned char) buf[--*nread];
        }
      else
        {
          *saved_byte = -1;
        }

#if defined(__x86_64__) || defined(_M_X64)
      if (__builtin_cpu_supports ("avx2"))
        swab_avx2 (buf, *nread);
      else
        swab_scalar (buf, *nread);
#else
      swab_scalar (buf, *nread);
#endif
      return buf;
    }

  /* Robust fallback path: Carry-over byte from a previous odd block exists */
  if (*nread & 1)
    *saved_byte = -1;
  else
    {
      unsigned char c = buf[--*nread];
      *saved_byte = c;
    }

  for (idx_t i = *nread; 1 < i; i -= 2)
    buf[i] = buf[i - 2];

  buf[1] = prev_saved;
  ++*nread;
  return buf;
}
