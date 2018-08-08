/**
 * \file strutil.c
 *
 * \brief String utilities for PennMUSH.
 *
 *
 */

#define _GNU_SOURCE /* For strchrnul, if applicable. */

#include "copyrite.h"
#include "strutil.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_INTTYPES_H
#if defined(__MINGW32__) || defined(__MINGW64__)
#undef __USE_MINGW_ANSI_STDIO
#endif
#include <inttypes.h>
#endif

#include "ansi.h"
#include "case.h"
#include "conf.h"
#include "log.h"
#include "markup.h"
#include "memcheck.h"
#include "mymalloc.h"
#include "mypcre.h"
#include "parse.h"
#include "pueblo.h"
#include "charclass.h"
#include "myutf8.h"
#include "memcheck.h"

/* TODO: Adding this prototype here is cheating, but it's easier for now. Clean
   this up eventually... */
void mush_panic(const char *);

/* TODO: Move this prototype elsewhere since this function is shared. */
int format_long(intmax_t val, char *buff, char **bp, int maxlen, int base);

/** Copy the first len bytes of a string
 * \param str the source string
 * \parma len the number of bytes to copy.
 * \param check memcheck id
 * \return newly allocated string
 */
char *
mush_strndup(const char *src, size_t len, const char *check)
{
  char *copy;
  size_t rlen = strlen(src);

  if (rlen < len)
    len = rlen;

  copy = mush_malloc(len + 1 + SSE_OFFSET, check);
  if (copy) {
    memcpy(copy, src, len);
    copy[len] = '\0';
  }

  return copy;
}

/** Copy the first len codepoints of a UTF-8 string
 * \param str the source string in UTF-8
 * \parma len the number of Unicode codepoints to copy.
 * \param check memcheck id
 * \return newly allocated string or NULL on error like malformed UTF-8
 */
char *
mush_strndup_cp(const char *src, size_t len, const char *check)
{
  char *copy;
  int offset = 0;
  UChar32 c;

  U8_NEXT(src, offset, -1, c);
  while (c > 0 && --len) {
    U8_NEXT(src, offset, -1, c);
  }

  if (c < 0) {
    return NULL;
  }

  copy = mush_malloc(offset + 1 + SSE_OFFSET, check);
  if (copy) {
    memcpy(copy, src, offset);
    copy[offset] = '\0';
  }

  return copy;
}

/** Our version of strdup, with memory leak checking.
 * This should be used in preference to strdup, and in assocation
 * with mush_free().
 * \param s string to duplicate.
 * \param check label for memory checking.
 * \return newly allocated copy of s.
 */
char *
mush_strdup(const char *s, const char *check __attribute__((__unused__)))
{
  char *x;
  size_t len = strlen(s) + 1;
  x = mush_malloc_zero(len + SSE_OFFSET, check);
  if (x)
    memcpy(x, s, len);
  return x;
}

/* Windows wrapper for snprintf */
#if !defined(HAVE_SNPRINTF) && defined(HAVE__VSNPRINTF_S)
int
sane_snprintf_s(char *str, size_t len, const char *fmt, ...)
{
  va_list args;
  int ret;

  va_start(args, fmt);
  ret = _vsnprintf_s(str, len, _TRUNCATE, fmt, args);
  va_end(args);

  return ret;
}
#endif

/* Wrapper for systems without vsnprintf. */
int
mush_vsnprintf(char *str, size_t len, const char *fmt, va_list ap)
{
  int ret;

#if defined(HAVE__VSNPRINTF_S)
  /* Windows version */
  ret = _vsnprintf_s(str, len, _TRUNCATE, fmt, ap);
#elif defined(HAVE_VSNPRINTF)
  /* C99 version */
  ret = vsnprintf(str, len, fmt, ap);
#else
  /* Old school icky unsafe version */
  {
    static char buff[BUFFER_LEN * 3];

    ret = vsprintf(buff, fmt, ap);

    if ((size_t) ret > len)
      ret = len;

    memcpy(str, buff, ret);
    str[ret] = '\0';
  }
#endif

  return ret;
}

/** Return the string chopped at lim characters.
 * lim must be <= BUFFER_LEN
 * For UTF-8 strings, use mush_strndup_cp() instead.
 * \param str string to chop.
 * \param lim character at which to chop the string.
 * \return statically allocated buffer with chopped string.
 */
char *
chopstr(const char *str, size_t lim)
{
  static char tbuf1[BUFFER_LEN];
  if (strlen(str) <= lim)
    return (char *) str;
  if (lim >= BUFFER_LEN)
    lim = BUFFER_LEN;
  mush_strncpy(tbuf1, str, lim);
  return tbuf1;
}

#if !defined(HAVE_STRCASECMP) && !defined(HAVE__STRICMP)
/** strcasecmp for systems without it.
 * \param s1 one string to compare.
 * \param s2 another string to compare.
 * \retval -1 s1 is less than s2.
 * \retval 0 s1 equals s2
 * \retval 1 s1 is greater than s2.
 */
int
strcasecmp(const char *s1, const char *s2)
{
  while (*s1 && *s2 && DOWNCASE(*s1) == DOWNCASE(*s2))
    s1++, s2++;

  return (DOWNCASE(*s1) - DOWNCASE(*s2));
}
#endif

#if !defined(HAVE_STRNCASECMP) && !defined(HAVE__STRNICMP)
/** strncasecmp for systems without it.
 * \param s1 one string to compare.
 * \param s2 another string to compare.
 * \param n number of characters to compare.
 * \retval -1 s1 is less than s2.
 * \retval 0 s1 equals s2
 * \retval 1 s1 is greater than s2.
 */
int
strncasecmp(const char *s1, const char *s2, size_t n)
{
  for (; 0 < n; ++s1, ++s2, --n)
    if (DOWNCASE(*s1) != DOWNCASE(*s2))
      return DOWNCASE(*s1) - DOWNCASE(*s2);
    else if (*s1 == 0)
      return 0;
  return 0;
}
#endif

/** Does string begin with prefix?
 * This comparison is case-insensitive. An empty prefix always
 * matches.
 * \param string to check.
 * \param prefix to check against.
 * \retval 1 string begins with prefix.
 * \retval 0 string does not begin with prefix.
 */
bool
string_prefix(const char *RESTRICT string, const char *RESTRICT prefix)
{
  if (!string || !prefix)
    return 0;
  while (*string && *prefix && DOWNCASE(*string) == DOWNCASE(*prefix))
    string++, prefix++;
  return *prefix == '\0';
}

/** Does string begin with prefix?
 * This comparison is case-insensitive. An empty prefix always
 * fails.
 * \param string to check.
 * \param prefix to check against.
 * \retval 1 string begins with prefix.
 * \retval 0 string does not begin with prefix.
 */
bool
string_prefixe(const char *RESTRICT string, const char *RESTRICT prefix)
{
  if (!string || !prefix || !*prefix)
    return 0;
  while (*string && *prefix && DOWNCASE(*string) == DOWNCASE(*prefix))
    string++, prefix++;
  return *prefix == '\0';
}

/** Match a substring at the start of a word in a string, case-insensitively.
 * \param src a string of words to match against.
 * \param sub a prefix to match against the start of a word in string.
 * \return pointer into src at the matched word, or NULL.
 */
const char *
string_match(const char *src, const char *sub)
{
  if (!src || !sub)
    return 0;

  if (*sub != '\0') {
    while (*src) {
      if (string_prefix(src, sub))
        return src;
      /* else scan to beginning of next word */
      while (*src && isalnum(*src))
        src++;
      while (*src && !isalnum(*src))
        src++;
    }
  }
  return NULL;
}

