/* KallistiOS ##version##

   kos/gmon.h
   Copyright (C) 2024 Andy Barajas

*/

/** \file    kos/gmon.h
    \brief   GPROF support.
    \ingroup debugging_gprof

    This file provides utilities for profiling applications using gprof.

    \author Andy Barajas
*/

#ifndef __KOS_GMON_H
#define __KOS_GMON_H

#include <sys/cdefs.h>
__BEGIN_DECLS

#include <stdbool.h>

/** \defgroup debugging_gprof GPROF
    \brief    Utilities for GPROF
    \ingroup  debugging

    @{
*/

/** \brief  Environment variable for setting the gmon output file prefix.

    This variable allows you to set a custom prefix for the gmon output file
    generated after profiling. The default output filename is gmon.out.
    If set, the specified prefix will be appended with the PID of the 
    running program, which is always 1 for Dreamcast.

    Example:
    \code
    setenv(GMON_OUT_PREFIX, "/pc/test.out", 1);
    \endcode
    This will generate a file called \c test.out.1 after profiling is complete.
*/
#define GMON_OUT_PREFIX   "GMON_OUT_PREFIX"

/** \brief  Restart or stop gprof profiling.

    This function restarts or stops gprof profiling. It does NOT start gprof 
    profiling initially, as gprof profiling starts before the program enters 
    the main function. You can use this function to stop profiling and then 
    restart it later when you reach the section of code you want to profile.

    \param  enable            A boolean value to restart (true) or stop (false) 
                              gprof profiling.
*/
void moncontrol(bool enable);

/** @} */

__END_DECLS

#endif  /* __KOS_GMON_H */
