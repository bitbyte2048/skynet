#include "skynet.h"

#include "skynet_timer.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#if defined(__APPLE__)
#include <AvailabilityMacros.h>
#include <sys/time.h>
#include <mach/task.h>
#include <mach/mach.h>
#endif

typedef void (*timer_execute_func)(void *ud,void *arg);

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)
#define TIME_NEAR_MASK (TIME_NEAR-1)	
#define TIME_LEVEL_MASK (TIME_LEVEL-1)

struct timer_event {
	uint32_t handle;
	int session;
};

struct timer_node {
	struct timer_node *next;
	uint32_t expire;
};

struct link_list {
	struct timer_node head;
	struct timer_node *tail;
};
//skynet定时器采用时间轮算法，插入,获取过期定时器时间复杂度都是O(1)，并没提供删除操作。
//先想象自然时钟，秒指针经过60进位，分指针60进位，时指针24进位
//skynet定时器 near指针256进位，t0,t1,t2,t3指针分别64进位。时间用32位unsigned int表示 near 8位，t0,t1,t2,t3分别是6位
//所以8+6+6+6+6 = 32位 每个时刻可以用<t3,t2,t1,t0,n>的四元组来表示 插入的环定义 near环直接是<n,*>,
//其他4个级别的环为<l0,*> <l1,*> <l2,*> <l3,*>,其中*为通配符，当级别为n时，*范围为0到255，其他级别范围为0到63
struct timer {
	struct link_list near[TIME_NEAR];	//最近256个tick的node
	struct link_list t[4][TIME_LEVEL];	//其他的级别为4的 每级64个node
	struct spinlock lock;				//自旋锁
	uint32_t time;						//当前tick
	uint32_t starttime;
	uint64_t current;				
	uint64_t current_point;
};

static struct timer * TI = NULL;


//清空链表 并返回链表的头
static inline struct timer_node *
link_clear(struct link_list *list) {
	struct timer_node * ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

//添加到链表尾部
static inline void
link(struct link_list *list,struct timer_node *node) {
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}

//添加定时器
static void
add_node(struct timer *T,struct timer_node *node) {
	uint32_t time=node->expire;
	uint32_t current_time=T->time;
	//令current_time为<t3,t2,t1,t0,n>
	//当time为<t3,t2,t1,t0,x> 其中x>=n,因为定时器超时时间至少大于0
	//插入near环 其中插入槽位idx等于x
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {
		link(&T->near[time&TIME_NEAR_MASK],node);
	} else {
		int i;
		//分别判断插入哪个级别，
		//当time为<*,*,*,t0,n>时，插入环<0,t0>
		//当time为<*,*,t1,*,n>时，插入环<1,t1>
		uint32_t mask=TIME_NEAR << TIME_LEVEL_SHIFT;
		for (i=0;i<3;i++) {
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;
		}
		//然后挂node到对应的i级别轮子上，槽位idx为time和current_time相同的t0,t1,t2,t3级别上
		link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);	
	}
}

static void
timer_add(struct timer *T,void *arg,size_t sz,int time) {
	//分配node大小+sz大小 主要在timer_node中存储event，这里event多了一次拷贝，感觉这里可以实现发送者负责分配event，接收者负责释放event
	//可以少一次拷贝操作，不过多拷贝一次是个比较安全的做法
	struct timer_node *node = (struct timer_node *)skynet_malloc(sizeof(*node)+sz);
	memcpy(node+1,arg,sz);

	//spinlock 线程安全访问T
	SPIN_LOCK(T);
	node->expire=time+T->time;	//过期时间
	add_node(T,node);			//添加node到定时器

	SPIN_UNLOCK(T);
}

//移动链表 清空level级别上idx槽位上的链表，然后重新add，此时应该都可以在near环上
static void
move_list(struct timer *T, int level, int idx) {
	struct timer_node *current = link_clear(&T->t[level][idx]);
	while (current) {
		struct timer_node *temp=current->next;
		add_node(T,current);	
		current=temp;
	}
}

static void
timer_shift(struct timer *T) {
	int mask = TIME_NEAR;
	uint32_t ct = ++T->time; //<t3,t2,t1,t0,n>中n增加1
	//溢出时,把环<l3,0>重新插入
	if (ct == 0) {
		move_list(T, 3, 0);
	} else {
		uint32_t time = ct >> TIME_NEAR_SHIFT; //右移8位 time为<t3,t2,t1,t0>
		int i=0;//i标记了轮子级别
		//发生进位了，查看进位在哪个级别
		while ((ct & (mask-1))==0) {
			int idx=time & TIME_LEVEL_MASK;	
			if (idx!=0) {
				//比如当前时间为<0,0,0,t0,0>，重新插入插入环上<l0,t0>
				//当时间为<0,0,t1,0,0>，重新插入插入环上<l1,t1>
				move_list(T, i, idx);				
				break;				
			}
			mask <<= TIME_LEVEL_SHIFT;
			time >>= TIME_LEVEL_SHIFT;
			++i;
		}
	}
}

