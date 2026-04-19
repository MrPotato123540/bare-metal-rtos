#include <osKernel.h>

#define STACK_SIZE 400 //400 32-bit values = 1600bytes

#define BUS_FREQ 16000000 //16Mhz
#define CTRL_ENABLE (1U<<0) //To enable SysTick
#define CTRL_TICKINT (1U<<1) //To enable SysTick's interrupt
#define CTRL_CLCKSRC (1U<<2) //This will allow us to indicate processor or external clock source
#define CTRL_COUNTFLAG (1U<<16) //Returns 1 if timer counted to 0 since last time this was read
#define SYSTICK_RST 0

#define TIM2EN (1U<<0)
#define CR1_CEN (1U<<0) //Counter enable bit
#define DIER_UIE (1U<<0) //Interrupt update interrupt enable register

#define INTCTRL (*((volatile uint32_t *)0xE000ED04)) //Address of ICSR register
#define PENDSTSET (1U<<26) //Symbolic name for PENDST SET bit (26th bit)

// Magic value placed at the bottom of every task stack
// If a task overflows, this gets overwritten and we catch it at context switch
// 0xDEADBEEF is picked because it is unlikely to occur as a legitimate stack value
#define STACK_CANARY 0xDEADBEEF

uint32_t period_tick;

void osSchedulerLaunch(void);
void osSchedulerRoundRobin(void);
void osMutexInit(osMutex_t *mutex);
void osMutexAcquire(osMutex_t *mutex);
void osMutexRelease(osMutex_t *mutex);
static void osIdleTask(void);
void osStackOverflowHandler(struct tcb *offender);

uint32_t MILLIS_PRESCALER;

// Global variable to store the selected scheduler type
SchedulerType active_scheduler = SCHED_ROUND_ROBIN; // Default is Round Robin

// doubly linked list (or circular)
struct tcb {
	int32_t *stackPt;
	struct tcb *nextPt;
	uint32_t priority; // 0 is highest priority, higher numbers mean lower priority (active priority)
	uint32_t basePriority; // original priority to return after releasing mutex
	uint32_t sleepTime; // Blocks the task for a specific amount of ticks
	int32_t *blockedOn; // If not NULL, this task is waiting on this semaphore
	                    // Scheduler must skip such tasks until they are unblocked
	struct mutex *blockedOnMutex; // if not NULL this task is waiting for this mutex
	
	// Pointer to the bottom of this task's stack (where the canary lives)
  // Cached here so the scheduler can check it in O(1)
  int32_t *stackBase;
	
	// CPU usage accounting: total TIM2 ticks this task has been the running task
  // Incremented in TIM2 ISR against whichever task currentPt points to at that moment
  // Reset periodically by the reporter so percentages reflect a recent window
    uint32_t runTicks;
};

typedef struct tcb tcbType;

//to store our tcbs
tcbType tcbs[MAX_TASKS];

//Each thread will have stack size of 100 = 400 bytes
int32_t TCB_STACK[MAX_TASKS][STACK_SIZE];

tcbType *currentPt; //to navigate through tcbs

void osKernelStackInıt(int i) {

	//We save last address to stack pointer to do context switching
	//& is for storing memory address
	//STACK_SIZE - 16 is for dummy stack fremae
	tcbs[i].stackPt = &TCB_STACK[i][STACK_SIZE - 16]; //Stack Pointer

	//STACK_SIZE - 1 will correspond to PSR(Program Status Register) register because we take the index from 0
	//We will put values to stack in full descending order because of the architecture
	//= (1U<<21) to use processor in thumb mode
	//So we use bit 21 (T-bit) in PSR to 1 to operate in thumb mode
	TCB_STACK[i][STACK_SIZE - 1] = (1U<<24);

	//Block below is optional
	//Dummy Stack Content for only debugging
	//R0 to R7 low registers
	//R8 to R12 high registers
	TCB_STACK[i][STACK_SIZE - 3] = 0xAAAAAAAA; //R14 = LR(Link register)
	TCB_STACK[i][STACK_SIZE - 4] = 0xAAAAAAAA; //R12
	TCB_STACK[i][STACK_SIZE - 5] = 0xAAAAAAAA; //R3
	TCB_STACK[i][STACK_SIZE - 6] = 0xAAAAAAAA; //R2
	TCB_STACK[i][STACK_SIZE - 7] = 0xAAAAAAAA; //R1
	TCB_STACK[i][STACK_SIZE - 8] = 0xAAAAAAAA; //R0

	TCB_STACK[i][STACK_SIZE - 9] = 0xAAAAAAAA; //R11
	TCB_STACK[i][STACK_SIZE - 10] = 0xAAAAAAAA; //R10
	TCB_STACK[i][STACK_SIZE - 11] = 0xAAAAAAAA; //R9
	TCB_STACK[i][STACK_SIZE - 12] = 0xAAAAAAAA; //R8
	TCB_STACK[i][STACK_SIZE - 13] = 0xAAAAAAAA; //R7
	TCB_STACK[i][STACK_SIZE - 14] = 0xAAAAAAAA; //R6
	TCB_STACK[i][STACK_SIZE - 15] = 0xAAAAAAAA; //R5
	TCB_STACK[i][STACK_SIZE - 16] = 0xAAAAAAAA; //R4
}