/** Return an initial-cased version of a string in a static buffer.
 * \param s string to initial-case.
 * \return pointer to a static buffer containing the initial-cased version.
 */
char *
strinitial(const char *s)
{
  static char buf1[BUFFER_LEN];
  char *p;

  if (!s || !*s) {
    buf1[0] = '\0';
    return buf1;
  }
  strcpy(buf1, s);
  for (p = buf1; *p; p++)
    *p = DOWNCASE(*p);
  buf1[0] = UPCASE(buf1[0]);
  return buf1;
}

/** Return an initial-cased version of a string in a caller supplied buffer.
 * \param s string to initial-case.
 * \param d destination buffer.
 * \parem len length of buffer.
 * \return pointer to a static buffer containing the initial-cased version.
 */
char *
strinitial_r(const char *restrict s, char *restrict d, size_t len)
{
  size_t p;

  if (len == 1) {
    d[0] = '\0';
    return d;
  }

  if (*s) {
    d[0] = toupper(*s);
    s += 1;
  }

  for (p = 1; *s && p < len - 1; p += 1, s += 1) {
    d[p] = tolower(*s);
  }
  d[p] = '\0';

  return d;
}

/** Return an uppercased version of a string in a static buffer.
 * \param s string to uppercase.
 * \return pointer to a static buffer containing the uppercased version.
 */
char *
strupper(const char *s)
{
  static char buf1[BUFFER_LEN];
  char *p;

  if (!s || !*s) {
    buf1[0] = '\0';
    return buf1;
  }
  mush_strncpy(buf1, s, BUFFER_LEN);
  for (p = buf1; *p; p++)
    *p = UPCASE(*p);
  return buf1;
}

/** Return a lowercased version of a string in a static buffer.
 * \param s string to lowercase.
 * \return pointer to a static buffer containing the lowercased version.
 */
char *
strlower(const char *s)
{
  static char buf1[BUFFER_LEN];
  char *p;

  if (!s || !*s) {
    buf1[0] = '\0';
    return buf1;
  }
  mush_strncpy(buf1, s, BUFFER_LEN);
  for (p = buf1; *p; p++)
    *p = DOWNCASE(*p);
  return buf1;
}

/** Return an uppercased version of a string in a newly allocated buffer.
 * \param s string to uppercase.
 * \param name memcheck name.
 * \return pointer to a string containing the uppercased version.
 */
char *
strupper_a(const char *s, const char *name)
{
  size_t len;
  char *out, *o;

  len = strlen(s);
  o = out = mush_malloc(len + 1, name);
  for (; *s; s++) {
    *o++ = toupper(*s);
  }
  *o = '\0';

  return out;
}

/** Return a lowercased version of a string in a newly allocated buffer.
 * \param s string to lowercase.
 * \param name memcheck name.
 * \return pointer to a string containing the lowercased version.
 */
char *
strlower_a(const char *s, const char *name)
{
  size_t len;
  char *out, *o;

  len = strlen(s);
  o = out = mush_malloc(len + 1, name);
  for (; *s; s++) {
    *o++ = tolower(*s);
  }
  *o = '\0';

  return out;
}

/** Return an uppercased version of a string in a caller-supplied buffer.
 * \param s string to uppercase.
 * \param d destination buffer.
 * \parem len length of buffer.
 * \return pointer to a string containing the uppercased version.
 */
char *
strupper_r(const char *restrict s, char *restrict d, size_t len)
{
  size_t p;

  if (!s) {
    if (len) {
      *d = '\0';
    }
    return d;
  }

  for (p = 0; *s && p < len - 1; p += 1, s += 1) {
    d[p] = toupper(*s);
  }
  d[p] = '\0';

  return d;
}

/** Return a lowercased version of a string in a newly allocated buffer.
 * \param s string to lowercase.
 * \param d destination buffer.
 * \parem len length of buffer.
 * \return pointer to a string containing the lowercased version.
 */
char *
strlower_r(const char *restrict s, char *restrict d, size_t len)
{
  size_t p;

  if (!s) {
    if (len) {
      *d = '\0';
    }
    return d;
  }

  for (p = 0; *s && p < len - 1; p += 1, s += 1) {
    d[p] = tolower(*s);
  }
  d[p] = '\0';

  return d;
}

/** Modify a string in-place to uppercase.
 * \param s string to uppercase.
 * \return s, now modified to be all uppercase.
 */
char *
upcasestr(char *s)
{
  char *p;
  for (p = s; p && *p; p++)
    *p = UPCASE(*p);
  return s;
}

/** Safely add an accented string to a buffer.
 * \param base base string to which accents are applied.
 * \param tmplate accent template string.
 * \param len length of base (and tmplate).
 * \param buff pointer to buffer to store accented string.
 * \param bp pointer to pointer to insertion point in buff.
 * \retval 1 failed to store entire string.
 * \retval 0 success.
 */
int
safe_accent(const char *RESTRICT base, const char *RESTRICT tmplate, size_t len,
            char *buff, char **bp)
{
  /* base and tmplate must be the same length */
  size_t n;
  char c;

  for (n = 0; n < len; n++) {
    switch (base[n]) {
    case 'A':
      switch (tmplate[n]) {
      case '`':
        c = 192;
        break;
      case '\'':
        c = 193;
        break;
      case '^':
        c = 194;
        break;
      case '~':
        c = 195;
        break;
      case ':':
        c = 196;
        break;
      case 'o':
        c = 197;
        break;
      case 'e':
      case 'E':
        c = 198;
        break;
      default:
        c = 'A';
      }
      break;
    case 'a':
      switch (tmplate[n]) {
      case '`':
        c = 224;
        break;
      case '\'':
        c = 225;
        break;
      case '^':
        c = 226;
        break;
      case '~':
        c = 227;
        break;
      case ':':
        c = 228;
        break;
      case 'o':
        c = 229;
        break;
      case 'e':
      case 'E':
        c = 230;
        break;
      default:
        c = 'a';
      }
      break;
    case 'C':
      if (tmplate[n] == ',')
        c = 199;
      else
        c = 'C';
      break;
    case 'c':
      if (tmplate[n] == ',')
        c = 231;
      else
        c = 'c';
      break;
    case 'E':
      switch (tmplate[n]) {
      case '`':
        c = 200;
        break;
      case '\'':
        c = 201;
        break;
      case '^':
        c = 202;
        break;
      case ':':
        c = 203;
        break;
      default:
        c = 'E';
      }
      break;
    case 'e':
      switch (tmplate[n]) {
      case '`':
        c = 232;
        break;
      case '\'':
        c = 233;
        break;
      case '^':
        c = 234;
        break;
      case ':':
        c = 235;
        break;
      default:
        c = 'e';
      }
      break;
    case 'I':
      switch (tmplate[n]) {
      case '`':
        c = 204;
        break;
      case '\'':
        c = 205;
        break;
      case '^':
        c = 206;
        break;
      case ':':
        c = 207;
        break;
      default:
        c = 'I';
      }
      break;
    case 'i':
      switch (tmplate[n]) {
      case '`':
        c = 236;
        break;
      case '\'':
        c = 237;
        break;
      case '^':
        c = 238;
        break;
      case ':':
        c = 239;
        break;
      default:
        c = 'i';
      }
      break;
    case 'N':
      if (tmplate[n] == '~')
        c = 209;
      else
        c = 'N';
      break;
    case 'n':
      if (tmplate[n] == '~')
        c = 241;
      else
        c = 'n';
      break;
    case 'O':
      switch (tmplate[n]) {
      case '`':
        c = 210;
        break;
      case '\'':
        c = 211;
        break;
      case '^':
        c = 212;
        break;
      case '~':
        c = 213;
        break;
      case ':':
        c = 214;
        break;
      default:
        c = 'O';
      }
      break;
    case 'o':
      switch (tmplate[n]) {
      case '&':
        c = 240;
        break;
      case '`':
        c = 242;
        break;
      case '\'':
        c = 243;
        break;
      case '^':
        c = 244;
        break;
      case '~':
        c = 245;
        break;
      case ':':
        c = 246;
        break;
      default:
        c = 'o';
      }
      break;
    case 'U':
      switch (tmplate[n]) {
      case '`':
        c = 217;
        break;
      case '\'':
        c = 218;
        break;
      case '^':
        c = 219;
        break;
      case ':':
        c = 220;
        break;
      default:
        c = 'U';
      }
      break;
    case 'u':
      switch (tmplate[n]) {
      case '`':
        c = 249;
        break;
      case '\'':
        c = 250;
        break;
      case '^':
        c = 251;
        break;
      case ':':
        c = 252;
        break;
      default:
        c = 'u';
      }
      break;
    case 'Y':
      if (tmplate[n] == '\'')
        c = 221;
      else
        c = 'Y';
      break;
    case 'y':
      if (tmplate[n] == '\'')
        c = 253;
      else if (tmplate[n] == ':')
        c = 255;
      else
        c = 'y';
      break;
    case '?':
      if (tmplate[n] == 'u')
        c = 191;
      else
        c = '?';
      break;
    case '!':
      if (tmplate[n] == 'u')
        c = 161;
      else
        c = '!';
      break;
    case '<':
      if (tmplate[n] == '"')
        c = 171;
      else
        c = '<';
      break;
    case '>':
      if (tmplate[n] == '"')
        c = 187;
      else
        c = '>';
      break;
    case 's':
      if (tmplate[n] == 'B')
        c = 223;
      else
        c = 's';
      break;
    case 'p':
      if (tmplate[n] == '|')
        c = 254;
      else
        c = 'p';
      break;
    case 'P':
      if (tmplate[n] == '|')
        c = 222;
      else
        c = 'P';
      break;
    case 'D':
      if (tmplate[n] == '-')
        c = 208;
      else
        c = 'D';
      break;
    default:
      c = base[n];
    }
    if (char_isprint(c)) {
      if (safe_chr((char) c, buff, bp))
        return 1;
    } else {
      if (safe_chr(base[n], buff, bp))
        return 1;
    }
  }
  return 0;
}

