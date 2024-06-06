#include "FreeRTOS.h"
#include "croutine.h"
#include "deprecated_definitions.h"
#include "event_groups.h"
#include "list.h"
#include "stream_buffer.h"
#include "message_buffer.h"
#include "mpu_wrappers.h"
#include "portable.h"
#include "projdefs.h"
#include "queue.h"
#include "semphr.h"
#include "StackMacros.h"
#include "task.h"
#include "timers.h"
#include "supporting_functions.h"
#include "stdio.h"
#include "stdlib.h"
#include "time.h"

#define NUM_PARKING_LOTS 24
#define PARKING_QUEUE_LENGTH 5
#define MESSAGE_BUFFER_SIZE 300
#define EVENT_PARKING_FULL (1 << 0)
#define EVENT_PARKING_PAID (1 << 1)

typedef enum {
    eAvailable,
    eOccupied
} State;

typedef struct {
    State eState;
    uint32_t ulCarID;
    TickType_t xEntryTime;
} Parking_Lot_t;

typedef struct {
    Parking_Lot_t xLots[NUM_PARKING_LOTS];
    SemaphoreHandle_t xParkingMutex;
    EventGroupHandle_t xParkingEventGroup;
} Parking_Area_t;

Parking_Area_t xArea;
QueueHandle_t xParkingQueue;
MessageBufferHandle_t xParkingMessageBuffer;
TimerHandle_t xProxyTimer;

TaskHandle_t xDaemonTaskHandle;
TaskHandle_t xTollingHandlerTaskHandle;

void ulEntryInterruptHandler(BaseType_t* pxHigherPriorityTaskWoken) {
    uint32_t ulCarID = rand() % 10000;
    TickType_t xEntryTime = xTaskGetTickCountFromISR();
    printf("ulEntryInterruptHandler: ulCarID=%04d, xEntryTime=%d\n", ulCarID, xEntryTime);
    xTaskNotifyFromISR(xDaemonTaskHandle, (ulCarID << 16) | xEntryTime, eSetValueWithOverwrite, pxHigherPriorityTaskWoken);
    
    if (*pxHigherPriorityTaskWoken == pdTRUE) {
        /* printf("Higher priority task woken, yielding...\n"); */
        /* taskYIELD(); */
        portYIELD_FROM_ISR(*pxHigherPriorityTaskWoken);
    }
}

void vSoftwareInterruptTrigger(uint32_t ulInterruptNumber, BaseType_t* pxHigherPriorityTaskWoken) {
    if (ulInterruptNumber == 3) {
        ulEntryInterruptHandler(pxHigherPriorityTaskWoken);
    }
}

void vPeriodicTaskEntry(void* pvParameters) {
    for (;;) {
        uint32_t delay = (rand() % 3 + 1) * 1000;
        vTaskDelay(pdMS_TO_TICKS(delay));
        /* printf("\nvPeriodicTaskEntry delay: %d\n", delay); */

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vSoftwareInterruptTrigger(3, &xHigherPriorityTaskWoken);
    }
}

void vEntry_Handler(void* pvParameters) {
    uint32_t ulReceivedValue;
    for (;;) {
        xTaskNotifyWait(0, 0, &ulReceivedValue, portMAX_DELAY);
        uint32_t ulCarID = ulReceivedValue >> 16;
        TickType_t xEntryTime = ulReceivedValue & 0xFFFF;

        xSemaphoreTake(xArea.xParkingMutex, portMAX_DELAY);
        /* printf("Borrow Semaphore_vEntry_Handler\n"); */

        for (int i = 0; i < NUM_PARKING_LOTS; i++) {
            if (xArea.xLots[i].eState == eAvailable) {
                xArea.xLots[i].eState = eOccupied;
                xArea.xLots[i].ulCarID = ulCarID;
                xArea.xLots[i].xEntryTime = xEntryTime;
                printf("Car %04d entered at time %d into slot %d\n", ulCarID, xEntryTime, i);
                break;
            }
        }
        xSemaphoreGive(xArea.xParkingMutex);
        /* printf("Ruturn Semaphore_vEntry_Handler\n"); */
    }
}

void vPeriodicTaskTolling(void* pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    for (;;) {
        uint32_t delay = (rand() % 3 + 1) * 1000;
        vTaskDelay(pdMS_TO_TICKS(delay));
        /* printf("\nvPeriodicTaskTolling delay: %d\n", delay); */

        xSemaphoreTake(xArea.xParkingMutex, portMAX_DELAY);
        /* printf("Borrow Semaphore_vPeriodicTaskTolling\n"); */

        for (int i = 0; i < NUM_PARKING_LOTS; i++) {
            if (xArea.xLots[i].eState == eOccupied) {
                xTaskNotify(xTollingHandlerTaskHandle, i, eSetValueWithOverwrite);
                break;
            }
        }
        xSemaphoreGive(xArea.xParkingMutex);
        /* printf("Ruturn Semaphore_vPeriodicTaskTolling\n"); */
    }
}

