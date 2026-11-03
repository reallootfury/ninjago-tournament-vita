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
 * vgl_async.c:
 * Bounded CPU-only preparation worker
 */

#include "shared.h"

#define VGL_ASYNC_QUEUE_SIZE 32
#define VGL_ASYNC_STACK_SIZE 64 * 1024

typedef struct {
	vglAsyncTask task;
	void *userdata;
} vgl_async_task;

#ifdef HAVE_ASYNC_PREP
static vgl_async_task async_queue[VGL_ASYNC_QUEUE_SIZE];
static volatile uint32_t async_head = 0;
static volatile uint32_t async_tail = 0;
static volatile uint32_t async_pending = 0;
static volatile int async_lock = 0;
static volatile GLboolean async_running = GL_FALSE;
static SceUID async_thread = -1;
static SceUID async_sema = -1;

static inline __attribute__((always_inline)) void lock_queue(void) {
	while (__sync_lock_test_and_set(&async_lock, 1))
		sceKernelDelayThread(1);
}

static inline __attribute__((always_inline)) void unlock_queue(void) {
	__sync_lock_release(&async_lock);
}

static int async_worker(unsigned int args, void *argp) {
	while (async_running) {
		sceKernelWaitSema(async_sema, 1, NULL);
		if (!async_running)
			break;

		for (;;) {
			lock_queue();
			if (async_head == async_tail) {
				unlock_queue();
				break;
			}
			vgl_async_task task = async_queue[async_head];
			async_head = (async_head + 1) % VGL_ASYNC_QUEUE_SIZE;
			unlock_queue();
			task.task(task.userdata);
			__sync_fetch_and_sub(&async_pending, 1);
		}
	}
	return 0;
}
#endif

GLboolean vglInitAsyncWorker(int priority, int affinity) {
#ifdef HAVE_ASYNC_PREP
	if (async_running)
		return GL_TRUE;

	async_head = async_tail = async_pending = 0;
	async_sema = sceKernelCreateSema("vgl async sema", 0, 0, VGL_ASYNC_QUEUE_SIZE, NULL);
	if (async_sema < 0)
		return GL_FALSE;

	async_thread = sceKernelCreateThread("vgl async worker", async_worker, priority, VGL_ASYNC_STACK_SIZE, 0, affinity, NULL);
	if (async_thread < 0) {
		sceKernelDeleteSema(async_sema);
		async_sema = -1;
		return GL_FALSE;
	}

	async_running = GL_TRUE;
	if (sceKernelStartThread(async_thread, 0, NULL) < 0) {
		async_running = GL_FALSE;
		sceKernelDeleteThread(async_thread);
		sceKernelDeleteSema(async_sema);
		async_thread = async_sema = -1;
		return GL_FALSE;
	}
	return GL_TRUE;
#else
	return GL_FALSE;
#endif
}

GLboolean vglQueueAsyncTask(vglAsyncTask task, void *userdata) {
#ifdef HAVE_ASYNC_PREP
	if (!async_running || !task)
		return GL_FALSE;

	lock_queue();
	uint32_t next = (async_tail + 1) % VGL_ASYNC_QUEUE_SIZE;
	if (next == async_head) {
		unlock_queue();
		return GL_FALSE;
	}
	async_queue[async_tail].task = task;
	async_queue[async_tail].userdata = userdata;
	async_tail = next;
	__sync_fetch_and_add(&async_pending, 1);
	unlock_queue();
	sceKernelSignalSema(async_sema, 1);
	return GL_TRUE;
#else
	return GL_FALSE;
#endif
}

void vglWaitAsyncTasks(void) {
#ifdef HAVE_ASYNC_PREP
	while (async_pending)
		sceKernelDelayThread(1000);
#endif
}

void vglTermAsyncWorker(void) {
#ifdef HAVE_ASYNC_PREP
	if (!async_running)
		return;
	vglWaitAsyncTasks();
	async_running = GL_FALSE;
	sceKernelSignalSema(async_sema, 1);
	sceKernelWaitThreadEnd(async_thread, NULL, NULL);
	if (async_thread >= 0)
		sceKernelDeleteThread(async_thread);
	if (async_sema >= 0)
		sceKernelDeleteSema(async_sema);
	async_thread = async_sema = -1;
#endif
}