/** Define the args used in APPEND_TO_BUF */
#define APPEND_ARGS size_t len, blen, clen
/** Append a string c to the end of buff, starting at *bp.
 * This macro is used by the safe_XXX functions.
 */
#define APPEND_TO_BUF                                                          \
  /* Trivial cases */                                                          \
  if (c[0] == '\0')                                                            \
    return 0;                                                                  \
  /* The array is at least two characters long here */                         \
  if (c[1] == '\0')                                                            \
    return safe_chr(c[0], buff, bp);                                           \
  len = strlen(c);                                                             \
  blen = *bp - buff;                                                           \
  if (blen > (BUFFER_LEN - 1))                                                 \
    return len;                                                                \
  if ((len + blen) <= (BUFFER_LEN - 1))                                        \
    clen = len;                                                                \
  else                                                                         \
    clen = (BUFFER_LEN - 1) - blen;                                            \
  memcpy(*bp, c, clen);                                                        \
  *bp += clen;                                                                 \
  return len - clen

/** Safely store a formatted string into a buffer.
 * This is a better way to do safe_str(tprintf(fmt,...),buff,bp)
 * \param buff buffer to store formatted string.
 * \param bp pointer to pointer to insertion point in buff.
 * \param fmt format string.
 * \return number of characters left over, or 0 for success.
 */
int
safe_format(char *buff, char **bp, const char *RESTRICT fmt, ...)
{
  APPEND_ARGS;
  char c[BUFFER_LEN];
  va_list args;

  va_start(args, fmt);
  mush_vsnprintf(c, sizeof c, fmt, args);
  va_end(args);

  APPEND_TO_BUF;
}

/** Safely store an integer into a buffer.
 * \param i integer to store.
 * \param buff buffer to store into.
 * \param bp pointer to pointer to insertion point in buff.
 * \return 0 on success, non-zero on failure.
 */
int
safe_integer(intmax_t i, char *buff, char **bp)
{
  return format_long(i, buff, bp, BUFFER_LEN, 10);
}

/** Safely store an unsigned integer into a buffer.
 * \param i integer to store.
 * \param buff buffer to store into.
 * \param bp pointer to pointer to insertion point in buff.
 * \return 0 on success, non-zero on failure.
 */
int
safe_uinteger(uintmax_t i, char *buff, char **bp)
{
  return safe_str(unparse_integer(i), buff, bp);
}

/** Safely store an unsigned integer into a short buffer.
 * \param i integer to store.
 * \param buff buffer to store into.
 * \param bp pointer to pointer to insertion point in buff.
 * \return 0 on success, non-zero on failure.
 */
int
safe_integer_sbuf(intmax_t i, char *buff, char **bp)
{
  return format_long(i, buff, bp, SBUF_LEN, 10);
}

/** Safely store a dbref into a buffer.
 * Don't store partial dbrefs.
 * \param d dbref to store.
 * \param buff buffer to store into.
 * \param bp pointer to pointer to insertion point in buff.
 * \retval 0 success.
 * \retval 1 failure.
 */
int
safe_dbref(dbref d, char *buff, char **bp)
{
  char *saved = *bp;
  if (safe_chr('#', buff, bp)) {
    *bp = saved;
    return 1;
  }
  if (format_long(d, buff, bp, BUFFER_LEN, 10)) {
    *bp = saved;
    return 1;
  }
  return 0;
}

/** Safely store a number into a buffer.
 * \param n number to store.
 * \param buff buffer to store into.
 * \param bp pointer to pointer to insertion point in buff.
 * \retval 0 success.
 * \retval 1 failure.
 */
int
safe_number(NVAL n, char *buff, char **bp)
{
  const char *c;
  APPEND_ARGS;
  c = unparse_number(n);
  APPEND_TO_BUF;
}

/** Safely store a string into a buffer.
 * \param c string to store.
 * \param buff buffer to store into.
 * \param bp pointer to pointer to insertion point in buff.
 * \retval 0 success.
 * \retval 1 failure.
 */
int
safe_str(const char *c, char *buff, char **bp)
{
  APPEND_ARGS;
  if (!c || !*c)
    return 0;
  APPEND_TO_BUF;
}

/** Safely store a Unicode string into a buffer.
 * Actually... is this even needed? I think safe_str() will work.
 *
 * \param s string to store, in UTF-8.
 * \param buff buffer to store into.
 * \param bp pointer to pointer to insertion point in buff.
 * \retval 0 success.
 * \retval 1 failure.
 */
int
safe_utf8(const char *s, char *buff, char **bp)
{
  UBool err = FALSE;
  UChar32 c;
  int offset = *bp - buff;
  int savedoffset = offset;
  int capacity = BUFFER_LEN - 1;
  int soffset = 0;

  U8_NEXT(s, soffset, -1, c);
  while (c > 0) {
    U8_APPEND((uint8_t *) buff, offset, capacity, c, err);
    if (err == TRUE) {
      break;
    }
    U8_NEXT(s, soffset, -1, c);
  }
  if (err == FALSE) {
    *bp += offset - savedoffset;
  }
  return err == TRUE;
}

/** Safely store a string into a buffer, quoting it if it contains a space.
 * \param c string to store.
 * \param buff buffer to store into.
 * \param bp pointer to pointer to insertion point in buff.
 * \retval 0 success.
 * \retval 1 failure.
 */