/*

//Addresses of thread functions will be passed argument to this function
//We use 3 threads because we defined 3
uint8_t osKernelAddThreads(void (*task0)(void), void (*task1)(void), void (*task2)(void)) {
	//Disable global interrupts
	__disable_irq();

	//this structure is for showing next in round robin scheduler
	tcbs[0].nextPt = &tcbs[1];
	tcbs[1].nextPt = &tcbs[2];
	tcbs[2].nextPt = &tcbs[0];

	//Initial stack for thread0
	//So we will initialize all fields except program counter
	osKernelStackInıt(0);

	//Initial PC
	TCB_STACK[0][STACK_SIZE - 2] = (uint32_t)(task0);

	//Initial stack for thread1
	//So we will initialize all fields except program counter
	osKernelStackInıt(1);

	//Initial PC
	TCB_STACK[1][STACK_SIZE - 2] = (uint32_t)(task1);

	//Initial stack for thread2
	//So we will initialize all fields except program counter
	osKernelStackInıt(2);

	//Initial PC
	TCB_STACK[2][STACK_SIZE - 2] = (uint32_t)(task2);

	//Start from thread0
	currentPt = &tcbs[0];

	//Enable global interrupts
	__enable_irq();

	return 1;
}

*/

// Track the current number of active threads in the system
uint32_t current_task_count = 0;

void osKernelInit(SchedulerType scheduler_type) {
	// Set the global scheduler type based on user input
	active_scheduler = scheduler_type;
	
	//If 16 million equals 1 second, then 16000 equals 1 millisecond
	MILLIS_PRESCALER = (BUS_FREQ / 1000);
	
	// add idle task automatically with the absolute lowest priority (255)
	// this guarantees the scheduler always has a valid task to run when others sleep
	// and since it is added first it becomes tcbs[0]
	osKernelAddThread(&osIdleTask, 255);
}

