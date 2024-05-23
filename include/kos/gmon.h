/* KallistiOS ##version##

   kos/gmon.h
   Copyright (C) 2024 Andy Barajas

*/

/** \file    kos/gmon.h
    \brief   Code to help you modify gprof behavior.
    \ingroup debugging

    \author Andy Barajas
*/

#ifndef __KOS_GMON_H
#define __KOS_GMON_H

#include <sys/cdefs.h>
__BEGIN_DECLS

#include <stdbool.h>

#define GMON_OUT_PREFIX "GMON_OUT_PREFIX"

/** \addtogroup debugging
    @{
*/

/** \brief  Enable or disable gproff profiling.

    This function enables/disable gprof profiling.

    \param  enable            Enable/disable gprof profiling.
*/
void moncontrol(bool enable);

/** @} */

__END_DECLS

#endif  /* __KOS_GMON_H */


