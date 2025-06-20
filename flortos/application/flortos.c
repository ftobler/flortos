/*
 * scheduler.c
 *
 *  Created on: Mar 27, 2022
 *      Author: ftobler
 */

#include "flortos.h"
#include "stm32_hal.h"
#include "flortos_conf.h"


static volatile SchedulerTask_t* currentTask = 0;
static volatile SchedulerTask_t* nextTask;


static SchedulerTask_t tasks[MAY_NUMBER_OF_TASKS] = {0};
static uint32_t highestTask = 0;

static void scheduler_work();
static void scheduler_task_time_update();


void scheduler_init() {
	// set the PendSV interrupt priority to the lowest level 0xF
    *(uint32_t volatile *)0xE000ED20 |= (0xFFU << 16);
}

void scheduler_addTask(uint32_t id, SchedulerTaskFunction function, uint8_t* stackBuffer, uint32_t stackSize) {
	SchedulerTask_t* task = &tasks[id];
	if (id > highestTask) {
		highestTask = id;
	}

	// task id must be in range
	if (id >= MAY_NUMBER_OF_TASKS) {
		while (1) {
			//trap. Make sure task id is in range. Configure more tasks in _conf.h
		}
	}

	// stack must be 4 byte aligned or ARM will not be happy. Not aligning may impact speed, or crash the core.
	if ((uint32_t)stackBuffer & 0x03) {
		while (1) {
			//trap. Make sure stack is 4 byte aligned.
		}
	}

	// calculate the stack pointer, the stack grows upside down
	uint32_t* stackPointer = (uint32_t*)((uint32_t)stackBuffer + stackSize);

	#ifdef SCHEDULER_ARCHITECURE_M0plus

    // not put data on stack so that if it is popped the task is ready to run
	*(--stackPointer) = 1U << 24; // xPSR (put by ISR)
	*(--stackPointer) = (uint32_t)function; // PC (put by ISR-HW)
	*(--stackPointer) = 0x0000000E; // LR  (put by ISR-HW)
	*(--stackPointer) = 0x0000000C; // R12 (put by ISR-HW)
	*(--stackPointer) = 0x00000003; // R3  (put by ISR-HW)
	*(--stackPointer) = 0x00000002; // R2  (put by ISR-HW)
	*(--stackPointer) = 0x00000001; // R1  (put by ISR-HW)
	*(--stackPointer) = 0x00000000; // R0  (put by ISR-HW)
	*(--stackPointer) = 0x0000000B; // R11  (put by ISR-SW)
	*(--stackPointer) = 0x0000000A; // R10  (put by ISR-SW)
	*(--stackPointer) = 0x00000009; // R9   (put by ISR-SW)
	*(--stackPointer) = 0x00000008; // R8   (put by ISR-SW)
	*(--stackPointer) = 0x00000007; // R7   (put by ISR-SW)
	*(--stackPointer) = 0x00000006; // R6   (put by ISR-SW)
	*(--stackPointer) = 0x00000005; // R5   (put by ISR-SW)
	*(--stackPointer) = 0x00000004; // R4   (put by ISR-SW)

	#endif
	#ifdef SCHEDULER_ARCHITECURE_M4F

    //not put data on stack so that if it is popped the task is ready to run
	*(--stackPointer) = 1U << 24; // xPSR (put by ISR)
	*(--stackPointer) = (uint32_t)function; // PC (put by ISR-HW)
	*(--stackPointer) = 0x0000000E; // LR  (put by ISR-HW) //also r14
	*(--stackPointer) = 0x0000000C; // R12 (put by ISR-HW)
	*(--stackPointer) = 0x00000003; // R3  (put by ISR-HW)
	*(--stackPointer) = 0x00000002; // R2  (put by ISR-HW)
	*(--stackPointer) = 0x00000001; // R1  (put by ISR-HW)
	*(--stackPointer) = 0x00000000; // R0  (put by ISR-HW)

	for (int i = 16;i < 32; i++) {
		*(--stackPointer) = 0; // FPU register   (put by ISR-SW)
	}

	*(--stackPointer) = 0x00000007; // R7   (put by ISR-SW)
	*(--stackPointer) = 0x00000006; // R6   (put by ISR-SW)
	*(--stackPointer) = 0x00000005; // R5   (put by ISR-SW)
	*(--stackPointer) = 0x00000004; // R4   (put by ISR-SW)
	*(--stackPointer) = 0x00000008; // R8   (put by ISR-SW)
	*(--stackPointer) = 0x00000009; // R9   (put by ISR-SW)
	*(--stackPointer) = 0x0000000A; // R10  (put by ISR-SW)
	*(--stackPointer) = 0x0000000B; // R11  (put by ISR-SW)

	#endif

	// not put data on stack so that if it is popped the task is ready to run

	// put current stack pointer position (including the put data) to the task struct
	task->stackPointer = stackPointer;

	// put the priority
	task->timeout = 0;
	task->eventFlags = 0;
	task->eventMask = 0;
	task->state = STATE_READY;
}

void scheduler_join() {
	__disable_irq();
	scheduler_work();
	__enable_irq();
}


void scheduler_task_sleep(uint32_t time) {
	volatile SchedulerTask_t* task = currentTask;
	task->timeout = time;
	task->state = STATE_WAIT_TIME;
	scheduler_work();
}


uint32_t scheduler_event_wait(uint32_t eventWaitMask) {
	volatile SchedulerTask_t* task = currentTask;
	task->eventMask = eventWaitMask;
	task->state = STATE_WAIT_FLAG;
	scheduler_work();
	uint32_t events = task->eventFlags;
	task->eventFlags &= ~eventWaitMask;  // clear the flags the task was waiting for
	return events;
}