// Dynamically adds a new thread to the RTOS thread pool
// task: Pointer to the task function to be executed
// 1 if successful, 0 if task limit is reached
uint8_t osKernelAddThread(void (*task)(void), uint32_t priority) {
    
    // Check if we have reached the maximum thread capacity
    if (current_task_count >= MAX_TASKS) {
        return 0; // Fail: No more space
    }

    // Dynamic Circular Linked List Creation
    if (current_task_count == 0) {
        // If it's the first task, it points to itself
        tcbs[0].nextPt = &tcbs[0];
    } else {
        // The previous task points to the newly added task
        tcbs[current_task_count - 1].nextPt = &tcbs[current_task_count];
        // The newly added task points back to the first task to close the circle
        tcbs[current_task_count].nextPt = &tcbs[0];
    }
		
		// Assign the priority to the newly created TCB
		tcbs[current_task_count].priority = priority;
		
		// Save original priority
		tcbs[current_task_count].basePriority = priority; 
		
		// Initialize sleep time to 0 (Task is Ready)
		tcbs[current_task_count].sleepTime = 0;
		
		// Initialize blockedOn to NULL (Task is not waiting on any semaphore)
		// If we skipped this, a fresh TCB would contain garbage memory
		// and the scheduler would think the task is blocked on a random address
		tcbs[current_task_count].blockedOn = 0;
		tcbs[current_task_count].blockedOnMutex = 0; // initialize with no mutex
		
		// Initialize CPU usage counter, will accumulate once the scheduler starts
		tcbs[current_task_count].runTicks = 0;

    // Initialize the Stack for the new task
    // xPSR register: Thumb bit must be set to 1 (bit 24)
    TCB_STACK[current_task_count][STACK_SIZE - 1] = (1U << 24);
    
    // PC (Program Counter) register: Points to the task function
    TCB_STACK[current_task_count][STACK_SIZE - 2] = (uint32_t)(task);

    // Initialize the stack pointer to point to the lowest initialized value (R4)
    // Cortex-M exceptions save 16 words in total
    tcbs[current_task_count].stackPt = &TCB_STACK[current_task_count][STACK_SIZE - 16]; 
		
		// Stack grows downward, so index 0 is the lowest address (bottom of stack)
		// We plant the canary here and cache its address in the TCB for quick access later
		// Without this, an overflow would silently corrupt the next task and crash elsewhere
		TCB_STACK[current_task_count][0] = (int32_t)STACK_CANARY;
		tcbs[current_task_count].stackBase = &TCB_STACK[current_task_count][0];

    // Increment task counter for the next addition
    current_task_count++;

    return 1;
}

void osSchedulerLaunch(void) {
	__asm volatile(
		//Load address of currentPt into R0
		"LDR R0, =currentPt\n"

		//Load r2 from address equals r0, r2 =currentPt
		"LDR R2, [R0]\n"

		//Load Cortex-M SP from address equals R2, that is SP = currentPt->stackPt
		"LDR SP, [R2]\n"

		//Restore r4, r5, r6, r7, r8, r9, r10, r11
		"POP {R4-R11}\n"

		//Restore r12
		"POP {R12}\n"

		//Restore r0, r1, r2, r3
		"POP {R0-R3}\n"

		//Skip LR
		//We do it because it is part of initial stack
		"ADD SP, SP, #4\n"

		//Create a new start location by popping LR
		"POP {LR}\n"

		//Skip PSR by adding 4 to SP
		"ADD SP, SP, #4\n"

		//Enable global interrupts
		"CPSIE I\n"

		//Return from exception and restore r0, r1, r2, r3, r12, lr, pc, psrx
		"BX LR\n"
	);
}

void osKernelLaunch(uint32_t quanta) {

	//Reset SysTick
	SysTick->CTRL = SYSTICK_RST;

	//Clear SysTick current value register
	SysTick->VAL = 0;

	//Load quanta
	//We count from zero so we add -1
	SysTick->LOAD = (quanta * MILLIS_PRESCALER) - 1;

	//Set SysTick to low priority
	//In rtos kernel needs to have lowest priority
	//so you can run very important tasks from interrupt service routines
	//We will use nested vectored interrupt controller to response to interrupt-driven events
	NVIC_SetPriority(SysTick_IRQn, 15);

	//Enable SysTick, select internal clock
	SysTick->CTRL = CTRL_CLCKSRC | CTRL_ENABLE;

	//Enable SysTick interrupt
	SysTick->CTRL |= CTRL_TICKINT;
	
	// Determine the initial thread
	currentPt = &tcbs[0];

	//Launch scheduler
	osSchedulerLaunch();
}

//__attribute__((naked)) is adding only our assembly codes
//When exception occurs these registers are automatically saved onto the stack:
// r0, r1, r2, r3, r12, lr, pc, psrx
__attribute__((naked)) void SysTick_Handler(void) {

	//Suspend current thread
	//Disable global interrupts because we want it to be atomic
	__asm volatile(
		"CPSID I\n"

		//Save r4, r5, r6, r7, r8, r9, r10, r11
		"PUSH {R4-R11}\n"

		//Load address currentPt into r0
		"LDR R0, =currentPt\n"

		//Load r1 from address equals r0, that is r1 =currentPt
		"LDR R1, [R0]\n"

		//Store Cortex-M SP at address equals r1, that is save SP into tcb
		"STR SP, [R1]\n"

		//Choose next thread

		/*
		//Load r1 from a location 4bytes above address r1, that is r1 = currentPt->next
		"LDR R1, [R1, #4]\n"
		*/

		//For not crashing the system inside of an interrupt
		"PUSH {R0, LR}\n"
		"BL osSchedulerSelectNextTask\n"
		"POP {R0, LR}\n"

		//R1 = CurrentPt, that is new thread
		"LDR R1, [R0]\n"

		//SP = CurrentPt->StackPt
		"LDR SP, [R1]\n"

		//Restore r4, r5, r6, r7, r8, r9, r10, r11
		"POP {R4-R11}\n"

		//Enable global interrupts
		"CPSIE I\n"

		//Return from exception and restore r0, r1, r2, r3, r12, lr, pc, psrx
		"BX LR\n"
	);
}