int
safe_str_space(const char *c, char *buff, char **bp)
{
  APPEND_ARGS;
  char *saved = *bp;

  if (!c || !*c)
    return 0;

  if (strchr(c, ' ')) {
    if (safe_chr('"', buff, bp) || safe_str(c, buff, bp) ||
        safe_chr('"', buff, bp)) {
      *bp = saved;
      return 1;
    }
    return 0;
  } else {
    APPEND_TO_BUF;
  }
}

/** Safely store a string of known length into a buffer
 * This is an optimization of safe_str for when we know the string's length.
 * \param s string to store.
 * \param len length of s.
 * \param buff buffer to store into.
 * \param bp pointer to pointer to insertion point in buff.
 * \retval 0 success.
 * \retval 1 failure.
 */
int
safe_strl(const char *s, size_t len, char *buff, char **bp)
{
  size_t blen, clen;

  if (!s || !*s)
    return 0;
  if (len == 0)
    return 0;
  else if (len == 1)
    return safe_chr(*s, buff, bp);

  blen = *bp - buff;
  if (blen > BUFFER_LEN - 1)
    return len;
  if ((len + blen) <= BUFFER_LEN - 1)
    clen = len;
  else
    clen = BUFFER_LEN - 1 - blen;
  memcpy(*bp, s, clen);
  *bp += clen;
  return len - clen;
}

int
safe_time_t(time_t t, char *buff, char **bp)
{
  return safe_integer((intmax_t) t, buff, bp);
}

/** Safely fill a string with a given character a given number of times.
 * \param x character to fill with.
 * \param n number of copies of character to fill in.
 * \param buff buffer to store into.
 * \param bp pointer to pointer to insertion point in buff.
 * \retval 0 success.
 * \retval 1 failure (filled to end of buffer, but more was requested).
 */
int
safe_fill(char x, size_t n, char *buff, char **bp)
{
  int ret = 0;

  if (n < 1)
    return 0;
  else if (n == 1)
    return safe_chr(x, buff, bp);

  if (*bp + n + 1 > buff + BUFFER_LEN) {
    n = buff + BUFFER_LEN - *bp;
    if (n > 0)
      --n;
    ret = 1;
    if (!n)
      return ret;
  }
  memset(*bp, x, n);
  *bp += n;

  return ret;
}

/** Pad a string, which may contain ANSI, so it contains at least n
 * visible (non-markup) chars.
 * \param x character to pad with
 * \param n desired size of buffer
 * \param buff BUFFER_LEN char array to pad
 * \retval 0 success
 * \param 1 failure (filled to end of buffer but more was requested).
 */
int
safe_fill_to(char x, size_t n, char *buff)
{
  char tmp[BUFFER_LEN];
  char *p = tmp;
  size_t curr;
  int ret = 0;

  curr = ansi_strlen(buff);

  if (n >= BUFFER_LEN)
    n = BUFFER_LEN - 1;

  if (curr >= n)
    return 0;

  if (safe_str(buff, tmp, &p)) {
    *p = '\0';
    return 1;
  }

  ret = safe_fill(x, n - curr, tmp, &p);
  *p = '\0';
  strcpy(buff, tmp);
  return ret;
}

int
safe_hexchar(char c, char *buff, char **bp)
{
  static const char digits[] = "0123456789abcdef";
  if (safe_chr(digits[c >> 4], buff, bp))
    return 1;
  if (safe_chr(digits[c & 0x0F], buff, bp))
    return 1;
  return 0;
}

int
safe_hexstr(uint8_t *bytes, int len, char *buff, char **bp)
{
  int n;

  for (n = 0; n < len; n += 1)
    if (safe_hexchar(bytes[n], buff, bp))
      return 1;

  return 0;
}

#undef APPEND_ARGS
#undef APPEND_TO_BUF

/* skip_space and seek_char are essentially right out of the 2.0 code */

/** Return a pointer to the next non-space character in a string, or NULL.
 * We return NULL if given a null string or a string with only spaces.
 * \param s string to search for non-spaces.
 * \return pointer to next non-space character in s.
 */
char *
skip_space(const char *s)
{
  char *c = (char *) s;
  while (c && *c && isspace(*c))
    c++;
  return c;
}

/** Return a pointer to next char in s which matches c, or to the terminating
 * nul at the end of s.
 * \param s string to search.
 * \param c character to search for.
 * \return pointer to next occurence of c or to the end of s.
 */
char *
seek_char(const char *s, char c)
{
#ifdef HAVE_STRCHRNUL
  return strchrnul(s, c);
#else
  char *p = (char *) s;
  if (!p)
    return NULL;
  while (*p && (*p != c))
    p++;
  return p;
#endif /* HAVE_STRCHRNUL */
}

/** Return a pointer to next char in s which matches c, or to the terminating
 * nul at the end of s.
 * \param s UTF-8 string to search.
 * \param c Unicode character to search for.
 * \return pointer to next occurence of c or to the end of s.
 */
char *
seek_uchar(const char *s, UChar32 c)
{
  int offset = 0;
  UChar32 curr;

  U8_NEXT((const uint8_t *) s, offset, -1, curr);
  while (curr) {
    if (curr == c) {
      return (char *) s + offset;
    }
    U8_NEXT((const uint8_t *) s, offset, -1, curr);
  }
  return (char *) s + offset;
}

/** Search for all copies of old in string, and replace each with newbit.
 * The replaced string is returned, newly allocated.
 * \param old string to find.
 * \param newbit string to replace old with.
 * \param string string to search for old in.
 * \return allocated string with replacements performed.
 */
char *
replace_string(const char *restrict old, const char *restrict newbit,
               const char *restrict string)
{
  char *result, *r;
  size_t len, newlen;

  r = result = mush_malloc_zero(BUFFER_LEN + SSE_OFFSET, "replace_string.buff");
  if (!result)
    mush_panic("Couldn't allocate memory in replace_string!");

  len = strlen(old);
  newlen = strlen(newbit);

  while (*string) {
    char *s = strstr(string, old);
    if (s) { /* Match found! */
      safe_strl(string, s - string, result, &r);
      safe_strl(newbit, newlen, result, &r);
      string = s + len;
    } else {
      safe_str(string, result, &r);
      break;
    }
  }
  *r = '\0';
  return result;
}

/** Standard replacer tokens for text and position */
const char *const standard_tokens[2] = {"##", "#@"};

/* Replace two tokens in a string at once. All-around better than calling
 * replace_string() twice
 */
/** Search for all copies of two old strings, and replace each with a
 * corresponding newbit.
 * The replaced string is returned, newly allocated.
 * \param old array of two strings to find.
 * \param newbits array of two strings to replace old with.
 * \param string string to search for old.
 * \return allocated string with replacements performed.
 */
char *
replace_string2(const char *const old[2], const char *const newbits[2],
                const char *restrict string)
{
  char *result, *rp;
  char firsts[3] = {'\0', '\0', '\0'};
  size_t oldlens[2], newlens[2];

  if (!string)
    return NULL;

  rp = result = mush_malloc(BUFFER_LEN, "replace_string.buff");
  if (!result)
    mush_panic("Couldn't allocate memory in replace_string2!");

  firsts[0] = old[0][0];
  firsts[1] = old[1][0];

  oldlens[0] = strlen(old[0]);
  oldlens[1] = strlen(old[1]);
  newlens[0] = strlen(newbits[0]);
  newlens[1] = strlen(newbits[1]);

  while (*string) {
    size_t skip = strcspn(string, firsts);
    if (skip) {
      safe_strl(string, skip, result, &rp);
      string += skip;
    }
    if (*string) {
      if (strncmp(string, old[0], oldlens[0]) == 0) { /* Copy the first */
        safe_strl(newbits[0], newlens[0], result, &rp);
        string += oldlens[0];
      } else if (strncmp(string, old[1], oldlens[1]) == 0) { /* The second */
        safe_strl(newbits[1], newlens[1], result, &rp);
        string += oldlens[1];
      } else {
        safe_chr(*string, result, &rp);
        string++;
      }
    }
  }

  *rp = '\0';
  return result;
}