uint32_t scheduler_event_wait_timeout(uint32_t eventWaitMask, uint32_t time) {
	volatile SchedulerTask_t* task = currentTask;
	task->eventMask = eventWaitMask;
	task->timeout = time;
	task->state = STATE_WAIT_FLAG;
	scheduler_work();
	uint32_t events = task->eventFlags;
	task->eventFlags &= ~eventWaitMask;  // clear the flags the task was waiting for
	return events;
}


void scheduler_event_set(uint32_t id, uint32_t eventSetMask) {
	//set remote tasks flags
	tasks[id].eventFlags |= eventSetMask;
    scheduler_work();
}


void scheduler_event_clear(uint32_t eventMask) {
	volatile SchedulerTask_t* task = currentTask;
	task->eventFlags &= ~eventMask;
}


__attribute__((optimize("O0")))
static void scheduler_work() {
	uint32_t id = highestTask;
	SchedulerTask_t* task = &tasks[id];
	// go through every task id, starting from the highest priority task
	while (id) {
		// update task
		if (task->state == STATE_WAIT_FLAG) {
			// check if at least one flag which is masked is set
			if (task->eventFlags & task->eventMask) {
				// task is ready to run
				task->state = STATE_READY;
			}
		}
		// if task is runnable then run it.
		if (task->state == STATE_READY) {
			// found task to run. Exit loop.
			nextTask = task;
			break;
		}
		// loop variables
		id--;
		task--;
	}
	if (id == 0) {
		// when nothing else to do run idle task.
		// since loop has gotten to id=0 the idle task is already on the pointer.
		nextTask = task;
	}

	// switch task if needed
	if (currentTask != nextTask) {
		// enable pendSV isr
		*(uint32_t volatile *)0xE000ED04 = (1U << 28);
	}
}


static void scheduler_task_time_update() {
	uint32_t id = highestTask;
	SchedulerTask_t* task = &tasks[id];
	// go through every task id, starting from the highest priority task
	while (id) {
		// update task
		if (task->timeout) {
			if (task->timeout == 1) {
				task->timeout = 0;
				task->state = STATE_READY;
			} else {
				task->timeout--;
			}
		}

		// loop variables
		id--;
		task--;
	}
}

void scheduler_systick_handler() {
	uwTick++;
	scheduler_task_time_update();
	scheduler_work();
}

__attribute((naked))
__attribute__((optimize("O0")))
void scheduler_pendSV_handler() {

	#ifdef SCHEDULER_ARCHITECURE_M0plus

	__disable_irq();
	volatile register uint32_t* stackPointer asm ("sp");

	if (currentTask) {
		asm volatile("push {r4-r7}"); // push additional registers
		asm volatile("mov r3, r8  \n push {r3}" : : : "r3","memory"); // these registers can not be handled by push
		asm volatile("mov r3, r9  \n push {r3}" : : : "r3","memory"); // "r3" in the clobber list informs the compiler that r3 will be used in this section
		asm volatile("mov r3, r10 \n push {r3}" : : : "r3","memory"); // "memory" in the clobber list informs the compiler that memory content may have changed
		asm volatile("mov r3, r11 \n push {r3}" : : : "r3","memory"); // "sp" in clobber list is deprecated and not needed
		currentTask->stackPointer = (uint32_t*)stackPointer;
	}

	stackPointer = nextTask->stackPointer;
	asm volatile("pop {r3}\n mov r11, r3" : : : "r3","memory");// these registers can not be handled by push
	asm volatile("pop {r3}\n mov r10, r3" : : : "r3","memory");
	asm volatile("pop {r3}\n mov  r9, r3" : : : "r3","memory");
	asm volatile("pop {r3}\n mov  r8, r3" : : : "r3","memory");
	asm volatile("pop {r4-r7}"); // pop additional registers
	currentTask = nextTask;

    __enable_irq();
    asm volatile("BX lr");  // return

	#endif

	#ifdef SCHEDULER_ARCHITECURE_M4F

		__disable_irq();
	volatile register uint32_t* stackPointer asm ("sp");
	asm volatile("isb"); // Flush instruction pipeline

	if (currentTask) {
		// Save FPU state (floating-point registers S16-S31)
		asm volatile("vstmdb sp!, {s16-s31}" : : : "memory"); // Store FPU registers
		// Save general-purpose registers R4-R11
		asm volatile("push {r4-r11}");
		// Memory barriers to ensure everything is written before switching
		asm volatile("dsb \n isb"); // Ensures memory accesses and pipeline flushing
		// Save stack pointer of the current task
		currentTask->stackPointer = (uint32_t*)stackPointer;
	}

	// Switch to the new task's stack pointer
	stackPointer = nextTask->stackPointer;

	// Ensure the stack is properly aligned (optional, depending on alignment settings)
	asm volatile("dsb \n isb"); // Synchronization to make sure all changes are applied
	// Restore general-purpose registers R4-R11
	asm volatile("pop {r4-r11}");
	// Restore FPU state (floating-point registers S16-S31)
	asm volatile("vldmia sp!, {s16-s31}" : : : "memory");

	// Update the current task to the next task
	currentTask = nextTask;

	__enable_irq();

	// Make sure no pending instructions exist before returning
	asm volatile("dsb \n isb"); // Synchronize memory and instruction pipelines before returning
	asm volatile("BX lr");  // Return from handler

	#endif
}