// we are adding cooperative mode to threads
// because when we use default quanta, some threads can need less time
// so because when a SysTick interrupts cares, the next thread is selected
// we need to find a way to trigger SysTick
void osThreadYield(void) {

	// Clear SysTick current value
	SysTick->VAL = 0;

	// Trigger SysTick
	// Interrupt control and state register
	// It will set-pending for non-maskable interrupt and for SysTick exception
	INTCTRL = PENDSTSET;
}

// Unified scheduler logic that acts based on the active_scheduler configuration
void osSchedulerSelectNextTask(void) {
	
	// Stack overflow check on the outgoing task
  // SysTick_Handler just saved its SP, so currentPt still points to it
	// We check two things, either one means overflow:
  //   (a) SP has reached or passed the canary (overflow happening now)
  //   (b) Canary value is corrupted (overflow happened earlier)
  // Canary alone could miss a big local array that jumps over it in one shot,
  // so the SP check closes that gap for almost zero cost
  if ((uint32_t)currentPt->stackPt <= (uint32_t)currentPt->stackBase ||
		*(currentPt->stackBase) != (int32_t)STACK_CANARY) {
		osStackOverflowHandler(currentPt);
  }
	
	// Execute periodic hardware clock task (Task 3)
	if (++period_tick == PERIOD) {
		(*task3)();
		period_tick = 0;
	}

		switch (active_scheduler) {
		
		case SCHED_ROUND_ROBIN:
			// Move to the next task in the circular list
			// In strict round robin we would not skip anything, but in practice
			// we must skip blocked and sleeping tasks or the system wastes CPU
			// running tasks that cannot make progress
			currentPt = currentPt->nextPt;
			
			// Skip sleeping or blocked tasks (up to one full lap)
			for (int i = 0; i < current_task_count; i++) {
				if (currentPt->sleepTime == 0 && currentPt->blockedOn == 0 && currentPt->blockedOnMutex == 0) break;
				currentPt = currentPt->nextPt;
			}
			break;

		case SCHED_COOPERATIVE:
			// In cooperative mode, we always advance to the next task in the circle
			// It does not matter if the trigger was a hardware tick or a software yield
			// The key idea of cooperative scheduling is that a task is never preempted
			// mid-execution unless it voluntarily calls osThreadYield() or osDelay()
			// Since we only enter this handler via yield or quanta-end, advancing here is safe
			currentPt = currentPt->nextPt;
		
			// Skip over any task that is currently sleeping or blocked on a semaphore
			// We loop at most current_task_count times to prevent an infinite loop
			// in the case where all tasks are sleeping or blocked (system would hang otherwise)
			// If every task is unavailable, currentPt will land on an unavailable task
			// and that task will be run incorrectly. A proper fix requires an idle task
			for (int i = 0; i < current_task_count; i++) {
				if (currentPt->sleepTime == 0 && currentPt->blockedOn == 0 && currentPt->blockedOnMutex == 0) break;
				currentPt = currentPt->nextPt;
			}
			break;

		case SCHED_PRIORITY: {
			// Find the task with the highest priority (lowest numerical value)
			tcbType *bestPt = &tcbs[0]; 
			uint32_t bestPriority = 0xFFFFFFFF; // Set to absolute maximum initially

			for (int i = 0; i < current_task_count; i++) {
				// Only consider tasks that are ready to run
				// Ready means not sleeping and not blocked on any semaphore
				if (tcbs[i].sleepTime == 0 && tcbs[i].blockedOn == 0 && 
            tcbs[i].blockedOnMutex == 0 && tcbs[i].priority < bestPriority) {
					bestPriority = tcbs[i].priority;
					bestPt = &tcbs[i];
				}
			}
			
			// Context switch to the highest priority task found
			currentPt = bestPt;
			break;
		}
	}
}