/* Copy a string up until a specific character (Or end of string.)
 * Replaces the strcpy()/strchr()/*p=0 pattern.
 * Input and output buffers shouldn't overlap.
 *
 * \param dest buffer to copy into.
 * \param src string to copy from.
 * \param c character to stop at.
 * \return pointer to the start of the string
 */
char *
copy_up_to(char *RESTRICT dest, const char *RESTRICT src, char c)
{
  char *d;

  for (d = dest; *src && *src != c; src++)
    *d++ = *src;

  *d = '\0';

  return dest;
}

/** Given a string and a separator, trim leading and trailing spaces
 * if the separator is a space. This destructively modifies the string.
 * \param str string to trim.
 * \param sep separator character.
 * \return pointer to (trimmed) string.
 */
char *
trim_space_sep(char *str, char sep)
{
  /* Trim leading and trailing spaces if we've got a space separator. */

  char *p;

  if (sep != ' ')
    return str;
  /* Skip leading spaces */
  str += strspn(str, " ");
  for (p = str; *p; p++)
    ;
  /* And trailing */
  for (p--; p > str && *p == ' '; p--)
    ;
  p++;
  *p = '\0';
  return str;
}

/** Find the start of the next token in a string.
 * If the separator is a space, we magically skip multiple spaces.
 * \param str the string.
 * \param sep the token separator character.
 * \return pointer to start of next token in string.
 */
char *
next_token(char *str, char sep)
{
  /* move pointer to start of the next token */

  while (*str) {
    if (*str == sep) {
      break;
    }
    switch (*str) {
    case TAG_START:
      while (*str && *str != TAG_END)
        str++;
      break;
    case ESC_CHAR:
      while (*str && *str != 'm')
        str++;
      break;
    }
    str++;
  }
  if (!*str)
    return NULL;
  str++;
  if (sep == ' ') {
    while (*str == sep)
      str++;
  }
  return str;
}

/** Find the start of the next token in a string.
 * If the separator is a space, we magically skip multiple spaces.
 * \param str the UTF-8 string.
 * \param sep the Unicode token separator character.
 * \return pointer to start of next token in string.
 */
char *
next_utoken(char *str, UChar32 sep)
{
  /* move pointer to start of the next token */
  int offset = 0;
  UChar32 c;

  U8_NEXT(str, offset, -1, c);
  while (c) {
    if (c == sep) {
      break;
    }
    switch (c) {
    case TAG_START:
      while (str[offset] && str[offset] != TAG_END) {
        offset += 1;
      }
      break;
    case ESC_CHAR:
      while (str[offset] && str[offset] != 'm') {
        offset += 1;
      }
      break;
    }
    U8_NEXT(str, offset, -1, c);
  }
  if (!c) {
    return NULL;
  }
  if (sep == ' ') {
    while (c == sep) {
      U8_NEXT(str, offset, -1, c);
    }
  }
  return str + offset;
}

/** Split out the next token from a string, destructively modifying it.
 * As usually, if the separator is a space, we skip multiple spaces.
 * The string's address is updated to be past the token, and the token
 * is returned. This code from TinyMUSH 2.0.
 * \param sp pointer to string to split from.
 * \param sep token separator.
 * \return pointer to token, now null-terminated.
 */
char *
split_token(char **sp, char sep)
{
  char *str, *save;

  save = str = *sp;
  if (!str) {
    *sp = NULL;
    return NULL;
  }
  while (*str) {
    if (*str == sep) {
      break;
    }
    switch (*str) {
    case TAG_START:
      while (*str && *str != TAG_END)
        str++;
      break;
    case ESC_CHAR:
      while (*str && *str != 'm')
        str++;
      break;
    }
    str++;
  }
  if (!*str) {
    str = NULL;
  } else {
    *str++ = '\0';
    if (sep == ' ') {
      while (*str == sep)
        str++;
    }
  }

  *sp = str;
  return save;
}

/** Split out the next token from a string, destructively modifying it.
 * As usually, if the separator is a space, we skip multiple spaces.
 * The string's address is updated to be past the token, and the token
 * is returned.
 * \param sp pointer to UTF-8 string to split from.
 * \param sep Unicode token separator.
 * \return pointer to token, now null-terminated.
 */
char *
split_utoken(char **sp, UChar32 sep)
{
  char *str, *save;
  UChar32 c;
  int offset = 0;

  save = str = *sp;
  if (!str) {
    *sp = NULL;
    return NULL;
  }
  U8_NEXT(str, offset, -1, c);
  while (c) {
    if (c == sep) {
      break;
    }
    switch (c) {
    case TAG_START:
      while (str[offset] && str[offset] != TAG_END) {
        offset += 1;
      }
      break;
    case ESC_CHAR:
      while (str[offset] && str[offset] != 'm') {
        offset += 1;
      }
      break;
    }
    U8_NEXT(str, offset, -1, c);
  }
  if (!c) {
    str = NULL;
  } else {
    int endoffset = offset;
    U8_PREV(str, 0, endoffset, c);
    str[endoffset] = '\0';
    if (sep == ' ') {
      c = ' ';
      while (c == sep) {
        U8_NEXT(str, offset, -1, c);
      }
    }
  }

  *sp = str + offset;
  return save;
}

/** Count the number of tokens in a string.
 * \param str string to count.
 * \param sep token separator.
 * \return number of tokens in str.
 */
int
do_wordcount(char *str, char sep)
{
  int n;

  if (!*str) {
    return 0;
  }
  for (n = 0; str; str = next_token(str, sep), n++) {
  }
  return n;
}

/** Count the number of tokens in a string.
 * \param str UTF-8 string to count.
 * \param sep Unicode token separator.
 * \return number of tokens in str.
 */
int
do_uwordcount(char *str, UChar32 sep)
{
  int n;

  if (!*str) {
    return 0;
  }

  for (n = 0; str; str = next_utoken(str, sep), n++) {
  }
  return n;
}

/** Given a string, a word, and a separator, remove first occurence
 * of the word from the string. Destructive.
 * \param list a string containing a separated list.
 * \param word a word to remove from the list.
 * \param sep the separator between list items.
 * \return pointer to static buffer containing list without first occurence
 * of word.
 */
char *
remove_word(char *list, char *word, char sep)
{
  char *sp;
  char *bp;
  static char buff[BUFFER_LEN];

  bp = buff;
  sp = split_token(&list, sep);
  if (!strcmp(sp, word)) {
    sp = split_token(&list, sep);
    safe_str(sp, buff, &bp);
  } else {
    safe_str(sp, buff, &bp);
    while (list && strcmp(sp = split_token(&list, sep), word)) {
      safe_chr(sep, buff, &bp);
      safe_str(sp, buff, &bp);
    }
  }
  while (list) {
    sp = split_token(&list, sep);
    safe_chr(sep, buff, &bp);
    safe_str(sp, buff, &bp);
  }
  *bp = '\0';
  return buff;
}

/** Given a string, a word, and a separator, remove first occurence
 * of the word from the string. Destructive.
 * \param list a UTF-8 string containing a separated list.
 * \param word a word to remove from the list in UTF-8.
 * \param sep the Unicode separator between list items.
 * \return pointer to newly allocated buffer containing list without first
 * occurence of word. Should be freed with sqlite_free().
 */
