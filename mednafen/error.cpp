/******************************************************************************/
/* Mednafen - Multi-system Emulator                                           */
/******************************************************************************/
/* error.cpp:
**  Copyright (C) 2007-2016 Mednafen Team
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "mednafen.h"
#include "error.h"

namespace Mednafen
{

MDFN_Error::MDFN_Error()
{
 abort();
}

MDFN_Error::MDFN_Error(int errno_code_new, const char *fmt, ...)
{
#ifdef VITA
 va_list list;
 static char string[512];

 va_start(list, fmt);
 vsprintf(string, fmt, list);
 va_end(list);

 printf("[ERROR %X] %s: %s\n", errno_code_new, string);
#endif
 errno_code = errno_code_new;
}


MDFN_Error::MDFN_Error(const ErrnoHolder &enh)
{
#ifdef VITA
 printf("[ERROR2 %X]\n", enh.Errno());
#endif
 errno_code = enh.Errno();
}


MDFN_Error::~MDFN_Error()
{
}

MDFN_Error::MDFN_Error(const MDFN_Error &ze_error)
{
#ifdef VITA
 printf("[ERROR3 %X]\n", ze_error.errno_code);
#endif
 errno_code = ze_error.errno_code;
}

MDFN_Error& MDFN_Error::operator=(const MDFN_Error &ze_error)
{
 int new_errno_code = ze_error.errno_code;

 errno_code = new_errno_code;

 return(*this);
}


int MDFN_Error::GetErrno(void)
{
 return(errno_code);
}

static MDFN_NOWARN_UNUSED const char *srr_wrap(int ret, const char *local_strerror)
{
 if(ret == -1)
  return("ERROR IN strerror_r()!!!");

 return(local_strerror);
}

static MDFN_NOWARN_UNUSED const char *srr_wrap(const char *ret, const char *local_strerror)
{
 if(ret == NULL)
  return("ERROR IN strerror_r()!!!");

 return(ret);
}

void ErrnoHolder::SetErrno(int the_errno)
{
 local_errno = the_errno;

 if(the_errno == 0)
  local_strerror[0] = 0;
 else
 {
  #ifdef HAVE_STRERROR_R
   const char *retv;

   retv = srr_wrap(strerror_r(the_errno, local_strerror, 256), local_strerror);

   if(retv != local_strerror)
    strncpy(local_strerror, retv, 255);

  #else	// No strerror_r :(

   strncpy(local_strerror, strerror(the_errno), 255);

  #endif

  local_strerror[255] = 0;
 }
}

}