// for adding periodic scheduler with hardware clock
void tim2_1hz_interrupt_init(void) {
  // Function name says 1hz (it is because of the old setting and I didn't want to change it)
	// but we now run it at 100Hz (10ms period)
  // to match the SysTick QUANTA 
	// this way osDelay(10) means 100ms of real wall time
  // (10 ticks * 10ms each) which matches the cooperative/round-robin time slice
	
  // Enable clock access to TIM2 via APB1 enable register
  RCC->APB1ENR |= TIM2EN;

	// Prescaler: 16MHz / 1600 = 10kHz timer clock
  // Each timer tick is now 0.1ms
	TIM2->PSC = 1600 - 1;

  // Auto-reload: 100 timer ticks = 10ms period (100Hz interrupt rate)
  // This matches our scheduler quanta so sleep timers count down at the same rate
  TIM2->ARR = 100 - 1;

  // Clear counter to start from a known state
  TIM2->CNT = 0;

  // Enable timer
	TIM2->CR1 = CR1_CEN;

  // Enable update interrupt (fires when counter reaches ARR)
	TIM2->DIER |= DIER_UIE;

	// Register the interrupt with the NVIC so the CPU actually responds to it
	NVIC_EnableIRQ(TIM2_IRQn);
}

void osSemaphoreInit(int32_t *semaphore, int32_t value) {
	*semaphore = value;
}

/*
void osSemaphoreSet(int32_t *semaphore) {
	//Atomic
	__disable_irq();
	*semaphore += 1;
	__enable_irq();
}

void osSemaphoreWait(int32_t *semaphore) {
	__disable_irq();
	while (*semaphore <= 0) {
		__disable_irq();
		__enable_irq();
	}
	*semaphore -= 1;
	__enable_irq();
}
*/

// Blocking semaphore wait
// Unlike the old busy-wait version, this does NOT burn CPU cycles while waiting
// Instead, it marks the current task as blocked and yields the CPU
// The scheduler will then skip this task until osSemaphoreSet wakes it up
void osSemaphoreWait(int32_t *semaphore) {
    // We use a while(1) loop because when the task wakes up, 
    // it must re-check if the semaphore is still available
    // (Another higher-priority task might have taken it while we were waking up)
    while (1) {
        __disable_irq(); // Make this check atomic (no interrupts allowed)
        
        if (*semaphore > 0) {
            *semaphore -= 1;
            __enable_irq(); // Re-enable interrupts
            return;         
        }
        
        // If we are here, the semaphore is 0 
        // This task is now blocked waiting for this specific semaphore
        currentPt->blockedOn = semaphore;
        __enable_irq(); 
        
        // Yield the CPU to the next ready task
        // We will sleep here until osSemaphoreSet wakes us up
        osThreadYield();
    }
}

// Signals the semaphore to free it up
// It increments the semaphore counter and wakes up one blocked task (if any)
void osSemaphoreSet(int32_t *semaphore) {
    __disable_irq(); // Make this operation atomic
    
    // Increment the semaphore value so a waiting task can take it
    *semaphore += 1;
    
    // Look through all tasks to see if anyone is waiting for this semaphore
    for (int i = 0; i < current_task_count; i++) {
        if (tcbs[i].blockedOn == semaphore) {
            // We found a blocked task, wake it up by clearing its blockedOn pointer
            // Now the scheduler will consider it ready and allow it to run again
            tcbs[i].blockedOn = 0;
            
            // Break out of the loop so we only wake up one task per signal.
            // If multiple tasks are waiting, the next osSemaphoreSet will wake the next one
            break;
        }
    }
    __enable_irq(); // Re-enable interrupts
}


// Suspends the calling thread for the specified number of SysTick quanta
// This is a non-blocking delay; it lets other tasks run while this one sleeps
void osDelay(uint32_t quanta) {
    __disable_irq(); 
    
    // Set the sleep timer for the current task
    currentPt->sleepTime = quanta;
    __enable_irq();
    
    // Force a context switch immediately 
    // Since our sleepTime is > 0, the scheduler will skip us and run other ready tasks
    osThreadYield(); 
}