//派发定时器node列表
static inline void
dispatch_list(struct timer_node *current) {
	do {
		struct timer_event * event = (struct timer_event *)(current+1);
		struct skynet_message message;
		message.source = 0;
		message.session = event->session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		skynet_context_push(event->handle, &message);
		
		struct timer_node * temp = current;
		current=current->next;
		skynet_free(temp);	
	} while (current);
}

static inline void
timer_execute(struct timer *T) {
	//根据当前时间求去idx
	int idx = T->time & TIME_NEAR_MASK;
	
	while (T->near[idx].head.next) {
		//清空链表 返回current 这里为什么是while 而不是if，如果在dispatch_list期间，有timer_node add进来，比如超时时间为0tick的定时器
		struct timer_node *current = link_clear(&T->near[idx]);
		//可能链表比较长，这里锁的粒度就比较大了，所以先unlock
		SPIN_UNLOCK(T);
		// dispatch_list don't need lock T
		dispatch_list(current);
		SPIN_LOCK(T);
	}
}

//update timer
static void 
timer_update(struct timer *T) {
	//加锁
	SPIN_LOCK(T);

	// try to dispatch timeout 0 (rare condition)
	//这里派发timeout为0的定时器node
	timer_execute(T);

	// shift time first, and then dispatch timer message
	//推进tick
	timer_shift(T);

	timer_execute(T);

	SPIN_UNLOCK(T);
}

//创建定时器 并初始化near以及其他4个级别的环
static struct timer *
timer_create_timer() {
	struct timer *r=(struct timer *)skynet_malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r));

	int i,j;

	for (i=0;i<TIME_NEAR;i++) {
		link_clear(&r->near[i]);
	}

	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL;j++) {
			link_clear(&r->t[i][j]);
		}
	}

	SPIN_INIT(r)

	r->current = 0;

	return r;
}

//skynet timeout
int
skynet_timeout(uint32_t handle, int time, int session) {
	//当time小于0时，直接把消息发送到对方的消息队列，无需经过定时器
	//所以当time小于0时，调度顺序只是按照书写顺序来执行
	if (time <= 0) {
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		if (skynet_context_push(handle, &message)) {
			return -1;
		}
	} else {
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		timer_add(TI, &event, sizeof(event), time);
	}

	return session;
}

// centisecond: 1/100 second
static void
systime(uint32_t *sec, uint32_t *cs) {
#if !defined(__APPLE__) || defined(AVAILABLE_MAC_OS_X_VERSION_10_12_AND_LATER)
	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);
	*sec = (uint32_t)ti.tv_sec;
	*cs = (uint32_t)(ti.tv_nsec / 10000000);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	*sec = tv.tv_sec;
	*cs = tv.tv_usec / 10000;
#endif
}

//gettime 返回值精度为100ms
static uint64_t
gettime() {
	uint64_t t;
#if !defined(__APPLE__) || defined(AVAILABLE_MAC_OS_X_VERSION_10_12_AND_LATER)
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);
	t = (uint64_t)ti.tv_sec * 100;	
	t += ti.tv_nsec / 10000000;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	t = (uint64_t)tv.tv_sec * 100;
	t += tv.tv_usec / 10000;
#endif
	return t;
}

void
skynet_updatetime(void) {
	uint64_t cp = gettime();
	if(cp < TI->current_point) {
		skynet_error(NULL, "time diff error: change from %lld to %lld", cp, TI->current_point);
		TI->current_point = cp;
	} else if (cp != TI->current_point) {
		uint32_t diff = (uint32_t)(cp - TI->current_point);
		TI->current_point = cp;	
		TI->current += diff;
		int i;
		//注意这里 如果线程卡住了，可能会一帧中调度多次update
		for (i=0;i<diff;i++) {
			timer_update(TI);
		}
	}
}

uint32_t
skynet_starttime(void) {
	return TI->starttime;
}

uint64_t 
skynet_now(void) {
	return TI->current;
}

void 
skynet_timer_init(void) {
	TI = timer_create_timer();
	uint32_t current = 0;
	systime(&TI->starttime, &current);
	TI->current = current;
	TI->current_point = gettime();
}

// for profile

#define NANOSEC 1000000000
#define MICROSEC 1000000

uint64_t
skynet_thread_time(void) {
#if  !defined(__APPLE__) || defined(AVAILABLE_MAC_OS_X_VERSION_10_12_AND_LATER)
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	return (uint64_t)ti.tv_sec * MICROSEC + (uint64_t)ti.tv_nsec / (NANOSEC / MICROSEC);
#else
	struct task_thread_times_info aTaskInfo;
	mach_msg_type_number_t aTaskInfoCount = TASK_THREAD_TIMES_INFO_COUNT;
	if (KERN_SUCCESS != task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t )&aTaskInfo, &aTaskInfoCount)) {
		return 0;
	}

	return (uint64_t)(aTaskInfo.user_time.seconds) + (uint64_t)aTaskInfo.user_time.microseconds;
#endif
}
