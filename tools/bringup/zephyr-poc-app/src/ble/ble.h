#ifndef POC_BLE_H
#define POC_BLE_H

#include <stdint.h>

/* Enable Bluetooth and start connectable advertising with the Nordic UART
 * Service UUID, so the overview dashboard can find and connect to the board.
 * Advertising restarts by itself after a disconnect. */
int ble_init(void);

/* Notify `data` on the NUS TX characteristic to the connected central.
 * Returns 0 if sent, or if nobody is connected/subscribed (not an error
 * for us: the next payload follows in 2 s); a negative errno otherwise. */
int ble_send(const uint8_t *data, uint16_t len);

#endif