// Called from a hardware time-base ISR (that is TIM2) to decrement sleep counters
// We expose this as a function so main.c does not need direct access to the TCB array
// This keeps the kernel internals encapsulated and the API clean
void osTickDecrement(void) {
    // Decrement sleep timers for all tasks on every hardware tick
    // We do it here (in TIM2) because TIM2 fires independently of yields
    // SysTick-based decrement was unreliable since osThreadYield() resets SysTick->VAL
    // which also clears COUNTFLAG, causing some tick events to be missed
    for (int i = 0; i < current_task_count; i++) {
        // If the task is currently sleeping
        if (tcbs[i].sleepTime > 0) {
            // reduce its sleep time by 1 tick
            // When it reaches 0, the scheduler will allow it to run again
            tcbs[i].sleepTime--;
        }
    }
}

// This task runs only when all other tasks are blocked or sleeping
// It prevents the OS from crashing when there is nothing else to do
static void osIdleTask(void) {
   while (1) { 
        // Wait For Interrupt (WFI) puts the CPU into sleep mode to save power
        // It doesnt execute useless loops 
				// It will wake up instantly as soon as a hardware interrupt (like TIM2 tick or UART RX) occurs.
        __WFI(); 
    }
}

void osMutexInit(osMutex_t *mutex) {
    mutex->lock = 1; // 1 means available
    mutex->owner = 0; // no owner initially
}

void osMutexAcquire(osMutex_t *mutex) {
    // we use an infinite loop because after a task wakes up from yielding
    // it must try to acquire the lock again since another higher priority task
    // might have grabbed it in the split second before this task ran
    while (1) {
        // disable global interrupts to make the check and claim process atomic
        // this guarantees no context switch can happen while reading or writing the lock state
        __disable_irq(); 
        
        // lock value 1 means the mutex is completely free to be taken
        if (mutex->lock == 1) {
            // claim the mutex by setting lock to 0
            mutex->lock = 0;
            // officially register the current running task as the owner
            // this is crucial for the priority inheritance logic to know who to boost later
            mutex->owner = currentPt;
            // re-enable interrupts now that the atomic operation is done safely
            __enable_irq();
            // exit the function successfully
            return;
        }
        
        // if the code reaches here it means lock is 0 and someone else holds the mutex
        
        // priority inversion check and priority inheritance implementation
        // we check if an owner exists and if current task has a higher priority
        // remember that lower numerical value means higher priority in our system
        if (mutex->owner != 0 && currentPt->priority < mutex->owner->priority) {
            // we found a priority inversion situation
            // the low priority owner is blocking our high priority current task
            // we temporarily boost the owner's priority to match our high priority
            // this ensures the owner gets enough cpu time to finish its job and release the lock
            mutex->owner->priority = currentPt->priority;
        }
        
        // link the current task to this specific mutex so the scheduler knows why it is stuck
        // when the owner releases the lock it will check this pointer to wake us up
        currentPt->blockedOnMutex = mutex;
        
        // re-enable interrupts before going to sleep otherwise the system would freeze
        __enable_irq(); 
        
        // voluntarily give up the remaining cpu time slice
        // the scheduler will skip this task in the next cycles until blockedOnMutex becomes 0
        osThreadYield();
    }
}

void osMutexRelease(osMutex_t *mutex) {
    __disable_irq(); // make this atomic
    
    // safety check so only owner can release the mutex
    if (mutex->owner == currentPt) {
        mutex->lock = 1; // free the mutex
        mutex->owner = 0;
        
        // restore original priority since we are done with shared resource
        currentPt->priority = currentPt->basePriority;
        
        // wake up all tasks waiting for this mutex
        for (int i = 0; i < current_task_count; i++) {
            if (tcbs[i].blockedOnMutex == mutex) {
                tcbs[i].blockedOnMutex = 0; // unblock them
            }
        }
    }
    
    __enable_irq();
    
    // yield to see if we just woke up a higher priority task
    osThreadYield(); 
}

// CPU Usage
// Returns how many tasks exist in the system including the idle task
// Callers use this to loop over all tasks when generating a usage report
uint32_t osGetTaskCount(void) {
    return current_task_count;
}