char *
remove_uword(char *list, char *word, UChar32 sep)
{
  char *sp;
  char sepstr[5] = {'\0'};
  UBool err = FALSE;
  int seplen = 0;
  sqlite3_str *out = sqlite3_str_new(NULL);

  U8_APPEND((uint8_t *) sepstr, seplen, 5, sep, err);
  if (err == TRUE) {
    return NULL;
  }

  sp = split_utoken(&list, sep);
  if (!strcmp(sp, word)) {
    sp = split_utoken(&list, sep);
    sqlite3_str_appendall(out, sp);
  } else {
    sqlite3_str_appendall(out, sp);
    while (list && strcmp(sp = split_utoken(&list, sep), word)) {
      sqlite3_str_append(out, sepstr, seplen);
      sqlite3_str_appendall(out, sp);
    }
  }
  while (list) {
    sp = split_utoken(&list, sep);
    sqlite3_str_append(out, sepstr, seplen);
    sqlite3_str_appendall(out, sp);
  }
  return sqlite3_str_finish(out);
}

/** Return the next name in a list. A name may be a single word, or
 * a quoted string. This is used by things like page/list. The list's
 * pointer is advanced to the next name in the list.
 * \param head pointer to pointer to string of names.
 * \return pointer to static buffer containing next name.
 */
char *
next_in_list(const char **head)
{
  int paren = 0;
  static char buf[BUFFER_LEN];
  char *p = buf;

  while (**head == ' ')
    (*head)++;

  if (**head == '"') {
    (*head)++;
    paren = 1;
  }

  /* Copy it char by char until you hit a " or, if not in a
   * paren, a space
   */
  while (**head && (paren || (**head != ' ')) && (**head != '"')) {
    safe_chr(**head, buf, &p);
    (*head)++;
  }

  if (paren && **head)
    (*head)++;

  safe_chr('\0', buf, &p);
  return buf;
}

#ifndef HAVE_IMAXDIV_T
typedef struct imaxdiv_t {
  intmax_t rem;
  intmax_t quot;
} imaxdiv_t;
#endif

#ifndef HAVE_IMAXDIV
imaxdiv_t
imaxdiv(intmax_t num, intmax_t denom)
{
  imaxdiv_t r;
  r.quot = num / denom;
  r.rem = num % denom;
  if (num >= 0 && r.rem < 0) {
    r.quot++;
    r.rem -= denom;
  }
  return r;
}
#endif /* !HAVE_IMAXDIV */

/** Safely append an int to a string. Returns a true value on failure.
 * This will someday take extra arguments for use with our version
 * of snprintf. Please try not to use it.
 * maxlen = total length of string.
 * buf[maxlen - 1] = place where \0 will go.
 * buf[maxlen - 2] = last visible character.
 * \param val value to append.
 * \param buff string to append to.
 * \param bp pointer to pointer to insertion point in buff.
 * \param maxlen total length of string.
 * \param base the base to render the number in.
 */
int
format_long(intmax_t val, char *buff, char **bp, int maxlen, int base)
{
  char stack[128]; /* Even a negative 64 bit number will only be 21
                      digits or so max. This should be plenty of
                      buffer room. */
  char *current;
  int size = 0, neg = 0;
  imaxdiv_t r;
  static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

  /* Sanity checks */
  if (!bp || !buff || !*bp)
    return 1;
  if (*bp - buff >= maxlen - 1)
    return 1;

  if (base < 2)
    base = 2;
  else if (base > 36)
    base = 36;

  if (val < 0) {
    neg = 1;
    val = -val;
    if (val < 0) {
/* -LONG_MIN == LONG_MIN on 2's complement systems. Take the
   easy way out since this value is rarely encountered. */

/* Most of these defaults are probably wrong on Win32. I hope it
   has at least the headers from C99. */
#ifndef PRIdMAX
#ifdef WIN32
#define PRIdMAX "I64d"
#else
#define PRIdMAX "lld"
#endif
#endif

#ifndef PRIxMAX
#ifdef WIN32
#define PRIxMAX "I64x"
#else
#define PRIxMAX "llx"
#endif
#endif

#ifndef PRIoMAX
#ifdef WIN32
#define PRIoMAX "I64o"
#else
#define PRIoMAX "llo"
#endif
#endif

      switch (base) {
      case 10:
        return safe_format(buff, bp, "%" PRIdMAX, val);
      case 16:
        return safe_format(buff, bp, "%" PRIxMAX, val);
      case 8:
        return safe_format(buff, bp, "%" PRIoMAX, val);
      default:
        /* Weird base /and/ LONG_MIN. Fix someday. */
        return 0;
      }
    }
  }

  current = stack + sizeof(stack);

  /* Take the rightmost digit, and push it onto the stack, then
   * integer divide by 10 to get to the next digit. */
  r.quot = val;
  do {
    /* ldiv(x, y) does x/y and x%y at the same time (both of
     * which we need).
     */
    r = imaxdiv(r.quot, base);
    *(--current) = digits[r.rem];
  } while (r.quot);

  /* Add the negative sign if needed. */
  if (neg)
    *(--current) = '-';

  /* The above puts the number on the stack.  Now we need to put
   * it in the buffer.  If there's enough room, use Duff's Device
   * for speed, otherwise do it one char at a time.
   */

  size = stack + sizeof(stack) - current;

  if (((int) (*bp - buff)) + size < maxlen - 2) {
    switch (size % 8) {
    case 0:
      while (current < stack + sizeof(stack)) {
        *((*bp)++) = *(current++);
      /* Fall through */
      case 7:
        *((*bp)++) = *(current++);
      /* Fall through */
      case 6:
        *((*bp)++) = *(current++);
      /* Fall through */
      case 5:
        *((*bp)++) = *(current++);
      /* Fall through */
      case 4:
        *((*bp)++) = *(current++);
      /* Fall through */
      case 3:
        *((*bp)++) = *(current++);
      /* Fall through */
      case 2:
        *((*bp)++) = *(current++);
      /* Fall through */
      case 1:
        *((*bp)++) = *(current++);
        /* Fall through */
      }
    }
  } else {
    while (current < stack + sizeof(stack)) {
      if (*bp - buff >= maxlen - 1) {
        return 1;
      }
      *((*bp)++) = *(current++);
    }
  }

  return 0;
}

#ifndef HAVE__STRNCOLL
/** A locale-sensitive strncmp.
 * \param s1 first string to compare.
 * \param s2 second string to compare.
 * \param t number of characters to compare.
 * \retval -1 s1 collates before s2.
 * \retval 0 s1 collates the same as s2.
 * \retval 1 s1 collates after s2.
 */
int
strncoll(const char *s1, const char *s2, size_t t)
{
#ifdef HAVE_STRXFRM
  char *d1, *d2, *ns1, *ns2;
  int result;
  size_t s1_len, s2_len;

  ns1 = mush_malloc(t + 1, "string");
  ns2 = mush_malloc(t + 1, "string");
  memcpy(ns1, s1, t);
  ns1[t] = '\0';
  memcpy(ns2, s2, t);
  ns2[t] = '\0';
  s1_len = strxfrm(NULL, ns1, 0) + 1;
  s2_len = strxfrm(NULL, ns2, 0) + 1;

  d1 = mush_malloc(s1_len + 1, "string");
  d2 = mush_malloc(s2_len + 1, "string");
  (void) strxfrm(d1, ns1, s1_len);
  (void) strxfrm(d2, ns2, s2_len);
  result = strcmp(d1, d2);
  mush_free(ns1, "string");
  mush_free(ns2, "string");
  mush_free(d1, "string");
  mush_free(d2, "string");
  return result;
#else
  return strncmp(s1, s2, t);
#endif
}
#endif

#ifndef HAVE__STRICOLL
/** A locale-sensitive strcasecmp.
 * \param s1 first string to compare.
 * \param s2 second string to compare.
 * \retval -1 s1 collates before s2.
 * \retval 0 s1 collates the same as s2.
 * \retval 1 s1 collates after s2.
 */
