/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * (c) 2013, 2014 Henner Zeller <h.zeller@acm.org>
 *
 * This file is part of BeagleG. http://github.com/hzeller/beagleg
 *
 * BeagleG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BeagleG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BeagleG.  If not, see <http://www.gnu.org/licenses/>.
 */

// Implementation of the MotionQueue interfacing the PRU.
// We are using some shared memory between CPU and PRU to communicate.

#include "motion-queue.h"

#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pruss_intc_mapping.h>
#include <prussdrv.h>
#include <strings.h>

// Shared between us and PRU
#include "motor-interface-constants.h"

// Generated PRU code from motor-interface-pru.p
#include "motor-interface-pru_bin.h"

#define DEBUG_QUEUE
#define PRU_NUM 0

#define GPIO_0_ADDR 0x44e07000	// memory space mapped to GPIO-0
#define GPIO_1_ADDR 0x4804c000	// memory space mapped to GPIO-1
#define GPIO_MMAP_SIZE 0x2000

#define GPIO_OE 0x134           // setting direction.
#define GPIO_DATAOUT 0x13c      // Set all the bits

#define MOTOR_OUT_BITS				\
  ((uint32_t) ( (1<<MOTOR_1_STEP_BIT)		\
		| (1<<MOTOR_2_STEP_BIT)		\
		| (1<<MOTOR_3_STEP_BIT)		\
		| (1<<MOTOR_4_STEP_BIT)		\
		| (1<<MOTOR_5_STEP_BIT)		\
		| (1<<MOTOR_6_STEP_BIT)		\
		| (1<<MOTOR_7_STEP_BIT)		\
		| (1<<MOTOR_8_STEP_BIT) ))

// Direction bits are a contiguous chunk, just a bit shifted.
#define DIRECTION_OUT_BITS ((uint32_t) (0xFF << DIRECTION_GPIO1_SHIFT))

// The communication with the PRU. We memory map the static RAM in the PRU
// and write stuff into it from here. Mostly this is a ring-buffer with
// commands to execute, but also configuration data, such as what to do when
// an endswitch fires.
struct PRUCommunication {
  volatile struct MotionSegment ring_buffer[QUEUE_LEN];
};

// This is a physical singleton, so simply reflect that here.
static volatile struct PRUCommunication *pru_data_;
static unsigned int queue_pos_;

// GPIO registers.
static volatile uint32_t *gpio_0 = NULL;
static volatile uint32_t *gpio_1 = NULL;

static int map_gpio() {
  int fd = open("/dev/mem", O_RDWR);
  gpio_0 = (volatile uint32_t*) mmap(0, GPIO_MMAP_SIZE, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, GPIO_0_ADDR);
  if (gpio_0 == MAP_FAILED) { perror("mmap() GPIO-0"); return 0; }
  gpio_1 = (volatile uint32_t*) mmap(0, GPIO_MMAP_SIZE, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, GPIO_1_ADDR);
  if (gpio_1 == MAP_FAILED) { perror("mmap() GPIO-1"); return 0; }
  close(fd);
  return 1;
}

static void unmap_gpio() {
  munmap((void*)gpio_0, GPIO_MMAP_SIZE); gpio_0 = NULL;
  munmap((void*)gpio_1, GPIO_MMAP_SIZE); gpio_1 = NULL;
}

static volatile struct PRUCommunication *map_pru_communication() {
  void *result;
  prussdrv_map_prumem(PRUSS0_PRU0_DATARAM, &result);
  bzero(result, sizeof(*pru_data_));
  pru_data_ = (struct PRUCommunication*) result;
  for (int i = 0; i < QUEUE_LEN; ++i) {
    pru_data_->ring_buffer[i].state = STATE_EMPTY;
  }
  queue_pos_ = 0;
  return pru_data_;
}

static volatile struct MotionSegment *next_queue_element() {
  queue_pos_ %= QUEUE_LEN;
  while (pru_data_->ring_buffer[queue_pos_].state != STATE_EMPTY) {
    prussdrv_pru_wait_event(PRU_EVTOUT_0);
    prussdrv_pru_clear_event(PRU_EVTOUT_0, PRU0_ARM_INTERRUPT);
  }
  return &pru_data_->ring_buffer[queue_pos_++];
}

static void pru_motor_enable_nowait(char on) {
  // Enable pin is inverse logic: -EN
  gpio_1[GPIO_DATAOUT/4] = on ? 0 : (1 << MOTOR_ENABLE_GPIO1_BIT);
}