// Returns the raw tick count for a given task index
// Out-of-range indices return 0 instead of crashing, defensive API design
// Note: this value is monotonic until osResetAllRunTicks is called
uint32_t osGetTaskRunTicks(uint32_t index) {
    if (index >= current_task_count) {
        return 0;
    }
    return tcbs[index].runTicks;
}

// Zeroes all task run tick counters atomically
// Called by the reporter after printing so the next report covers a fresh window
// Without this, percentages would be lifetime averages and slowly become meaningless
// as one-time startup bursts get diluted by long steady-state runtime
void osResetAllRunTicks(void) {
    __disable_irq();
    for (uint32_t i = 0; i < current_task_count; i++) {
        tcbs[i].runTicks = 0;
    }
    __enable_irq();
}


// Initializes a queue with a user-provided storage buffer
// Caller owns the buffer memory, so the kernel does not touch the heap
// Internal semaphore starts at 0 because the queue is empty at the beginning
// Any receive call before first send will correctly block
void osQueueInit(osQueue_t *q, uint8_t *buffer, uint32_t size) {
    q->buffer = buffer;
    q->size = size;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    osSemaphoreInit(&q->semaphore, 0);
}

// Writes one byte to the queue, non-blocking so ISR-safe
// Returns 0 if the queue is full, the caller decides whether to drop or retry
// We signal the semaphore at the end to wake up any task blocked on receive
// Interrupts are disabled during the update so head/count stay consistent
// even if this runs from an ISR that was itself interrupted (nested scenario)
uint8_t osQueueSend(osQueue_t *q, uint8_t data) {
    __disable_irq();

    // Drop the byte if there is no room, caller gets 0 and can log the overflow
    if (q->count >= q->size) {
        __enable_irq();
        return 0;
    }

    // Write to head, then advance head with wrap-around
    // Modulo keeps head inside [0, size) without needing a branch
    q->buffer[q->head] = data;
    q->head = (q->head + 1) % q->size;
    q->count++;

    __enable_irq();

    // Wake up one waiting receiver if any
    // osSemaphoreSet handles its own atomicity so we can call it outside the critical section
    osSemaphoreSet(&q->semaphore);
    return 1;
}

// Reads one byte from the queue, blocks until data is available
// Must be called from task context only, never from an ISR (it may yield)
// The internal semaphore puts the task to sleep if the queue is empty
// so there is zero CPU cost while waiting, exactly like osSemaphoreWait alone
void osQueueReceive(osQueue_t *q, uint8_t *out) {
    // Sleep here until a sender posts data and increments the semaphore
    osSemaphoreWait(&q->semaphore);

    __disable_irq();

    // Read from tail, then advance tail with wrap-around
    // count > 0 is guaranteed here because semaphore was > 0 when we woke up
    *out = q->buffer[q->tail];
    q->tail = (q->tail + 1) % q->size;
    q->count--;

    __enable_irq();
}

// Same ring buffer logic as osQueue but stores pointers
// User provides the backing array so the kernel never touches the heap
void osPtrQueueInit(osPtrQueue_t *q, void **buffer, uint32_t size) {
    q->buffer = buffer;
    q->size = size;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    osSemaphoreInit(&q->semaphore, 0);
}

// Non-blocking push, ISR-safe
// Returns 0 if full so the caller decides to drop, log, or retry
uint8_t osPtrQueueSend(osPtrQueue_t *q, void *item) {
    __disable_irq();
    if (q->count >= q->size) {
        __enable_irq();
        return 0;
    }
    q->buffer[q->head] = item;
    q->head = (q->head + 1) % q->size;
    q->count++;
    __enable_irq();

    osSemaphoreSet(&q->semaphore);   // wake one receiver if any
    return 1;
}

// Blocking pop, sleeps on internal semaphore while empty so zero CPU burn
// Task context only, never call from ISR (osSemaphoreWait may yield)
void osPtrQueueReceive(osPtrQueue_t *q, void **out) {
    osSemaphoreWait(&q->semaphore);
    __disable_irq();
    *out = q->buffer[q->tail];
    q->tail = (q->tail + 1) % q->size;
    q->count--;
    __enable_irq();
}

