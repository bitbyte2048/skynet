#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64
#define MAX_GLOBAL_MQ 0x10000

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.

#define MQ_IN_GLOBAL 1
#define MQ_OVERLOAD 1024

//skynet的消息队列的设计，有个全局的消息队列，全局消息队列的每个item都是个子消息队列
//消息队列，每个服务一个消息队列
struct message_queue {
	struct spinlock lock;
	uint32_t handle;	//handle唯一标示
	int cap;			//容量
	int head;			//头
	int tail;			//尾
	int release;
	int in_global;					//是否在全局队列中
	int overload;					//负载
	int overload_threshold;			//负载阀值
	struct skynet_message *queue;	//消息队列（环形队列）
	struct message_queue *next;		//指向下一个队列
};

//全局消息队列，每个node都是一个message_queue，全局消息队列采用链表管理，是FIFO的队列
struct global_queue {
	struct message_queue *head;
	struct message_queue *tail;
	struct spinlock lock;
};

static struct global_queue *Q = NULL;

//全局消息队列压入子消息队列 添加到链表的尾部
void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;

	SPIN_LOCK(q)
	assert(queue->next == NULL);
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else {
		q->head = q->tail = queue;
	}
	SPIN_UNLOCK(q)
}

//
struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q;

	SPIN_LOCK(q)
	struct message_queue *mq = q->head;
	if(mq) {
		q->head = mq->next;
		if(q->head == NULL) {
			assert(mq == q->tail);
			q->tail = NULL;
		}
		mq->next = NULL;
	}
	SPIN_UNLOCK(q)

	return mq;
}

struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;		//handle关联message_queue
	q->cap = DEFAULT_QUEUE_SIZE;
	q->head = 0;
	q->tail = 0;
	SPIN_INIT(q)
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_push to push it to global queue.
	q->in_global = MQ_IN_GLOBAL;
	q->release = 0;
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);
	q->next = NULL;

	return q;
}

static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);
	SPIN_DESTROY(q)
	skynet_free(q->queue);
	skynet_free(q);
}

uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

//获取消息队列中消息数量
int
skynet_mq_length(struct message_queue *q) {
	int head, tail,cap;

	SPIN_LOCK(q)
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	SPIN_UNLOCK(q)
	
	if (head <= tail) {
		return tail - head;
	}
	return tail + cap - head;
}

//判断消息是否负载，如果负载输出消息数量，然后在重置
int
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;
		return overload;
	} 
	return 0;
}

//消息队列中pop中消息
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	SPIN_LOCK(q)
	//head != tail时消息队列不为空
	if (q->head != q->tail) {
		*message = q->queue[q->head++];
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		//head大于等于cap 处理环形的情况
		if (head >= cap) {
			q->head = head = 0;
		}
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}
		//指数增加的方式调整输出警告的阀值
		while (length > q->overload_threshold) {
			q->overload = length;
			q->overload_threshold *= 2;
		}
	} else {
		//消息队列为空了，此时可以调整警告的阀值到初始值了
		// reset overload_threshold when queue is empty
		q->overload_threshold = MQ_OVERLOAD;
	}

	if (ret) {
		q->in_global = 0;	//有消息pop出，此时标记消息队列不在全局消息队列中
	}
	
	SPIN_UNLOCK(q)

	return ret;
}

//扩展队列 2倍方式扩展
static void
expand_queue(struct message_queue *q) {
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	//消息从queue[head,head+cap-1] 拷贝到new_queue[0,cap-1]
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	//重置head和tail
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;
	
	skynet_free(q->queue);
	q->queue = new_queue;
}

//消息队列push消息
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	SPIN_LOCK(q)

	q->queue[q->tail] = *message;
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}
	//当head == tail时 环形消息队列已经负载满了，所以消息队列要扩展
	if (q->head == q->tail) {
		expand_queue(q);
	}

	//这里in_global标志为0时，这里解释下为什么重新把q插入全局队列（先pop出然后在插入主要为了避免饥饿当发生）
	if (q->in_global == 0) {
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	
	SPIN_UNLOCK(q)
}

void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	SPIN_INIT(q);
	Q=q;
}

//标记需要释放的消息队列
void 
skynet_mq_mark_release(struct message_queue *q) {
	SPIN_LOCK(q)
	assert(q->release == 0);
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	SPIN_UNLOCK(q)
}

//把消息队列的消息全部pop掉然后调用drop_func处理消息，最后释放消息队列
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	while(!skynet_mq_pop(q, &msg)) {
		drop_func(&msg, ud);
	}
	_release(q);
}

void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	SPIN_LOCK(q)
	
	if (q->release) {
		SPIN_UNLOCK(q)
		_drop_queue(q, drop_func, ud);
	} else {
		skynet_globalmq_push(q);
		SPIN_UNLOCK(q)
	}
}