#ifdef DEBUG_QUEUE
static void DumpMotionSegment(volatile const struct MotionSegment *e) {
  if (e->state == STATE_EXIT) {
    fprintf(stderr, "enqueue[%02td]: EXIT\n", e - pru_data_->ring_buffer);
  } else {
    struct MotionSegment copy = *e;
    fprintf(stderr, "enqueue[%02td]: dir:0x%02x s:(%5d + %5d + %5d) = %5d "
	    "ad: %d; td: %d ",
	    e - pru_data_->ring_buffer, copy.direction_bits,
	    copy.loops_accel, copy.loops_travel, copy.loops_decel,
	    copy.loops_accel + copy.loops_travel + copy.loops_decel,
	    copy.hires_accel_cycles >> DELAY_CYCLE_SHIFT,
	    copy.travel_delay_cycles);
#if 1
    for (int i = 0; i < MOTION_MOTOR_COUNT; ++i) {
      if (copy.fractions[i] == 0) continue;  // not interesting.
      fprintf(stderr, "f%d:0x%08x ", i, copy.fractions[i]);
    }
#endif
    fprintf(stderr, "\n");
  }
}
#endif

static void pru_enqueue_segment(struct MotionSegment *element) {
  const uint8_t state_to_send = element->state;
  assert(state_to_send != STATE_EMPTY);  // forgot to set proper state ?
  // Initially, we copy everything with 'STATE_EMPTY', then flip the state
  // to avoid a race condition while copying.
  element->state = STATE_EMPTY;
  volatile struct MotionSegment *queue_element = next_queue_element();
  *queue_element = *element;

  // Fully initialized. Tell busy-waiting PRU by flipping the state.
  queue_element->state = state_to_send;
#ifdef DEBUG_QUEUE
  DumpMotionSegment(queue_element);
#endif
}

static void pru_wait_queue_empty() {
  const unsigned int last_insert_position = (queue_pos_ - 1) % QUEUE_LEN;
  while (pru_data_->ring_buffer[last_insert_position].state != STATE_EMPTY) {
    prussdrv_pru_wait_event(PRU_EVTOUT_0);
    prussdrv_pru_clear_event(PRU_EVTOUT_0, PRU0_ARM_INTERRUPT);
  }
}

static void pru_shutdown(char flush_queue) {
  if (flush_queue) {
    struct MotionSegment end_element = {0};
    end_element.state = STATE_EXIT;
    pru_enqueue_segment(&end_element);
    pru_wait_queue_empty();
  }
  prussdrv_pru_disable(PRU_NUM);
  prussdrv_exit();
  pru_motor_enable_nowait(0);
  unmap_gpio();
}

int init_pru_motion_queue(struct MotionQueue *queue) {
  if (!map_gpio()) {
    fprintf(stderr, "Couldn't mmap() GPIO ranges.\n");
    return 1;
  }

  // Prepare all the pins we need for output. All the other bits are inputs,
  // so the STOP switch bits are automatically prepared for input.
  gpio_0[GPIO_OE/4] = ~(MOTOR_OUT_BITS | (1 << AUX_1_BIT) | (1 << AUX_2_BIT));
  gpio_1[GPIO_OE/4] = ~(DIRECTION_OUT_BITS | (1 << MOTOR_ENABLE_GPIO1_BIT));

  pru_motor_enable_nowait(0);  // motors off initially.

  tpruss_intc_initdata pruss_intc_initdata = PRUSS_INTC_INITDATA;
  prussdrv_init();

  /* Get the interrupt initialized */
  int ret = prussdrv_open(PRU_EVTOUT_0);  // allow access.
  if (ret) {
    fprintf(stderr, "prussdrv_open() failed (%d)\n", ret);
    return ret;
  }
  prussdrv_pruintc_init(&pruss_intc_initdata);
  if (map_pru_communication() == NULL) {
    fprintf(stderr, "Couldn't map PRU memory for queue.\n");
    return 1;
  }

  prussdrv_pru_write_memory(PRUSS0_PRU0_IRAM, 0, PRUcode, sizeof(PRUcode));
  prussdrv_pru_enable(0);

  // Initialize operations.
  queue->enqueue = &pru_enqueue_segment;
  queue->wait_queue_empty = &pru_wait_queue_empty;
  queue->motor_enable = &pru_motor_enable_nowait;
  queue->shutdown = &pru_shutdown;

  return 0;
}

// Dummy implementation of a MotionQueue. For convenience, just implemented here.
static void dummy_enqueue(struct MotionSegment *segment) {}
static void dummy_wait_queue_empty() {}
static void dummy_motor_enable(char on) {}
static void dummy_shutdown(char do_flush) {}

void init_dummy_motion_queue(struct MotionQueue *queue) {
  queue->enqueue = &dummy_enqueue;
  queue->wait_queue_empty = &dummy_wait_queue_empty;
  queue->motor_enable = &dummy_motor_enable;
  queue->shutdown = &dummy_shutdown;
}