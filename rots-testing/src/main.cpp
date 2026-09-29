#include <Arduino.h>
#include <Wire.h>
#include <array>
#include "SparkFun_SCD30_Arduino_Library.h" // Include the SparkFun SCD30 library

#define FAST_SENSOR_PIN A0  
#define SLOW_SENSOR_PIN A2  

// Thread-safety tool: Ensures only one task modifies the fast readings at a time
SemaphoreHandle_t xDataMutex;
// Notification tool: Tells the processing task when a slow execution occurs
SemaphoreHandle_t xProcessingSemaphore;

SCD30 co2Sensor;
bool co2SensorReady = false;

typedef struct __attribute__((packed)) {
  uint32_t timestamp_ms;
  uint16_t i2c_value;
  uint8_t fast_count;
  uint8_t status_flags;
  uint16_t fast_values[50];
} SensorPayload_t;

static_assert(sizeof(SensorPayload_t) == 108, "SensorPayload_t must be 108 bytes");

constexpr uint8_t STATUS_I2C_VALID = 1 << 0;
constexpr uint8_t STATUS_I2C_ERROR = 1 << 1;
constexpr size_t PAYLOAD_HEADER_SIZE = 8;
constexpr size_t MAX_SERIALIZED_PAYLOAD_SIZE = PAYLOAD_HEADER_SIZE + 50 * sizeof(uint16_t);

using SerializedPayload = std::array<uint8_t, MAX_SERIALIZED_PAYLOAD_SIZE>;

size_t serializePayload(const SensorPayload_t &payload, SerializedPayload &bytes) {
  bytes[0] = static_cast<uint8_t>(payload.timestamp_ms >> 24);
  bytes[1] = static_cast<uint8_t>(payload.timestamp_ms >> 16);
  bytes[2] = static_cast<uint8_t>(payload.timestamp_ms >> 8);
  bytes[3] = static_cast<uint8_t>(payload.timestamp_ms);
  bytes[4] = payload.fast_count;
  bytes[5] = static_cast<uint8_t>(payload.i2c_value >> 8);
  bytes[6] = static_cast<uint8_t>(payload.i2c_value);
  bytes[7] = payload.status_flags;

  for (size_t i = 0; i < payload.fast_count; i++) {
    bytes[PAYLOAD_HEADER_SIZE + 2 * i] = static_cast<uint8_t>(payload.fast_values[i] >> 8);
    bytes[PAYLOAD_HEADER_SIZE + 2 * i + 1] = static_cast<uint8_t>(payload.fast_values[i]);
  }

  return PAYLOAD_HEADER_SIZE + 2 * payload.fast_count;
}

// Shared volatile variables protected by the Mutex
std::array<int, 50> fastReadings{};
int fastReadingCount = 0;
int lastCO2Value = 0;
bool lastCO2Valid = false;

// Function prototypes
void TaskReadFast(void *pvParameters);
void TaskReadCO2(void *pvParameters);
void TaskProcessBackground(void *pvParameters);

void setup() {
  Serial.begin(115200); 
  Serial.println("Starting setup");

  Wire.begin(); 
  
  // Initialize the sensor
  co2SensorReady = co2Sensor.begin();
  if (!co2SensorReady) {
    Serial.println("SCD30 not detected. Please check your wiring.");
  } else {
    Serial.println("SCD30 detected. Reading data...");
  }

  pinMode(FAST_SENSOR_PIN, INPUT);
  pinMode(SLOW_SENSOR_PIN, INPUT);

  // Initialize FreeRTOS synchronization objects
  xDataMutex = xSemaphoreCreateMutex();
  xProcessingSemaphore = xSemaphoreCreateBinary();

  Serial.println("Initializing FreeRTOS Processing Pipelines...");

  xTaskCreate(TaskReadFast, "FastTask", 3072, NULL, 3, NULL);

  if (co2SensorReady) {
    xTaskCreate(TaskReadCO2, "Co2Task", 3072, NULL, 2, NULL);
  }

  xTaskCreate(TaskProcessBackground, "ProcessTask", 3072, NULL, 1, NULL);
}

void loop() {
}

// TASKS
// ==========================================