int
strcasecoll(const char *s1, const char *s2)
{
#ifdef HAVE_STRXFRM
  char *d1, *d2;
  int result;
  size_t s1_len, s2_len;

  s1_len = strxfrm(NULL, s1, 0) + 1;
  s2_len = strxfrm(NULL, s2, 0) + 1;

  d1 = mush_malloc(s1_len, "string");
  d2 = mush_malloc(s2_len, "string");
  (void) strxfrm(d1, strupper(s1), s1_len);
  (void) strxfrm(d2, strupper(s2), s2_len);
  result = strcmp(d1, d2);
  mush_free(d1, "string");
  mush_free(d2, "string");
  return result;
#else
  return strcasecmp(s1, s2);
#endif
}
#endif

#ifndef HAVE__STRNICOLL
/** A locale-sensitive strncasecmp.
 * \param s1 first string to compare.
 * \param s2 second string to compare.
 * \param t number of characters to compare.
 * \retval -1 s1 collates before s2.
 * \retval 0 s1 collates the same as s2.
 * \retval 1 s1 collates after s2.
 */
int
strncasecoll(const char *s1, const char *s2, size_t t)
{
#ifdef HAVE_STRXFRM
  char *d1, *d2, *ns1, *ns2;
  int result;
  size_t s1_len, s2_len;

  ns1 = mush_malloc(t + 1, "string");
  ns2 = mush_malloc(t + 1, "string");
  memcpy(ns1, s1, t);
  ns1[t] = '\0';
  memcpy(ns2, s2, t);
  ns2[t] = '\0';
  s1_len = strxfrm(NULL, ns1, 0) + 1;
  s2_len = strxfrm(NULL, ns2, 0) + 1;

  d1 = mush_malloc(s1_len, "string");
  d2 = mush_malloc(s2_len, "string");
  (void) strxfrm(d1, strupper(ns1), s1_len);
  (void) strxfrm(d2, strupper(ns2), s2_len);
  result = strcmp(d1, d2);
  mush_free(ns1, "string");
  mush_free(ns2, "string");
  mush_free(d1, "string");
  mush_free(d2, "string");
  return result;
#else
  return strncasecmp(s1, s2, t);
#endif
}
#endif

/** Safe version of strncpy() that always nul-terminates the
 * destination string. The only reason it's not called
 * safe_strncpy() is to avoid confusion with the unrelated
 * safe_*() pennstr functions.
 * \param dst the destination string to copy to
 * \param src the source string to copy from
 * \param len the length of dst. At most len-1 bytes will be copied from src
 * \return dst
 */
char *
mush_strncpy(char *restrict dst, const char *restrict src, size_t len)
{
  size_t n = 0;
  char *start = dst;

  if (!src || !dst || len == 0)
    return dst;

  len--;

  while (*src && n < len) {
    *dst++ = *src++;
    n++;
  }

  *dst = '\0';
  return start;
}

/** Safely append a list item to a buffer, possibly with punctuation
 * and conjunctions.
 * Given the current item number in a list, whether it's the last item
 * in the list, the list's output separator, a conjunction,
 * and a punctuation mark to use between items, store the appropriate
 * inter-item stuff into the given buffer safely.
 * \param cur_num current item number of the item to append.
 * \param done 1 if this is the final item.
 * \param delim string to insert after most items (comma).
 * \param conjoin string to insert before last time ("and").
 * \param space output delimiter.
 * \param buff buffer to append to.
 * \param bp pointer to pointer to insertion point in buff.
 */
void
safe_itemizer(int cur_num, int done, const char *delim, const char *conjoin,
              const char *space, char *buff, char **bp)
{
  /* We don't do anything if it's the first one */
  if (cur_num == 1)
    return;
  /* Are we done? */
  if (done) {
    /* if so, we need a [<delim>]<space><conj> */
    if (cur_num >= 3)
      safe_str(delim, buff, bp);
    safe_str(space, buff, bp);
    safe_str(conjoin, buff, bp);
  } else {
    /* if not, we need just <delim> */
    safe_str(delim, buff, bp);
  }
  /* And then we need another <space> */
  safe_str(space, buff, bp);
}

/** Return a stringified time in a static buffer
 * Just like ctime() except without the trailing newlines.
 * \param t the time to format.
 * \param utc true if the time should be displayed in UTC, 0 for local time
 * zone.
 * \return a pointer to a static buffer with the stringified time.
 */
char *
show_time(time_t t, bool utc)
{
  struct tm *when;

  if (utc)
    when = gmtime(&t);
  else
    when = localtime(&t);

  return show_tm(when);
}

/** Return a stringified time in a static buffer
 * Just like asctime() except without the trailing newlines.
 * \param when the time to format.
 * \return a pointer to a static buffer with the stringified time.
 */
char *
show_tm(struct tm *when)
{
  static char buffer[BUFFER_LEN];
  int p;

  if (!when)
    return NULL;

  strcpy(buffer, asctime(when));

  p = strlen(buffer) - 1;
  if (buffer[p] == '\n')
    buffer[p] = '\0';

  if (buffer[8] == ' ')
    buffer[8] = '0';

  return buffer;
}

/** Return a default pcre_extra pointer pointing to a static region
    set up to use a fairly low match-limit setting.
*/
pcre_extra *
default_match_limit(void)
{
  static pcre_extra ex;
  memset(&ex, 0, sizeof ex);
  set_match_limit(&ex);
  return &ex;
}

/** Set a low match-limit setting in an existing pcre_extra struct. */
void
set_match_limit(pcre_extra *ex)
{
  if (!ex)
    return;
  ex->flags |= PCRE_EXTRA_MATCH_LIMIT;
  ex->match_limit = PENN_MATCH_LIMIT;
}

/** Destructively gets rid of trailing whitespace in a string.
 * \param buff the string to transform.
 * \param len the length of the string.
 * \return the new length of the string
 */
size_t
remove_trailing_whitespace(char *buff, size_t len)
{
  char *c;

  for (c = buff + len - 1; c >= buff; c -= 1) {
    if (isspace(*c)) {
      len -= 1;
      *c = '\0';
    } else
      break;
  }
  return len;
}

/** Append an ASCII character to the end of a BUFFER_LEN long string.
 *
 * \param c the character to append.
 * \param buff the start of the buffer
 * \param bp pointer to the current location in the buffer
 * \return 0 on success, 1 on failure
 */
int
safe_chr(char c, char *buff, char **bp)
{
  if ((*bp - buff) >= (BUFFER_LEN - 1))
    return 1;
  else {
    *(*bp)++ = c;
    return 0;
  }
}

/** Append a Unicode character to the end of a BUFFER_LEN long UTF-8
 * string.
 *
 * \param c the character to append.
 * \param buff the start of the buffer
 * \param bp pointer to the current location in the buffer
 * \return 0 on success, 1 on failure
 */
int
safe_uchar(UChar32 c, char *buff, char **bp)
{
  UBool err = FALSE;
  int capacity = BUFFER_LEN - 1;
  int offset = *bp - buff;
  int savedoffset = offset;

  U8_APPEND((uint8_t *) buff, offset, capacity, c, err);
  *bp += offset - savedoffset;
  return err == TRUE;
}

/* keystr format:
 *
 * Either a single word, which is returned for any keyword Or one or
 * more key:value pairs, in which case the matching value is
 * returned. If none are found, looks for a key named default.  If
 * that's missing too, returns the deflt argument.
 */

extern const unsigned char *tables;

