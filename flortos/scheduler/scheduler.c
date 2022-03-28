/*
 * scheduler.c
 *
 *  Created on: Mar 27, 2022
 *      Author: ftobler
 */

#include "scheduler.h"
#include "stm32f0xx.h"



static SchedulerTask_t* currentTask;
static SchedulerTask_t* nextTask;


static SchedulerTask_t tasks[8] = {0};
static uint32_t highestTask = 0;

static void scheduler_work();


void scheduler_init() {
	//set the PendSV interrupt priority to the lowest level 0xF
    *(uint32_t volatile *)0xE000ED20 |= (0xFFU << 16);
}

void scheduler_addTask(uint32_t id, SchedulerTaskFunction function, uint8_t* stackBuffer, uint32_t stackSize) {
	SchedulerTask_t* task = &tasks[id];
	if (id > highestTask) {
		highestTask = id;
	}

	//calculate the stack pointer, the stack grows upside down
	uint32_t* stackPointer = (uint32_t*)((uint32_t)stackBuffer + stackSize);

    //not put data on stack so that if it is popped the task is ready to run
	*(--stackPointer) = 1U << 24; //xPSR (put by ISR)
	*(--stackPointer) = (uint32_t)function; //PC (put by ISR-HW)
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

	//put current stack pointer position (including the put data) to the task struct
	task->stackPointer = stackPointer;

	//put the priority
	task->timeout = 0;
	task->eventFlags = 0;
	task->eventMask = 0;
	task->state = STATE_READY;
}

void scheduler_join() {
	__disable_irq();
	scheduler_systick_handler();
	__enable_irq();
}


void scheduler_task_sleep(uint32_t time) {
	SchedulerTask_t* task = currentTask;
	task->state = STATE_WAIT_TIME;
	task->timeout = time;
	scheduler_work();
}


uint32_t scheduler_event_wait(uint32_t eventWaitMask) {

}


void scheduler_event_set(uint32_t id, uint32_t eventSetMask) {

}


static void scheduler_work() {
	uint32_t id = highestTask;
	SchedulerTask_t* task = &tasks[id];
	//go through every task id, starting from the highest priority task
	while (id) {
		//update task
		if (task->state == STATE_WAIT_TIME) {
			//handle tasks waiting conditions
			task->timeout--;
			if (!task->timeout) {
				task->state = STATE_READY;
			}
		}
		//if task is runnable then run it.
		if (task->state == STATE_READY) {
			//found task to run. Exit loop.
			nextTask = task;
			break;
		}
		//loop variables
		id--;
		task--;
	}
	if (id == 0) {
		//when nothing else to do run idle task.
		//since loop has gotten to id=0 the idle task is already on the pointer.
		nextTask = task;
	}

	//switch task if needed
	if (currentTask != nextTask) {
		//enable pendSV isr
		*(uint32_t volatile *)0xE000ED04 = (1U << 28);
	}
}

void scheduler_systick_handler() {
	uwTick++;
	scheduler_work();
}

__attribute((naked)) void scheduler_pendSV_handler() {
	__disable_irq();
	register uint32_t* stackPointer asm ("sp");
	register uint32_t register8 asm ("r8");
	register uint32_t register9 asm ("r9");
	register uint32_t register10 asm ("r10");
	register uint32_t register11 asm ("r11");
	if (currentTask != nextTask && currentTask) {
		asm volatile("push {r4-r7}"); //push additional registers
		*(--stackPointer) = register8; //these registers can not be handled by push
		*(--stackPointer) = register9;
		*(--stackPointer) = register10;
		*(--stackPointer) = register11;
		currentTask->stackPointer = stackPointer;
	}

	stackPointer = nextTask->stackPointer;
	*(stackPointer++) = register11; //these registers can not be handled by push
	*(stackPointer++) = register10;
	*(stackPointer++) = register9;
	*(stackPointer++) = register8;
	asm volatile("pop {r4-r7}"); //pop additional registers
	currentTask = nextTask;

    __enable_irq();

    asm volatile("BX lr");  //return
}
