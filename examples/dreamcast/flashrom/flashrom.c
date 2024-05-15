/* KallistiOS ##version##

   flashrom.c

   Copyright (C) 2024 Andy Barajas
*/

#include <stdio.h>
#include <dc/flashrom.h>

#include <time.h>
#include <stdint.h>

void print_date_from_seconds(uint32_t seconds) {
    // January 1, 1950 in seconds since Unix epoch (January 1, 1970)
    time_t base_time = -631152000;

    // Calculate the exact time by adding the seconds
    time_t exact_time = base_time + seconds;

    // Convert time_t to broken-down time structure
    struct tm *time_info = gmtime(&exact_time);

    // Print the date in a human-readable format
    char buffer[80];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", time_info);
    printf("Date: %s\n", buffer);
}

int main(int argc, char *argv[]) {
    uint32_t  start, size;

    flashrom_syscfg_t sys_cfg;

    if(flashrom_get_syscfg(&sys_cfg))
    {
        fprintf(stderr, "Count not grab system config\n");
    }

    printf("Time: %ld, Language: %ld Stereo: %ld AutoStart: %ld\n", sys_cfg.time, sys_cfg.language, sys_cfg.audio, sys_cfg.autostart);
    print_date_from_seconds(sys_cfg.time);

// #define FLASHROM_PT_SYSTEM      0   /**< \brief Factory settings (read-only, 8K) */
// #define FLASHROM_PT_RESERVED    1   /**< \brief reserved (all 0s, 8K) */
// #define FLASHROM_PT_BLOCK_1     2   /**< \brief Block allocated (16K) */
// #define FLASHROM_PT_SETTINGS    3   /**< \brief Game settings (block allocated, 32K) */
// #define FLASHROM_PT_BLOCK_2     4   /**< \brief Block allocated (64K) */

    flashrom_info(FLASHROM_PT_SYSTEM,  &start, &size);

    return 0;
}
