/* pseudo-reloc.c

   Contributed by Egor Duda  <deo@logos-m.ru>
   Modified by addition of runtime_pseudo_reloc version 2
   by Kai Tietz  <kai.tietz@onevision.com>
	
   THIS SOFTWARE IS NOT COPYRIGHTED

   This source code is offered for use in the public domain. You may
   use, modify or distribute it freely.

   This code is distributed in the hope that it will be useful but
   WITHOUT ANY WARRANTY. ALL WARRENTIES, EXPRESS OR IMPLIED ARE HEREBY
   DISCLAMED. This includes but is not limited to warrenties of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
*/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <memory.h>

#if defined(__CYGWIN__)
#include <wchar.h>
#include <ntdef.h>
#include <sys/cygwin.h>
/* copied from winsup.h */
# define NO_COPY __attribute__((nocommon)) __attribute__((section(".data_cygwin_nocopy")))
/* custom status code: */
#define STATUS_ILLEGAL_DLL_PSEUDO_RELOCATION ((NTSTATUS) 0xe0000269)
#define SHORT_MSG_BUF_SZ 128
#else
# define NO_COPY
#endif

#ifdef __GNUC__
#define ATTRIBUTE_NORETURN __attribute__ ((noreturn))
#else
#define ATTRIBUTE_NORETURN
#endif

#ifndef __MINGW_LSYMBOL
#define __MINGW_LSYMBOL(sym) sym
#endif

extern char __RUNTIME_PSEUDO_RELOC_LIST__;
extern char __RUNTIME_PSEUDO_RELOC_LIST_END__;
extern char __MINGW_LSYMBOL(_image_base__);

void _pei386_runtime_relocator (void);

/* v1 relocation is basically:
 *   *(base + .target) += .addend
 * where (base + .target) is always assumed to point
 * to a DWORD (4 bytes).
 */
typedef struct {
  DWORD addend;
  DWORD target;
} runtime_pseudo_reloc_item_v1;

/* v2 relocation is more complex. In effect, it is
 *    *(base + .target) += *(base + .sym) - (base + .sym)
 * with care taken in both reading, sign extension, and writing
 * because .flags may indicate that (base + .target) may point
 * to a BYTE, WORD, DWORD, or QWORD (w64).
 */
typedef struct {
  DWORD sym;
  DWORD target;
  DWORD flags;
} runtime_pseudo_reloc_item_v2;

typedef struct {
  DWORD magic1;
  DWORD magic2;
  DWORD version;
} runtime_pseudo_reloc_v2;

static void ATTRIBUTE_NORETURN
__report_error (const char *msg, ...)
{
#ifdef __CYGWIN__
  /* This function is used to print short error messages
   * to stderr, which may occur during DLL initialization
   * while fixing up 'pseudo' relocations. This early, we
   * may not be able to use cygwin stdio functions, so we
   * use the win32 WriteFile api. This should work with both
   * normal win32 console IO handles, redirected ones, and
   * cygwin ptys.
   */
  char buf[SHORT_MSG_BUF_SZ];
  char module[MAX_PATH];
  char posix_module[MAX_PATH];
  static const char   UNKNOWN_MODULE[] = "<unknown module>: ";
  static const size_t UNKNOWN_MODULE_LEN = sizeof (UNKNOWN_MODULE) - 1;
  static const char   CYGWIN_FAILURE_MSG[] = "Cygwin runtime failure: ";
  static const size_t CYGWIN_FAILURE_MSG_LEN = sizeof (CYGWIN_FAILURE_MSG) - 1;
  DWORD len;
  DWORD done;
  va_list args;
  HANDLE errh = GetStdHandle (STD_ERROR_HANDLE);
  ssize_t modulelen = GetModuleFileNameA (NULL, module, sizeof (module));

  if (errh == INVALID_HANDLE_VALUE)
    cygwin_internal (CW_EXIT_PROCESS,
                     STATUS_ILLEGAL_DLL_PSEUDO_RELOCATION,
                     1);

  if (modulelen > 0)
    if (!cygwin_conv_to_posix_path (module, posix_module))
      posix_module[0] = '\0';

  va_start (args, msg);
  len = (DWORD) vsnprintf (buf, SHORT_MSG_BUF_SZ, msg, args);
  va_end (args);
  buf[SHORT_MSG_BUF_SZ-1] = '\0'; /* paranoia */

  if (posix_module[0])
    {
      WriteFile (errh, (PCVOID)CYGWIN_FAILURE_MSG,
                 CYGWIN_FAILURE_MSG_LEN, &done, NULL);
      WriteFile (errh, (PCVOID)posix_module,
                 strlen(posix_module), &done, NULL);
      WriteFile (errh, (PCVOID)": ", 2, &done, NULL);
      WriteFile (errh, (PCVOID)buf, len, &done, NULL);
    }
  else
    {
      WriteFile (errh, (PCVOID)CYGWIN_FAILURE_MSG,
                 CYGWIN_FAILURE_MSG_LEN, &done, NULL);
      WriteFile (errh, (PCVOID)UNKNOWN_MODULE,
                 UNKNOWN_MODULE_LEN, &done, NULL);
      WriteFile (errh, (PCVOID)buf, len, &done, NULL);
    }
  WriteFile (errh, (PCVOID)"\n", 1, &done, NULL);

  cygwin_internal (CW_EXIT_PROCESS,
                   STATUS_ILLEGAL_DLL_PSEUDO_RELOCATION,
                   1);
  /* not reached, but silences noreturn warning */
  abort ();
#else
  va_list argp;
  va_start (argp, msg);
# ifdef __MINGW64_VERSION_MAJOR
  fprintf (stderr, "Mingw-w64 runtime failure:\n");
# else
  fprintf (stderr, "Mingw runtime failure:\n");
# endif
  vfprintf (stderr, msg, argp);
  va_end (argp);
  abort ();
#endif
}

