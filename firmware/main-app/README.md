# Main App

Firmware for the real sensor-node board — distinct from
[firmware/lora-testing](../lora-testing), which is just the minimal LoRa
TX/RX bring-up harness. This is where the actual per-second
read-sensors-and-transmit loop will live.

Nothing here yet. TODO before real development starts:

- [ ] Set up an interrupt (hardware timer) to trigger sensor processing and
  transmission once per second, instead of the `delay()`-based loop used
  in firmware/lora-testing.
- [ ] Define a shared struct for the data packet sent over LoRa — fixed-size
  binary fields instead of the ASCII string format in firmware/lora-testing —
  so TX and RX agree on layout without manual string parsing.
- [ ] Lay out a basic structure for reading from each sensor (one
  function/module per sensor), and document the expected data format
  (units, range, encoding) each one produces before it's packed into the
  struct above.


