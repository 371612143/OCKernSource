/*
 * Copyright (c) 2008-2013 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

#include "internal.h"

#undef dispatch_once
#undef dispatch_once_f


typedef struct _dispatch_once_waiter_s {
	volatile struct _dispatch_once_waiter_s *volatile dow_next;
	dispatch_thread_event_s dow_event;
	mach_port_t dow_thread;
} *_dispatch_once_waiter_t;

#define DISPATCH_ONCE_DONE ((_dispatch_once_waiter_t)~0l)

#ifdef __BLOCKS__
void
dispatch_once(dispatch_once_t *val, dispatch_block_t block)
{
	dispatch_once_f(val, block, _dispatch_Block_invoke(block));
}
#endif

#if DISPATCH_ONCE_INLINE_FASTPATH
#define DISPATCH_ONCE_SLOW_INLINE inline DISPATCH_ALWAYS_INLINE
#else
#define DISPATCH_ONCE_SLOW_INLINE DISPATCH_NOINLINE
#endif // DISPATCH_ONCE_INLINE_FASTPATH

DISPATCH_ONCE_SLOW_INLINE
static void
dispatch_once_f_slow(dispatch_once_t *val, void *ctxt, dispatch_function_t func)
{
#if DISPATCH_GATE_USE_FOR_DISPATCH_ONCE
	dispatch_once_gate_t l = (dispatch_once_gate_t)val;

	if (_dispatch_once_gate_tryenter(l)) {
		_dispatch_client_callout(ctxt, func);
		_dispatch_once_gate_broadcast(l);
	} else {
		_dispatch_once_gate_wait(l);
	}
#else
	_dispatch_once_waiter_t volatile *vval = (_dispatch_once_waiter_t*)val;
	struct _dispatch_once_waiter_s dow = { };
	_dispatch_once_waiter_t tail = &dow, next, tmp;
	dispatch_thread_event_t event;
	

	// *vval 是否等于NULL? 是，则返回true，并将*vval置为tail。如果不是，返回false（第一次进入，*vval == NULL, 之后又其他线程进入，则进入else分支） 如果之后在没有其他线程进入，则val的值一直会保持tail
	if (os_atomic_cmpxchg(vval, NULL, tail, acquire)) {
		dow.dow_thread = _dispatch_tid_self();
		//调用dispatch_once内block回调
		_dispatch_client_callout(ctxt, func);

		next = (_dispatch_once_waiter_t)_dispatch_once_xchg_done(val);
		 //利用while循环不断处理未完成的更改请求，直到所有更改结束
		while (next != tail) {
			tmp = (_dispatch_once_waiter_t)_dispatch_wait_until(next->dow_next);
			event = &next->dow_event;
			next = tmp;
			_dispatch_thread_event_signal(event);
		}
	} else {
		_dispatch_thread_event_init(&dow.dow_event);
		next = *vval;
		for (;;) {
			//遍历每一个后续请求，如果状态已经是Done，直接进行下一个，同时该状态检测还用于避免在后续wait之前，信号量已经发出(signal)造成的死锁
			if (next == DISPATCH_ONCE_DONE) {
				break;
			}
			//如果当前dispatch_once执行的block没有结束，那么就将这些后续请求添加到链表当中
			if (os_atomic_cmpxchgv(vval, next, tail, &next, release)) {
				dow.dow_thread = next->dow_thread;
				dow.dow_next = next;
				//如果当前dispatch_once执行的block没有结束，那么就将这些后续请求添加到链表当中
				if (dow.dow_thread) {
					pthread_priority_t pp = _dispatch_get_priority();
					_dispatch_thread_override_start(dow.dow_thread, pp, val);
				}
				// 线程在这里休眠，直到单例方法执行完毕后，被唤醒
				_dispatch_thread_event_wait(&dow.dow_event);
				if (dow.dow_thread) {
					_dispatch_thread_override_end(dow.dow_thread, val);
				}
				break;
			}
		}
		_dispatch_thread_event_destroy(&dow.dow_event);
	}
#endif
}

DISPATCH_NOINLINE
void
dispatch_once_f(dispatch_once_t *val, void *ctxt, dispatch_function_t func)
{
#if !DISPATCH_ONCE_INLINE_FASTPATH
	if (likely(os_atomic_load(val, acquire) == DLOCK_ONCE_DONE)) {  //0xffff
		return;
	}
#endif // !DISPATCH_ONCE_INLINE_FASTPATH
	return dispatch_once_f_slow(val, ctxt, func);
}
