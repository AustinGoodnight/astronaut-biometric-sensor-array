#include "sensorPayload.h"

size_t serializePayload(const SensorPayload_t &payload, SerializedPayload &bytes) {
  const size_t sampleCount = payload.fast_count > MAX_FAST_READINGS
                                 ? MAX_FAST_READINGS
                                 : payload.fast_count;

  bytes[0] = static_cast<uint8_t>(payload.timestamp_ms >> 24);
  bytes[1] = static_cast<uint8_t>(payload.timestamp_ms >> 16);
  bytes[2] = static_cast<uint8_t>(payload.timestamp_ms >> 8);
  bytes[3] = static_cast<uint8_t>(payload.timestamp_ms);
  bytes[4] = static_cast<uint8_t>(sampleCount);
  bytes[5] = static_cast<uint8_t>(payload.i2c_value >> 8);
  bytes[6] = static_cast<uint8_t>(payload.i2c_value);
  bytes[7] = payload.status_flags;

  for (size_t i = 0; i < sampleCount; i++) {
    bytes[PAYLOAD_HEADER_SIZE + 2 * i] =
        static_cast<uint8_t>(payload.fast_values[i] >> 8);
    bytes[PAYLOAD_HEADER_SIZE + 2 * i + 1] =
        static_cast<uint8_t>(payload.fast_values[i]);
  }

  return PAYLOAD_HEADER_SIZE + 2 * sampleCount;
}