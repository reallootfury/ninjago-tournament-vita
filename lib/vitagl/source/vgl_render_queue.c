/*
 * This file is part of vitaGL
 * Copyright 2026 vitaGL contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * vgl_render_queue.c:
 * Render-thread finalization queue
 */

#include "shared.h"
#include "vgl_optimizer.h"

#ifdef HAVE_RENDER_QUEUE

#define VGL_RENDER_QUEUE_SIZE 64

typedef struct {
	vglRenderTask task;
	void *userdata;
} vgl_render_task;

static vgl_render_task render_queue[VGL_RENDER_QUEUE_SIZE];
static volatile uint32_t render_head = 0;
static volatile uint32_t render_tail = 0;
static volatile int render_lock = 0;

static inline __attribute__((always_inline)) void lock_queue(void) {
	while (__sync_lock_test_and_set(&render_lock, 1))
		sceKernelDelayThread(1);
}

static inline __attribute__((always_inline)) void unlock_queue(void) {
	__sync_lock_release(&render_lock);
}

#endif

GLboolean vglQueueRenderTask(vglRenderTask task, void *userdata) {
#ifdef HAVE_RENDER_QUEUE
	if (!task)
		return GL_FALSE;

	lock_queue();
	uint32_t next = (render_tail + 1) % VGL_RENDER_QUEUE_SIZE;
	if (next == render_head) {
		unlock_queue();
		return GL_FALSE;
	}
	render_queue[render_tail].task = task;
	render_queue[render_tail].userdata = userdata;
	render_tail = next;
	unlock_queue();
	return GL_TRUE;
#else
	return GL_FALSE;
#endif
}

void vglFlushRenderTasks(void) {
#ifdef HAVE_RENDER_QUEUE
	for (;;) {
		lock_queue();
		if (render_head == render_tail) {
			unlock_queue();
			break;
		}
		vgl_render_task task = render_queue[render_head];
		render_head = (render_head + 1) % VGL_RENDER_QUEUE_SIZE;
		unlock_queue();
		task.task(task.userdata);
		vgl_optimizer_render_task();
	}
#endif
}
