/* KallistiOS ##version##

   sys/gmon.h
   Copyright (C) 2024 Andy Barajas

*/

#ifndef __SYS_GMON_H
#define __SYS_GMON_H

#include <sys/cdefs.h>
__BEGIN_DECLS

#include <stdint.h>
#include <stdbool.h>

/** \defgroup debugging_gprof GPROF
    \brief    GPROF utilities
    \ingroup  debugging


    This file provides utilities for profiling applications using gprof. Gprof
    is a profiling tool that allows you to analyze where your program spent 
    its time and which functions called which other functions during execution. 
    This can help you identify performance bottlenecks and optimize your code.

    Profiling Steps:

    1. Compile your program with profiling flags:
    
        Use the following flags:
        ```sh
        CFLAGS = -pg -fno-omit-frame-pointer -fno-inline
        ```
        = or =
        ```sh
        CFLAGS = -g -finstrument-functions -fno-inline 
        LDFLAGS = -lgprof
        ```
        These flags enable profiling and prevent function inlining to ensure 
        accurate profiling data.  If you use the -pg flag, the GCC compiler 
        inserts trapa #33 instructions into your build.

    2. Running your program to create gmon.out:

        Execute your program normally to completion. This will generate a file 
        named `gmon.out` in the current directory, which contains the profiling 
        data.

    3. Running gprof:

        Use the following command to analyze the profiling data:
        ```sh
        $(KOS_GPROF) $(TARGET) gmon.out > gprof_output.txt
        ```
        Replace `$(TARGET)` with the name of your compiled program. This 
        command will generate a human-readable report in `gprof_output.txt`.

    4. Visualizing profiling data with gprof2dot:

        To create a graphical representation of the profiling data, use the 
        `gprof2dot` tool:
        ```sh
        gprof2dot gprof_output.txt > graph.dot
        ```
        This converts the `gprof` output into a DOT format file, which can be 
        used to generate various types of graphs.

    5. Generating an image from the DOT file:

        Use the `dot` command from Graphviz to create a JPEG image from the DOT 
        file:
        ```sh
        dot -Tjpg graph.dot -o graph.jpg
        ```
        This command will generate a JPEG image (`graph.jpg`) that visually 
        represents the profiling data.

    By following these steps, you can effectively profile your program and 
    identify performance bottlenecks for optimization.

    \author Andy Barajas

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

#endif  /* __SYS_GMON_H */