void vTollingHandler(void* pvParameters) {
    uint32_t ulSlotIndex;
    for (;;) {
        xTaskNotifyWait(0, 0, &ulSlotIndex, portMAX_DELAY);
        /* printf("vTollingHandler is operating\n"); */
        xEventGroupSetBits(xArea.xParkingEventGroup, (1 << ulSlotIndex));
    }
}

void vExitHandler(void* pvParameters) {
    EventBits_t uxBits;
    for (;;) {
        uxBits = xEventGroupWaitBits(xArea.xParkingEventGroup, 0xFFFFFF, pdTRUE, pdFALSE, portMAX_DELAY);
        /* printf("vExitHandler is operating\n"); */

        xSemaphoreTake(xArea.xParkingMutex, portMAX_DELAY);
        /* printf("Borrow Semaphore_vExitHandler\n"); */

        for (int i = 0; i < NUM_PARKING_LOTS; i++) {
            if (uxBits & (1 << i)) {
                uint32_t ulCarID = xArea.xLots[i].ulCarID;
                TickType_t xEntryTime = xArea.xLots[i].xEntryTime;
                TickType_t xExitTime = xTaskGetTickCount();
                uint32_t ulParkingData[3] = { ulCarID, xEntryTime, xExitTime };
                xQueueSend(xParkingQueue, &ulParkingData, portMAX_DELAY);
                xArea.xLots[i].eState = eAvailable;
                break;
            }
        }
        xSemaphoreGive(xArea.xParkingMutex);
        /* printf("Ruturn Semaphore_vExitHandler\n"); */
    }
}

void prvProxyTimerCallback(TimerHandle_t xTimer) {
    char buffer[60];
    uint32_t ulParkingData[3];

    while (xQueueReceive(xParkingQueue, &ulParkingData, 0) == pdTRUE) {
        sprintf(buffer, "Car %04d entered at %d and exited at %d\n", ulParkingData[0], ulParkingData[1], ulParkingData[2]);
        /* printf("Sent: %s", buffer); */
        xMessageBufferSend(xParkingMessageBuffer, buffer, sizeof(buffer), portMAX_DELAY);
    }
}

void vServerTask(void* pvParameters) {
    char buffer[60];
    for (;;) {
        size_t xReceivedBytes = xMessageBufferReceive(xParkingMessageBuffer, buffer, sizeof(buffer), portMAX_DELAY);
        if (xReceivedBytes > 0) {
            printf("Received: %s", buffer);
        }
    }
}

int main(void) {

    /* Print initial state of parking lots */
    /*for (int i = 0; i < NUM_PARKING_LOTS; i++) {
        printf("Initial Slot %d: State = %d, CarID = %d, EntryTime = %d\n",
            i, xArea.xLots[i].eState, xArea.xLots[i].ulCarID, xArea.xLots[i].xEntryTime);
    }*/

    xArea.xParkingMutex = xSemaphoreCreateMutex();
    xArea.xParkingEventGroup = xEventGroupCreate();
    xParkingQueue = xQueueCreate(PARKING_QUEUE_LENGTH, sizeof(uint32_t) * 3);
    xParkingMessageBuffer = xMessageBufferCreate(MESSAGE_BUFFER_SIZE);

    xTaskCreate(vPeriodicTaskEntry, "PeriodicTaskEntry", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(vEntry_Handler, "EntryHandler", configMINIMAL_STACK_SIZE, NULL, 1, &xDaemonTaskHandle);
    xTaskCreate(vPeriodicTaskTolling, "PeriodicTaskTolling", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(vTollingHandler, "TollingHandler", configMINIMAL_STACK_SIZE, NULL, 1, &xTollingHandlerTaskHandle);
    xTaskCreate(vExitHandler, "ExitHandler", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(vServerTask, "ServerTask", configMINIMAL_STACK_SIZE, NULL, 1, NULL);

    xProxyTimer = xTimerCreate("ProxyTimer", pdMS_TO_TICKS(5000), pdTRUE, 0, prvProxyTimerCallback);
    xTimerStart(xProxyTimer, 0);

    vTaskStartScheduler();

    for (;;){
    }
    return 0;
}