/* This function temporarily marks the page containing addr
 * writable, before copying len bytes from *src to *addr, and
 * then restores the original protection settings to the page.
 *
 * Using this function eliminates the requirement with older
 * pseudo-reloc implementations, that sections containing
 * pseudo-relocs (such as .text and .rdata) be permanently
 * marked writable. This older behavior sabotaged any memory
 * savings achieved by shared libraries on win32 -- and was
 * slower, too.  However, on cygwin as of binutils 2.20 the
 * .text section is still marked writable, and the .rdata section
 * is folded into the (writable) .data when --enable-auto-import.
 */
static void
__write_memory (void *addr, const void *src, size_t len)
{
  MEMORY_BASIC_INFORMATION b;
  DWORD oldprot;

  if (!len)
    return;

  if (!VirtualQuery (addr, &b, sizeof(b)))
    {
      __report_error ("  VirtualQuery failed for %d bytes at address %p",
		      (int) sizeof(b), addr);
    }

  /* Temporarily allow write access to read-only protected memory.  */
  if (b.Protect != PAGE_EXECUTE_READWRITE && b.Protect != PAGE_READWRITE)
    VirtualProtect (b.BaseAddress, b.RegionSize, PAGE_EXECUTE_READWRITE,
		  &oldprot);
  /* write the data. */
  memcpy (addr, src, len);
  /* Restore original protection. */
  if (b.Protect != PAGE_EXECUTE_READWRITE && b.Protect != PAGE_READWRITE)
    VirtualProtect (b.BaseAddress, b.RegionSize, oldprot, &oldprot);
}

#define RP_VERSION_V1 0
#define RP_VERSION_V2 1

