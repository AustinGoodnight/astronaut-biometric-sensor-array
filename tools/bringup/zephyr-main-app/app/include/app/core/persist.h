/*
 * Persistent settings (settings subsystem on NVS, internal flash):
 * boot count and mode. Also reads and clears the hardware reset cause.
 */

#ifndef APP_CORE_PERSIST_H_
#define APP_CORE_PERSIST_H_

#include <stdint.h>

/**
 * @brief Load settings, increment and save the boot count, read and clear
 *        the reset cause.
 *
 * main, first thing (before records are built). Blocks on flash I/O.
 * @return 0, or -errno if settings failed (boot count is then 0 and the
 *         firmware keeps running).
 */
int persist_init(void);

/** @brief Boot count of this boot (1 = first boot). */
uint16_t persist_boot_count(void);

/** @brief hwinfo reset-cause bits read at boot (RESET_* in drivers/hwinfo.h). */
uint32_t persist_reset_cause(void);

/** @brief Saved mode (enum app_mode); MODE_NORMAL if none. */
uint8_t persist_mode(void);

/**
 * @brief Save the mode.
 *
 * Supervisor thread. Blocks on flash I/O. @return 0 or -errno.
 */
int persist_set_mode(uint8_t mode);

#endif /* APP_CORE_PERSIST_H_ */