const char *
keystr_find_full(const char *restrict map, const char *restrict key,
                 const char *restrict deflt, char delim)
{
  pcre *re;
  int erroffset;
  const char *errptr;
  int offsets[33];
  int matches;
  static char tbuf[BUFFER_LEN];
  char pattern[BUFFER_LEN], *pp;

  if (!strchr(map, ' ') && !strchr(map, delim))
    return map;

  pp = pattern;
  safe_format(pattern, &pp, "\\b\\Q%s%c\\E(\\w+)\\b", key, delim);
  *pp = '\0';

  if (!(re = pcre_compile(pattern, PCRE_CASELESS, &errptr, &erroffset, tables)))
    return deflt;

  matches = pcre_exec(re, NULL, map, strlen(map), 0, 0, offsets, 33);
  pcre_free(re);

  if (matches == 2) {
    pcre_copy_substring(map, offsets, matches, 1, tbuf, BUFFER_LEN);
    return tbuf;
  } else if (strcmp(key, "default") == 0)
    return deflt;
  else
    return keystr_find_full(map, "default", deflt, delim);
}

/** Convert a MUSH-style wildcard pattern using * to a SQL wildcard pattern
 * using %.
 *
 * \param orig the string to convert in UTF-8.
 * \param esc the character to escape special ones (_%) with.
 * \param len the length of the returned string, not counting the trailing nul.
 * \return a newly allocated string that should be freed with sqlite3_free()
 */
char *
glob_to_like(const char *orig, UChar32 esc, int *len)
{
  char escbuf[5] = {'\0'};
  char esclen = 0;
  UChar32 c;
  int offset = 0;
  sqlite3_str *out = sqlite3_str_new(NULL);

  U8_APPEND_UNSAFE(escbuf, esclen, esc);
  U8_NEXT(orig, offset, -1, c);
  while (c) {
    if (c == '%' || c == '_' || c == esc) {
      sqlite3_str_append(out, escbuf, esclen);
      sqlite3_str_appenduchar(out, c);
    } else if (c == '\\') {
      U8_NEXT(orig, offset, -1, c);
      if (out) {
        sqlite3_str_append(out, escbuf, esclen);
        sqlite3_str_appenduchar(out, c);
      } else {
        break;
      }
    } else if (c == '*') {
      sqlite3_str_appendchar(out, 1, '%');
    } else if (c == '?') {
      sqlite3_str_appendchar(out, 1, '_');
    } else {
      sqlite3_str_appenduchar(out, c);
    }
    U8_NEXT(orig, offset, -1, c);
  }
  if (len) {
    *len = sqlite3_str_length(out);
  }
  return sqlite3_str_finish(out);
}

/** Escape SQL like wildcards from a string.
 *
 * \param orig the string to escape, in UTF-8.
 * \param esc the Unicode character to escape special ones (_%) with.
 * \param len the length of the returned string, not counting the trailing nul.
 * \return a newly allocated string that should be freed with sqlite3_free().
 */
char *
escape_like(const char *orig, UChar32 esc, int *len)
{
  char escbuf[5] = {'\0'};
  int esclen = 0, offset = 0;
  sqlite3_str *out = sqlite3_str_new(NULL);
  UChar32 c;

  U8_APPEND_UNSAFE(escbuf, esclen, esc);
  U8_NEXT(orig, offset, -1, c);
  while (c) {
    if (c == '%' || c == '_' || c == esc) {
      sqlite3_str_append(out, escbuf, esclen);
      sqlite3_str_appenduchar(out, c);
    } else if (c == '\\') {
      U8_NEXT(orig, offset, -1, c);
      if (out) {
        sqlite3_str_append(out, escbuf, esclen);
        sqlite3_str_appenduchar(out, c);
      } else {
        break;
      }
    } else {
      sqlite3_str_appenduchar(out, c);
    }
    U8_NEXT(orig, offset, -1, c);
  }

  if (len) {
    *len = sqlite3_str_length(out);
  }

  return sqlite3_str_finish(out);
}

/** Append a Unicode character to a sqlite3 string builder
 *
 * \param str the string builder
 * \param c the Unicode character to append.
 */
void
sqlite3_str_appenduchar(sqlite3_str *str, UChar32 c)
{
  char cbuf[5] = {'\0'};
  int clen = 0;

  U8_APPEND_UNSAFE(cbuf, clen, c);
  sqlite3_str_append(str, cbuf, clen);
}

/* Exists solely for giving a maximum length to pennstr buffers */
static sqlite3 *ps_db = NULL;

/** Return a newly allocated pennstr */
pennstr *
ps_new(void)
{
  if (!ps_db) {
    if (sqlite3_open_v2(":memory:", &ps_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        NULL) == SQLITE_OK) {
      /* Increase this later and/or make it a configure option */
      sqlite3_limit(ps_db, SQLITE_LIMIT_LENGTH, BUFFER_LEN);
    }
  }

  return sqlite3_str_new(ps_db);
}

/** Append a floating point number to a pennstr */
void
ps_safe_number(pennstr *ps, NVAL n)
{
  /* TODO: Improve me */
  sqlite3_str_appendf(ps, "%s", unparse_number(n));
}

/** Applies printf-like formatting to its arguments and appends the
 * result to a pennstr. The accepted format is slightly different from
 * normal - see https://www.sqlite.org/printf.html for details. */
void
ps_safe_format(pennstr *ps, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  sqlite3_str_vappendf(ps, fmt, args);
  va_end(args);
}

/** Appends up to N codepoints from a valid UTF-8 string to the pennstr */
void
ps_safe_strl_cp(pennstr *ps, const char *s, int n)
{
  UChar32 c;
  int offset = 0;

  if (n <= 0) {
    return;
  }

  do {
    U8_NEXT(s, offset, -1, c);
  } while (c && --n > 0);

  sqlite3_str_append(ps, s, offset);
}

/** Free a pennstr */
void
ps_free(pennstr *ps)
{
  char *s = sqlite3_str_finish(ps);
  sqlite3_free(s);
}

/** Free the pennstr and return its string. This string must be freed with
 * ps_free_str() when done. */
char *
ps_finish(pennstr *ps)
{
  char *s = sqlite3_str_finish(ps);
  if (s) {
    ADD_CHECK("pennstring");
  }
  return s;
}

/** Free a string returned by ps_finish() */
void
ps_free_str(char *s)
{
  if (s) {
    DEL_CHECK("pennstring");
    sqlite3_free(s);
  }
}

/** Iterate over a UTF-8 string calling a function for each codepoint.
 *
 * \param s the UTF-8 string to walk.
 * \param fun the callback function
 * \param data user-supplied data pointer.
 * \return true if the entire string was iterated, false if it quit early.
 */
bool
for_each_cp(const char *s, cp_callback fun, void *data)
{
  UChar32 c;
  int offset = 0, prev_offset = 0;

  U8_NEXT(s, offset, -1, c);
  while (c) {
    if (!fun(c, s, prev_offset, offset - prev_offset, data)) {
      return 0;
    }
    prev_offset = offset;
    U8_NEXT(s, offset, -1, c);
  }
  return 1;
}

/** Return the number of codepoints in a UTF-8 string */
int
strlen_cp(const char *s)
{
  int n = 0;
  int offset = 0;
  while (s[offset]) {
    n += 1;
    U8_FWD_1(s, offset, -1);
  }
  return n;
}

/** Return the first codepoint in a UTF-8 string
 *
 * \param s the UTF-8 string
 * \param len If non-NULL, set to the number of bytes taken up by the codepoint.
 * \return the codepoint, negative on an invalid byte sequence.
 */
UChar32
first_cp(const char *s, int *len)
{
  UChar32 c;
  int offset = 0;

  U8_NEXT(s, offset, -1, c);
  if (len) {
    *len = offset;
  }
  return c;
}