static void
do_pseudo_reloc (void * start, void * end, void * base)
{
  ptrdiff_t addr_imp, reldata;
  ptrdiff_t reloc_target = (ptrdiff_t) ((char *)end - (char*)start);
  runtime_pseudo_reloc_v2 *v2_hdr = (runtime_pseudo_reloc_v2 *) start;
  runtime_pseudo_reloc_item_v2 *r;

  /* A valid relocation list will contain at least one entry, and
   * one v1 data structure (the smallest one) requires two DWORDs.
   * So, if the relocation list is smaller than 8 bytes, bail.
   */
  if (reloc_target < 8)
    return;

  /* Check if this is the old pseudo relocation version.  */
  /* There are two kinds of v1 relocation lists:
   *   1) With a (v2-style) version header. In this case, the
   *      first entry in the list is a 3-DWORD structure, with
   *      value:
   *         { 0, 0, RP_VERSION_V1 }
   *      In this case, we skip to the next entry in the list,
   *      knowing that all elements after the head item can
   *      be cast to runtime_pseudo_reloc_item_v1.
   *   2) Without a (v2-style) version header. In this case, the
   *      first element in the list IS an actual v1 relocation
   *      record, which is two DWORDs.  Because there will never
   *      be a case where a v1 relocation record has both
   *      addend == 0 and target == 0, this case will not be
   *      confused with the prior one.
   * All current binutils, when generating a v1 relocation list,
   * use the second (e.g. original) form -- that is, without the
   * v2-style version header.
   */
  if (reloc_target >= 12
      && v2_hdr->magic1 == 0 && v2_hdr->magic2 == 0
      && v2_hdr->version == RP_VERSION_V1)
    {
      /* We have a list header item indicating that the rest
       * of the list contains v1 entries.  Move the pointer to
       * the first true v1 relocation record.  By definition,
       * that v1 element will not have both addend == 0 and
       * target == 0 (and thus, when interpreted as a
       * runtime_pseudo_reloc_v2, it will not have both
       * magic1 == 0 and magic2 == 0).
       */
      v2_hdr++;
    }

  if (v2_hdr->magic1 != 0 || v2_hdr->magic2 != 0)
    {
      /*************************
       * Handle v1 relocations *
       *************************/
      runtime_pseudo_reloc_item_v1 * o;
      for (o = (runtime_pseudo_reloc_item_v1 *) v2_hdr;
	   o < (runtime_pseudo_reloc_item_v1 *)end;
           o++)
	{
	  DWORD newval;
	  reloc_target = (ptrdiff_t) base + o->target;
	  newval = (*((DWORD*) reloc_target)) + o->addend;
	  __write_memory ((void *) reloc_target, &newval, sizeof(DWORD));
	}
      return;
    }

  /* If we got this far, then we have relocations of version 2 or newer */

  /* Check if this is a known version.  */
  if (v2_hdr->version != RP_VERSION_V2)
    {
      __report_error ("  Unknown pseudo relocation protocol version %d.\n",
		      (int) v2_hdr->version);
      return;
    }

  /*************************
   * Handle v2 relocations *
   *************************/

  /* Walk over header. */
  r = (runtime_pseudo_reloc_item_v2 *) &v2_hdr[1];

  for (; r < (runtime_pseudo_reloc_item_v2 *) end; r++)
    {
      /* location where new address will be written */
      reloc_target = (ptrdiff_t) base + r->target;

      /* get sym pointer. It points either to the iat entry
       * of the referenced element, or to the stub function.
       */
      addr_imp = (ptrdiff_t) base + r->sym;
      addr_imp = *((ptrdiff_t *) addr_imp);

      /* read existing relocation value from image, casting to the
       * bitsize indicated by the 8 LSBs of flags. If the value is
       * negative, manually sign-extend to ptrdiff_t width. Raise an
       * error if the bitsize indicated by the 8 LSBs of flags is not
       * supported.
       */
      switch ((r->flags & 0xff))
        {
          case 8:
	    reldata = (ptrdiff_t) (*((unsigned char *)reloc_target));
	    if ((reldata & 0x80) != 0)
	      reldata |= ~((ptrdiff_t) 0xff);
	    break;
	  case 16:
	    reldata = (ptrdiff_t) (*((unsigned short *)reloc_target));
	    if ((reldata & 0x8000) != 0)
	      reldata |= ~((ptrdiff_t) 0xffff);
	    break;
	  case 32:
	    reldata = (ptrdiff_t) (*((unsigned int *)reloc_target));
#ifdef _WIN64
	    if ((reldata & 0x80000000) != 0)
	      reldata |= ~((ptrdiff_t) 0xffffffff);
#endif
	    break;
#ifdef _WIN64
	  case 64:
	    reldata = (ptrdiff_t) (*((unsigned long long *)reloc_target));
	    break;
#endif
	  default:
	    reldata=0;
	    __report_error ("  Unknown pseudo relocation bit size %d.\n",
		    (int) (r->flags & 0xff));
	    break;
        }

      /* Adjust the relocation value */
      reldata -= ((ptrdiff_t) base + r->sym);
      reldata += addr_imp;

      /* Write the new relocation value back to *reloc_target */
      switch ((r->flags & 0xff))
	{
         case 8:
           __write_memory ((void *) reloc_target, &reldata, 1);
	   break;
	 case 16:
           __write_memory ((void *) reloc_target, &reldata, 2);
	   break;
	 case 32:
           __write_memory ((void *) reloc_target, &reldata, 4);
	   break;
#ifdef _WIN64
	 case 64:
           __write_memory ((void *) reloc_target, &reldata, 8);
	   break;
#endif
	}
     }
}

void
_pei386_runtime_relocator (void)
{
  static NO_COPY int was_init = 0;
  if (was_init)
    return;
  ++was_init;
  do_pseudo_reloc (&__RUNTIME_PSEUDO_RELOC_LIST__,
		   &__RUNTIME_PSEUDO_RELOC_LIST_END__,
		   &__MINGW_LSYMBOL(_image_base__));
}
