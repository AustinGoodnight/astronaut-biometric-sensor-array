#include <Arduino.h>

#define FAST_SENSOR_PIN A0  
#define SLOW_SENSOR_PIN A2  

// Thread-safety tool: Ensures only one task modifies the fast readings at a time
SemaphoreHandle_t xDataMutex;
// Notification tool: Tells the processing task when a slow execution occurs
SemaphoreHandle_t xProcessingSemaphore;

// Shared volatile variables protected by the Mutex
int fastReadings[50]; 
int fastReadingCount = 0;
int lastSlowValue = 0;

// Function prototypes
void TaskReadFast(void *pvParameters);
void TaskReadSlow(void *pvParameters);
void TaskProcessBackground(void *pvParameters);

void setup() {
  Serial.begin(1115200); 
  while (!Serial) { delay(10); } 

  pinMode(FAST_SENSOR_PIN, INPUT);
  pinMode(SLOW_SENSOR_PIN, INPUT);

  // Initialize FreeRTOS synchronization objects
  xDataMutex = xSemaphoreCreateMutex();
  xProcessingSemaphore = xSemaphoreCreateBinary();

  Serial.println("Initializing FreeRTOS Processing Pipelines...");

  xTaskCreate(TaskReadFast, "FastTask", 3072, NULL, 3, NULL);

  xTaskCreate(TaskReadSlow, "SlowTask", 3072, NULL, 2, NULL);

  xTaskCreate(TaskProcessBackground, "ProcessTask", 3072, NULL, 1, NULL);
}

void loop() {
  // Main thread remains empty and dormant
}

// TASKS
// ==========================================

// Fast Task: Reads 5 times a second
void TaskReadFast(void *pvParameters) {
  (void) pvParameters;
  const TickType_t xDelay = pdMS_TO_TICKS(200);
  TickType_t xLastWakeTime = xTaskGetTickCount();

  for (;;) {
    int fastValue = (analogRead(FAST_SENSOR_PIN)/4095) * 3.3;
    Serial.printf("[Live Fast] Value: %d\n", fastValue);

    // Secure the shared variables and add this reading to the buffer
    if (xSemaphoreTake(xDataMutex, portMAX_DELAY) == pdTRUE) {
      if (fastReadingCount < 50) { // Bound check to prevent memory overflow
        fastReadings[fastReadingCount] = fastValue;
        fastReadingCount++;
      }
      xSemaphoreGive(xDataMutex);
    }

    vTaskDelayUntil(&xLastWakeTime, xDelay);
  }
}

// Slow Task: Reads every 2 seconds (every 2000ms)
void TaskReadSlow(void *pvParameters) {
  (void) pvParameters;
  const TickType_t xDelay = pdMS_TO_TICKS(2000);
  TickType_t xLastWakeTime = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&xLastWakeTime, xDelay);

    int slowValue = (analogRead(SLOW_SENSOR_PIN)/4095)*3.3;

    // Secure the shared variables and save the latest slow reading
    if (xSemaphoreTake(xDataMutex, portMAX_DELAY) == pdTRUE) {
      lastSlowValue = slowValue;
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
      int slowValueCopy = 0;

      // Lock data briefly to safely copy shared values out into local memory
      if (xSemaphoreTake(xDataMutex, portMAX_DELAY) == pdTRUE) {
        countCopy = fastReadingCount;
        slowValueCopy = lastSlowValue;
        
        for (int i = 0; i < countCopy; i++) {
          sum += fastReadings[i];
        }
        
        // Reset the counters immediately so Fast Task can keep gathering fresh data
        fastReadingCount = 0; 
        xSemaphoreGive(xDataMutex);
      }

      // Calculate the average using our local copy (so we keep Mutex lock time tiny)
      if (countCopy > 0) {
        average = (float)sum / countCopy;
      }

      // Package and Print the aggregated packet
      Serial.println("\n=============================================");
      Serial.printf("[BACKGROUND REPORT]\n");
      Serial.printf(" -> Captured Fast Readings: %d\n", countCopy);
      Serial.printf(" -> Calculated Fast Avg:    %.2f\n", average);
      Serial.printf(" -> Matched Slow Value:     %d\n", slowValueCopy);
      Serial.println("=============================================\n");
    }
  }
}
