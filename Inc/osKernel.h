#ifndef MY_CUSTOM_RTOS_KERNEL_H_
#define MY_CUSTOM_RTOS_KERNEL_H_

#include <stdint.h>
#include <stm32f4xx.h>

#define PERIOD 100 // Period is 100 * quanta, that is quanta = 10 -> 100 * 10 = 1000
#define SR_UIF (1U<<0)

#define MAX_TASKS 7 // Maximum number of threads the RTOS can handle

typedef enum {
    SCHED_ROUND_ROBIN,    // Time-sliced, equal share
    SCHED_COOPERATIVE,    // Tasks run until they explicitly yield
    SCHED_PRIORITY        // Highest priority task runs first
} SchedulerType;

void osKernelInit(SchedulerType scheduler_type);
void osKernelLaunch(uint32_t quanta);
uint8_t osKernelAddThread(void (*task)(void), uint32_t priority);
void osThreadYield(void);
void task3(void);
void tim2_1hz_interrupt_init(void);
void osSemaphoreInit(int32_t *semaphore, int32_t value);
void osSemaphoreSet(int32_t *semaphore);
void osSemaphoreWait(int32_t *semaphore);
void osDelay(uint32_t quanta);
void osTickDecrement(void);

// forward declaration for tcb struct
struct tcb; 

// mutex struct definition
typedef struct mutex {
    int32_t lock; // 1 is available 0 is locked
    struct tcb *owner; // tcb that holds the lock
} osMutex_t;

// mutex functions
void osMutexInit(osMutex_t *mutex);
void osMutexAcquire(osMutex_t *mutex);
void osMutexRelease(osMutex_t *mutex);

// CPU usage
uint32_t osGetTaskCount(void);                  // total tasks including idle
uint32_t osGetTaskRunTicks(uint32_t index);     // ticks for task at given index, 0 if invalid
void osResetAllRunTicks(void);                  // zero all counters, used after printing a report


// Queue / Mailbox for ISR-to-task or task-to-task data transfer
// Unlike a semaphore which only signals "wake up", a queue carries actual data
// Uses a circular (ring) buffer so producer and consumer can work at different rates
// without blocking each other beyond the buffer size
typedef struct osQueue {
    uint8_t *buffer;     // pointer to the storage array provided by the user
    uint32_t size;       // total capacity in bytes, fixed at init time
    uint32_t head;       // write index, moved forward by osQueueSend
    uint32_t tail;       // read index, moved forward by osQueueReceive
    uint32_t count;      // current number of items in the queue
    int32_t semaphore;   // signals waiting receivers when new data arrives
} osQueue_t;

// Queue API
// Init binds a user-provided storage array to the queue, no heap allocation
void osQueueInit(osQueue_t *q, uint8_t *buffer, uint32_t size);

// Send is non-blocking so it is safe to call from ISR context
// Returns 1 on success, 0 if the queue is full (caller decides what to do)
uint8_t osQueueSend(osQueue_t *q, uint8_t data);

// Receive blocks until data is available, then copies one byte into *out
// The calling task sleeps via the internal semaphore while waiting, zero CPU cost
void osQueueReceive(osQueue_t *q, uint8_t *out);

// Pointer queue used by the timer service and any other producer-consumer
// flow that needs to pass structured data (not just bytes) between ISR and task
// Stores void pointers so any struct type can ride through this queue
// We keep it separate from the byte queue so the two can be tuned independently
typedef struct osPtrQueue {
    void **buffer;       // user-provided array of void pointers
    uint32_t size;       // total capacity
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    int32_t semaphore;   // blocks receivers when queue is empty
} osPtrQueue_t;

void osPtrQueueInit(osPtrQueue_t *q, void **buffer, uint32_t size);
uint8_t osPtrQueueSend(osPtrQueue_t *q, void *item);
void osPtrQueueReceive(osPtrQueue_t *q, void **out);

// Software timer, replaces the "dedicated task just to call a callback periodically" pattern
// Far cheaper than a task: ~20 bytes per timer vs 1600 byte stack + a TCB slot
// Callbacks run in the timer service task so they can safely use mutexes, printf, osDelay
typedef struct osTimer {
    uint32_t period;            // reload value in TIM2 ticks (1 tick = 10ms)
    uint32_t remaining;         // counts down each tick, fires at 0
    void (*callback)(void);     // user function, runs in service task context
    uint8_t periodic;           // 1 = auto-reload, 0 = one-shot
    uint8_t active;             // 1 = counting, 0 = stopped (ISR skips)
    struct osTimer *next;       // linked list, NULL terminates
} osTimer_t;

// Create configures but does not start, Start must be called separately
void osTimerCreate(osTimer_t *t, uint32_t period_ticks, uint8_t periodic, void (*cb)(void));
void osTimerStart(osTimer_t *t);
void osTimerStop(osTimer_t *t);
void osTimerReset(osTimer_t *t);

void osTimerTick(void);          // called from TIM2 ISR every tick
void osTimerServiceTask(void);   // service task, add to kernel at highest priority

// Increment the run tick counter of whatever task is currently executing
// Called from TIM2 ISR, encapsulates the currentPt access inside the kernel
void osAccountCurrentTick(void);


#endif