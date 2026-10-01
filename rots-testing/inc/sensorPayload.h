#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

constexpr size_t MAX_FAST_READINGS = 50;
constexpr size_t PAYLOAD_HEADER_SIZE = 8;
constexpr size_t MAX_SERIALIZED_PAYLOAD_SIZE = PAYLOAD_HEADER_SIZE +
																								MAX_FAST_READINGS * sizeof(uint16_t);

typedef struct __attribute__((packed)) {
	uint32_t timestamp_ms;
	uint8_t fast_count;
	uint16_t i2c_value;
	uint8_t status_flags;
	uint16_t fast_values[MAX_FAST_READINGS];
} SensorPayload_t;

static_assert(sizeof(SensorPayload_t) == 108, "SensorPayload_t must be 108 bytes");

constexpr uint8_t STATUS_I2C_VALID = 1 << 0;
constexpr uint8_t STATUS_I2C_ERROR = 1 << 1;

using SerializedPayload = std::array<uint8_t, MAX_SERIALIZED_PAYLOAD_SIZE>;

size_t serializePayload(const SensorPayload_t &payload, SerializedPayload &bytes);