// Records which task overflowed so we can inspect it from the debugger
// Volatile, otherwise the compiler sees no reader and optimizes the write away
volatile tcbType *g_overflowedTask = 0;

// Called from inside the SysTick ISR when overflow is detected
// Memory is already in an unknown state, so we do not try to recover or print
// Safest action is to freeze cleanly so a debugger can show what went wrong
void osStackOverflowHandler(tcbType *offender) {
    __disable_irq();           // stop everything, prevent further corruption
    g_overflowedTask = offender;

    // __BKPT halts the debugger here during development
    // For production, swap this for NVIC_SystemReset() to recover by rebooting
    while (1) {
        __BKPT(0);
    }
}

// Head of the linked list of all timers ever started
// Linked list over a fixed array so there is no MAX_TIMERS ceiling to maintain
// and the user decides where timer storage lives (static, stack frame, etc.)
static osTimer_t *timer_list_head = 0;


// Queue that carries expired timer pointers from TIM2 ISR to the service task
#define TIMER_QUEUE_SIZE 8
static void *timer_queue_storage[TIMER_QUEUE_SIZE];
static osPtrQueue_t timer_service_queue;
static uint8_t timer_system_initialized = 0;

// Lazy init so the user does not need a separate kernel call in main
// First osTimerCreate triggers this, subsequent calls are no-ops
static void osTimerSystemInit(void) {
    if (!timer_system_initialized) {
        osPtrQueueInit(&timer_service_queue, timer_queue_storage, TIMER_QUEUE_SIZE);
        timer_system_initialized = 1;
    }
}

// Fills the struct but does not arm the timer
// User owns the osTimer_t storage, we just configure the fields
void osTimerCreate(osTimer_t *t, uint32_t period_ticks, uint8_t periodic, void (*cb)(void)) {
    osTimerSystemInit();
    t->period = period_ticks;
    t->remaining = period_ticks;
    t->callback = cb;
    t->periodic = periodic;
    t->active = 0;
    t->next = 0;
}

// Links timer into the active list and arms its countdown
// Prepend is O(1) and order does not matter since the ISR walks everyone anyway
// The !active check prevents cycles if Start is called twice on the same timer
void osTimerStart(osTimer_t *t) {
    __disable_irq();
    if (!t->active) {
        t->next = timer_list_head;
        timer_list_head = t;
    }
    t->remaining = t->period;
    t->active = 1;
    __enable_irq();
}

// Pauses without unlinking, ISR skips inactive timers during walk
// We do not unlink because that would require finding the previous node (O(n))
// and Stop should be cheap and ISR-callable
void osTimerStop(osTimer_t *t) {
    __disable_irq();
    t->active = 0;
    __enable_irq();
}

// Restarts countdown without toggling active state
// Classic use case: watchdog timer that gets kicked on every healthy event
void osTimerReset(osTimer_t *t) {
    __disable_irq();
    t->remaining = t->period;
    t->active = 1;
    __enable_irq();
}

// Called from TIM2 ISR on every tick, walks the list once
// Expired timers are queued to the service task, NOT executed here,
// so the ISR stays short and callbacks run in safe task context
// Periodic reload happens here so the service task stays fire-and-forget
void osTimerTick(void) {
    osTimer_t *t = timer_list_head;
    while (t != 0) {
        if (t->active && t->remaining > 0) {
            t->remaining--;
            if (t->remaining == 0) {
                osPtrQueueSend(&timer_service_queue, t);   // hand off, ignore full
                if (t->periodic) {
                    t->remaining = t->period;   // reload, keep counting
                } else {
                    t->active = 0;              // one-shot, disarm after firing
                }
            }
        }
        t = t->next;
    }
}

// The service task, added to kernel like any other task at highest priority
// Sleeps on the queue, wakes per expiration, runs callback in task context
// Null check guards against misconfigured timers with no callback set
void osTimerServiceTask(void) {
    osTimer_t *expired;
    while (1) {
        osPtrQueueReceive(&timer_service_queue, (void **)&expired);
        if (expired->callback != 0) {
            expired->callback();
        }
    }
}

void osAccountCurrentTick(void) {
    if (currentPt != 0) {
        currentPt->runTicks++;
    }
}