// Fast Task: Reads 5 times a second
void TaskReadFast(void *pvParameters) {
  (void) pvParameters;
  const TickType_t xDelay = pdMS_TO_TICKS(200);
  TickType_t xLastWakeTime = xTaskGetTickCount();

  for (;;) {
    int fastValue = analogRead(FAST_SENSOR_PIN);
    // Serial.printf("[Live Fast] Value: %d\n", fastValue);

    // Secure the shared variables and add this reading to the buffer
    if (xSemaphoreTake(xDataMutex, portMAX_DELAY) == pdTRUE) {
      if (fastReadingCount < fastReadings.size()) {
        fastReadings[fastReadingCount] = fastValue;
        fastReadingCount++;
      }
      xSemaphoreGive(xDataMutex);
    }

    vTaskDelayUntil(&xLastWakeTime, xDelay);
  }
}

// Slow Task: Reads every 2 seconds (every 2000ms)
void TaskReadCO2(void *pvParameters) {
  (void) pvParameters;
  const TickType_t xDelay = pdMS_TO_TICKS(500);
  TickType_t xLastWakeTime = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&xLastWakeTime, xDelay);

    // Serial.printf("Attempting CO2 value... %c\n", co2Sensor.dataAvailable() ? 'Y' : 'N');

    if (!co2Sensor.readMeasurement()) {
      continue;
    }

    int CO2Value = co2Sensor.getCO2();

    // Secure the shared variables and save the latest slow reading
    if (xSemaphoreTake(xDataMutex, portMAX_DELAY) == pdTRUE) {
      lastCO2Value = CO2Value;
      lastCO2Valid = true;
      xSemaphoreGive(xDataMutex);
    }

    // Wake up the background processing task
    xSemaphoreGive(xProcessingSemaphore);
  }
}

// Background Processing Task: Calculates average and packages data
void TaskProcessBackground(void *pvParameters) {
  (void) pvParameters;

  for (;;) {
    // Sleep indefinitely until the Slow Task gives the semaphore signal
    if (xSemaphoreTake(xProcessingSemaphore, portMAX_DELAY) == pdTRUE) {
      
      long sum = 0;
      float average = 0.0;
      int countCopy = 0;
      int CO2ValueCopy = 0;
      bool CO2ValidCopy = false;
      std::array<int, 50> fastReadingsCopy{};

      // Lock data briefly to safely copy shared values out into local memory
      if (xSemaphoreTake(xDataMutex, portMAX_DELAY) == pdTRUE) {
        countCopy = fastReadingCount;
        CO2ValueCopy = lastCO2Value;
        CO2ValidCopy = lastCO2Valid;
        
        for (int i = 0; i < countCopy; i++) {
          fastReadingsCopy[i] = fastReadings[i];
        }
        
        // Reset the counters immediately so Fast Task can keep gathering fresh data
        fastReadingCount = 0; 
        xSemaphoreGive(xDataMutex);
      }

      // Calculate the average using our local copy (so we keep Mutex lock time tiny)
      if (countCopy > 0) {
        for (int i = 0; i < countCopy; i++) {
          sum += fastReadingsCopy[i];
        }
        average = (float)sum / countCopy;
      }

      SensorPayload_t payload{};
      payload.timestamp_ms = millis();
      payload.i2c_value = static_cast<uint16_t>(CO2ValueCopy);
      payload.fast_count = static_cast<uint8_t>(countCopy);
      payload.status_flags = CO2ValidCopy ? STATUS_I2C_VALID : 0;
      for (int i = 0; i < countCopy; i++) {
        payload.fast_values[i] = static_cast<uint16_t>(fastReadingsCopy[i]);
      }
      SerializedPayload payloadBytes{};
      const size_t payloadLength = serializePayload(payload, payloadBytes);

      // Package and Print the aggregated packet
      Serial.println("\n=============================================");
      Serial.printf("[BACKGROUND REPORT]\n");
      Serial.printf(" -> Captured Fast Readings: %d\n", countCopy);
      Serial.print(" -> Fast readings: [");
      for (int i = 0; i < countCopy; i++) {
        Serial.print(fastReadingsCopy[i]);
        if (i < countCopy - 1) {
          Serial.print(", ");
        }
      }
      Serial.println("]");
      Serial.printf(" -> Calculated Fast Avg:    %.2f\n", average);
      Serial.printf(" -> Matched CO2 value     %d\n", CO2ValueCopy);
      Serial.printf(" -> RF payload (%u bytes): ", static_cast<unsigned>(payloadLength));
      for (size_t i = 0; i < payloadLength; i++) {
        Serial.printf("%02X ", payloadBytes[i]);
      }
      Serial.println();
      Serial.println("=============================================\n");
    }
  }
}