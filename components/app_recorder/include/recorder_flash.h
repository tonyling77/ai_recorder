#pragma once

/* Do a write of the flash on a task whose stack is in the internal memory.
 *
 * The interface of the board cannot write the flash itself. The stack of
 * the render task is in the external memory, and the flash is written with
 * the cache of the flash turned off. The external memory is out of reach
 * while that cache is off, so the writing framework refuses the write from
 * a task whose stack is there:
 *
 *     assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
 *     cache_utils.c:114 (esp_task_stack_is_sane_cache_disabled())
 *
 * and the board stops.
 *
 * The write is done by a task of its own, whose stack is in the internal
 * memory, and the task goes away when the write is done.
 *
 * The write is a callback, so the caller does not have to know how the
 * write is done.
 *
 * The argument belongs to this function: it has to be on the heap, because
 * the write happens later and on another task. It is alive while the
 * callback runs, and it is freed when the write is done, whether the write
 * worked or not. The callback must not free it.
 *
 * The write happens in the background. A caller that has to know whether it
 * worked has to wait for the answer some other way. */
void recorder_flash_write(void (*write)(void *arg), void *arg);
