/*
 *
 * Copyright CEA/DAM/DIF (2012)
 * contributor : Dominique Martinet  dominique.martinet@cea.fr
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    trans_rdma.c
 * \brief   rdma helper
 *
 * This is (very) loosely based on a mix of diod, rping (librdmacm/examples)
 * and kernel's net/9p/trans_rdma.c
 *
 */

#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <inttypes.h>	//uint*_t
#include <errno.h>	//ENOMEM
#include <sys/socket.h> //sockaddr
#include <pthread.h>	//pthread_* (think it's included by another one)
#include <semaphore.h>  //sem_* (is it a good idea to mix sem and pthread_cond/mutex?)
#include <arpa/inet.h>  //inet_ntop
#include <netinet/in.h> //sock_addr_in

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>

#include <rdma/rdma_cma.h>

#include "log.h"
#include "mooshika.h"

#define SHM_KEY 4213
#define SHM_SIZE 100*1024*1025
#define SHM_SEM_KEY 4241
#define SERVER_SEND_SEM_KEY 4242
#define CLIENT_SEND_SEM_KEY 4243
#define RECV_SEM 0
#define SEND_SEM 1
#define SHM_SEM 2

struct msk_ctx {
	int used;
	msk_data_t *pdata;
	ctx_callback_t callback;
	void *callback_arg;
};

typedef struct msk_list msk_list_t;
typedef struct msk_shm msk_shm_t;
typedef struct msk_sem msk_sem_t;

struct msk_list {
	msk_ctx_t *ctx;
	msk_list_t *next;
};

struct msk_shm {
	int shmid;
	key_t shmkey;
	uint32_t len;
	msk_data_t *shm;
	key_t semkey;
	int semid;
};

struct msk_sem {
	key_t semkey;
	int semid;
	msk_list_t *ctx_head;
	msk_list_t *ctx_last;
};

static pthread_t send_thread;
static pthread_t recv_thread;

/* UTILITY FUNCTIONS */

/**
 * msk_reg_mr: registers memory for rdma use (almost the same as ibv_reg_mr)
 *
 * @param trans   [IN]
 * @param memaddr [IN] the address to register
 * @param size    [IN] the size of the area to register
 * @param access  [IN] the access to grants to the mr (e.g. IBV_ACCESS_LOCAL_WRITE)
 *
 * @return a pointer to the mr if registered correctly or NULL on failure
 */

struct ibv_mr *msk_reg_mr(msk_trans_t *trans, void *memaddr, size_t size, int access) {
	struct ibv_mr *mr;
	mr = malloc(sizeof(struct ibv_mr));
	if (!mr) {
		ERROR_LOG("malloc failed!");
		return NULL;
	}
	mr->addr = memaddr;
	mr->length = size;

	return mr;
}

/**
 * msk_reg_mr: deregisters memory for rdma use (exactly ibv_dereg_mr)
 *
 * @param mr [INOUT] the mr to deregister
 *
 * @return 0 on success, errno value on failure
 */
int msk_dereg_mr(struct ibv_mr *mr) {
	free(mr);
	return 0;
}

/**
 * msk_make_rloc: makes a rkey to send it for remote host use
 * 
 * @param mr   [IN] the mr in which the addr belongs
 * @param addr [IN] the addr to give
 * @param size [IN] the size to allow (hint)
 *
 * @return a pointer to the rkey on success, NULL on failure.
 */
msk_rloc_t *msk_make_rloc(struct ibv_mr *mr, uint64_t addr, uint32_t size) {
	msk_rloc_t *rloc;
	rloc = malloc(sizeof(msk_rloc_t));
	if (!rloc) {
		ERROR_LOG("Out of memory!");
		return NULL;
	}

	rloc->raddr = addr;
	rloc->rkey = mr->rkey;
	rloc->size = size;

	return rloc;
}

/**
 * msk_semop
 *
 * does the op on send sem if send, recv sem otherwise
 */
static int msk_semop(msk_trans_t *trans, short send, short op) {
	struct sembuf sops;
	sops.sem_num = 0;
	sops.sem_flg = 0;
	sops.sem_op = op;
	if (send == SEND_SEM)
		return semop(((msk_sem_t *)trans->qp->send_cq->cq_context)->semid, &sops, 1);
	else if (send == RECV_SEM)
		return semop(((msk_sem_t *)trans->qp->recv_cq->cq_context)->semid, &sops, 1);
	else if (send == SHM_SEM)
		return semop(((msk_sem_t *)trans->qp->qp_context)->semid, &sops, 1);
	else
		return EINVAL;
}

/* INIT/SHUTDOWN FUNCTIONS */

static void *msk_send_thread(void *arg) {
	msk_trans_t *trans = arg;
	if (!trans || !trans->qp || !trans->qp->send_cq || !trans->qp->send_cq->cq_context || !trans->qp->qp_context) {
		ERROR_LOG("cant start without everything init");
		pthread_exit(NULL);
	}

	msk_sem_t *sem = trans->qp->send_cq->cq_context;
	msk_shm_t *shm = trans->qp->qp_context;
	msk_ctx_t *ctx;

	while ( ! pthread_cond_wait(&trans->cond, &trans->lock)) {
		if (sem->ctx_head) {
			ctx = sem->ctx_head->ctx;
			msk_semop(trans, SHM_SEM, -1);
			shm->shm->size = ctx->pdata->size;
			memcpy(&shm->shm->data, ctx->pdata->data, ctx->pdata->size);
			msk_semop(trans, SHM_SEM, 1);
			msk_semop(trans, SEND_SEM, 1);
			if (ctx->callback)
				ctx->callback(trans, ctx->pdata, ctx->callback_arg);
			free(sem->ctx_head);
			sem->ctx_head = sem->ctx_head->next;
			if (!sem->ctx_head)
				sem->ctx_last = NULL;
			ctx->used = 0;
			pthread_cond_broadcast(&trans->cond);
		}
	}
	pthread_exit(NULL);
}

static void *msk_recv_thread(void *arg) {
	msk_trans_t *trans = arg;
	if (!trans || !trans->qp || !trans->qp->recv_cq || !trans->qp->recv_cq->cq_context || !trans->qp->qp_context) {
		ERROR_LOG("cant start without everything init");
		pthread_exit(NULL);
	}

	msk_sem_t *sem = trans->qp->recv_cq->cq_context;
	msk_shm_t *shm = trans->qp->qp_context;
	msk_ctx_t *ctx;

	while (! msk_semop(trans, RECV_SEM, -1)) {
		if (sem->ctx_head) {
			ctx = sem->ctx_head->ctx;
			msk_semop(trans, SHM_SEM, -1);
			ctx->pdata->size = shm->shm->size;
			memcpy(ctx->pdata->data, &shm->shm->data, ctx->pdata->size);
			msk_semop(trans, SHM_SEM, 1);
			if (ctx->callback)
				ctx->callback(trans, ctx->pdata, ctx->callback_arg);
			free(sem->ctx_head);
			sem->ctx_head = sem->ctx_head->next;
			if (!sem->ctx_head)
				sem->ctx_last = NULL;
			ctx->used = 0;
			pthread_mutex_lock(&trans->lock);
			pthread_cond_broadcast(&trans->cond);
			pthread_mutex_unlock(&trans->lock);
		}
	}

	trans->state = MSK_CLOSED;
	if (trans->disconnect_callback)
		trans->disconnect_callback(trans);

	pthread_exit(NULL);
}


/**
 * msk_destroy_buffer
 *
 */
static void msk_destroy_buffer(msk_trans_t *trans) {
	if (trans->send_buf)
		free(trans->send_buf);

	if (trans->recv_buf)
		free(trans->recv_buf);

	if (trans->qp->send_cq->cq_context) {
		semctl(((msk_sem_t *)trans->qp->send_cq->cq_context)->semid, 0, IPC_RMID);
		free(trans->qp->send_cq->cq_context);
	}
	if (trans->qp->send_cq)
		free(trans->qp->send_cq);
	if (trans->qp->recv_cq->cq_context) {
		semctl(((msk_sem_t *)trans->qp->recv_cq->cq_context)->semid, 0, IPC_RMID);
		free(trans->qp->recv_cq->cq_context);
	}
	if (trans->qp->qp_context) {
		shmctl(((msk_shm_t *)trans->qp->qp_context)->shmid, IPC_RMID, NULL);
		free(trans->qp->qp_context);
	}
	if (trans->qp)
		free(trans->qp);

}


/**
 * msk_destroy_trans: disconnects and free trans data
 *
 * @param trans [INOUT] the trans to destroy
 */
void msk_destroy_trans(msk_trans_t **ptrans) {

	msk_trans_t *trans = *ptrans;

	msk_destroy_buffer(trans);

	pthread_mutex_destroy(&trans->lock);
	pthread_cond_destroy(&trans->cond);

	free(trans);
}

/**
 * msk_init: part of the init that's the same for client and server
 *
 * @param ptrans [INOUT]
 * @param attr   [IN]    attributes to set parameters in ptrans. attr->addr must be set, others can be either 0 or sane values.
 *
 * @return 0 on success, errno value on failure
 */
int msk_init(msk_trans_t **ptrans, msk_trans_attr_t *attr) {
	int ret;

	msk_trans_t *trans;

	*ptrans = malloc(sizeof(msk_trans_t));
	if (!*ptrans) {
		ERROR_LOG("Out of memory");
		return ENOMEM;
	}

	trans=*ptrans;

	memset(trans, 0, sizeof(msk_trans_t));

	trans->state = MSK_INIT;

	trans->server = attr->server;
	trans->timeout = attr->timeout   ? attr->timeout  : 3000000; // in ms
	trans->sq_depth = attr->sq_depth ? attr->sq_depth : 5;
	trans->rq_depth = attr->rq_depth ? attr->rq_depth : 5;
	trans->disconnect_callback = attr->disconnect_callback;

	ret = pthread_mutex_init(&trans->lock, NULL);
	if (ret) {
		ERROR_LOG("pthread_mutex_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return ret;
	}
	ret = pthread_cond_init(&trans->cond, NULL);
	if (ret) {
		ERROR_LOG("pthread_cond_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return ret;
	}

	return 0;
}




/**
 * msk_setup_buffer
 */
static int msk_setup_buffer(msk_trans_t *trans) {
	int ret;
	msk_shm_t *shm;
	msk_sem_t *sem;

	trans->recv_buf = malloc(trans->rq_depth * sizeof(msk_ctx_t));
	if (!trans->recv_buf) {
		ERROR_LOG("couldn't malloc trans->recv_buf");
		return ENOMEM;
	}
	memset(trans->recv_buf, 0, trans->rq_depth * sizeof(msk_ctx_t));

	trans->send_buf = malloc(trans->sq_depth * sizeof(msk_ctx_t));
	if (!trans->send_buf) {
		ERROR_LOG("couldn't malloc trans->send_buf");
		msk_destroy_buffer(trans);
		return ENOMEM;
	}
	memset(trans->send_buf, 0, trans->sq_depth * sizeof(msk_ctx_t));


	trans->qp = malloc(sizeof(struct ibv_qp));
	if (!trans->qp) {
		ERROR_LOG("couldn't malloc trans->qp");
		msk_destroy_buffer(trans);
		return ENOMEM;
	}

	trans->qp->qp_context = malloc(sizeof(msk_shm_t));
	if (!trans->qp->qp_context) {
		ERROR_LOG("couldn't malloc trans->qp->qp_context");
		msk_destroy_buffer(trans);
		return ENOMEM;
	}

	shm = trans->qp->qp_context;

	shm->len = SHM_SIZE;
	shm->shmkey = SHM_KEY;
	shm->shmid = shmget(shm->shmkey, SHM_SIZE, 0666 | IPC_CREAT);
	if (shm->shmid == -1) {
		ret = errno;
		ERROR_LOG("shmget failed: %s (%d)", strerror(ret), ret);
		return ret;
	}
	shm->shm = shmat(shm->shmid, NULL, 0);
	if (shm->shm == (void *)-1) {
		ret = errno;
		ERROR_LOG("shmat failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	shm->semkey = SHM_SEM_KEY;
	shm->semid = semget(shm->semkey, 1, 0666 | IPC_CREAT);
	if (!shm->semid) {
		ret = errno;
		ERROR_LOG("semget failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	trans->qp->send_cq = malloc(sizeof(struct ibv_cq)); 
	if (!trans->qp->send_cq) {
		ERROR_LOG("couldn't malloc trans->qp->send_cq");
		msk_destroy_buffer(trans);
		return ENOMEM;
	}
	
	trans->qp->send_cq->cq_context = malloc(sizeof(msk_sem_t));
	if (!trans->qp->send_cq->cq_context) {
		ERROR_LOG("couldn't malloc trans->qp->send_cq->cq_context");
		msk_destroy_buffer(trans);
		return ENOMEM;
	}

	sem = trans->qp->send_cq->cq_context;
	sem->semkey = trans->server ? SERVER_SEND_SEM_KEY : CLIENT_SEND_SEM_KEY;
	sem->semid = semget(sem->semkey, 1, 0666 | IPC_CREAT);
	if (!sem->semid) {
		ret = errno;
		ERROR_LOG("semget failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	trans->qp->recv_cq = malloc(sizeof(struct ibv_cq)); 
	if (!trans->qp->recv_cq) {
		ERROR_LOG("couldn't malloc trans->qp->recv_cq");
		msk_destroy_buffer(trans);
		return ENOMEM;
	}

	trans->qp->recv_cq->cq_context = malloc(sizeof(msk_sem_t));
	if (!trans->qp->recv_cq->cq_context) {
		ERROR_LOG("couldn't malloc trans->qp->recv_cq->cq_context");
		msk_destroy_buffer(trans);
		return ENOMEM;
	}

	sem = trans->qp->recv_cq->cq_context;
	sem->semkey = trans->server ? CLIENT_SEND_SEM_KEY : SERVER_SEND_SEM_KEY;
	sem->semid = semget(sem->semkey, 1, 0666 | IPC_CREAT);
	if (!sem->semid) {
		ret = errno;
		ERROR_LOG("semget failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	return 0;
}

/**
 * msk_bind_server
 *
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
int msk_bind_server(msk_trans_t *trans) {

	return 0;
}


/**
 * msk_start_cm_thread: dummy function
 *
 * @param trans [IN]
 *
 * @return same as pthread_create (0 on success)
 */
int msk_start_cm_thread(msk_trans_t *trans) {
	return 0;
}


/**
 * msk_accept: does the real connection acceptance
 *
 * @param trans [IN]
 *
 * @return 0 on success, the value of errno on error
 */
int msk_finalize_accept(msk_trans_t *trans) {
	msk_semop(trans, SHM_SEM, -1);
	msk_semop(trans, SHM_SEM, 0);
	msk_semop(trans, SHM_SEM, 1);

	return 0;
}

/**
 * msk_accept_one: given a listening trans, waits till one connection is requested and accepts it
 *
 * @param rdma_connection [IN] the mother trans
 *
 * @return a new trans for the child on success, NULL on failure
 */
msk_trans_t *msk_accept_one(msk_trans_t *trans) {
	int ret;

	if (!trans) {
		ERROR_LOG("trans must be initialized first!");
		return NULL;
	}

	ret = msk_setup_buffer(trans);
	if (ret) {
		ERROR_LOG("msk setup buffer failed: %d", ret);
		return NULL;
	}

	pthread_attr_t attr_thr;

	/* Init for thread parameter (mostly for scheduling) */
	if(pthread_attr_init(&attr_thr) != 0)
		ERROR_LOG("can't init pthread's attributes");

	if(pthread_attr_setscope(&attr_thr, PTHREAD_SCOPE_SYSTEM) != 0)
		ERROR_LOG("can't set pthread's scope");

	if(pthread_attr_setdetachstate(&attr_thr, PTHREAD_CREATE_JOINABLE) != 0)
		ERROR_LOG("can't set pthread's join state");

	pthread_mutex_lock(&trans->lock); // lock will be unlocked on cond_wait when recv_thread is ready
	pthread_create(&send_thread, &attr_thr, msk_send_thread, trans);
	pthread_create(&recv_thread, &attr_thr, msk_recv_thread, trans);

	msk_semop(trans, SHM_SEM, 1);
	return trans;
}

/**
 * msk_connect_client: does the actual connection to the server
 *
 */
int msk_finalize_connect(msk_trans_t *trans) {
	msk_semop(trans, SHM_SEM, -1);
	msk_semop(trans, SHM_SEM, 0);

	return 0;
}

/**
 * msk_connect: connects a client to a server
 *
 * @param trans [INOUT] trans must be init first
 *
 * @return 0 on success, the value of errno on error 
 */
int msk_connect(msk_trans_t *trans) {
	int ret;

	if (!trans) {
		ERROR_LOG("trans must be initialized first!");
		return EINVAL;
	}

	ret = msk_setup_buffer(trans);
	if (ret) {
		ERROR_LOG("msk setup buffer failed: %d", ret);
		return ret;
	}

	pthread_attr_t attr_thr;

	/* Init for thread parameter (mostly for scheduling) */
	if(pthread_attr_init(&attr_thr) != 0)
		ERROR_LOG("can't init pthread's attributes");

	if(pthread_attr_setscope(&attr_thr, PTHREAD_SCOPE_SYSTEM) != 0)
		ERROR_LOG("can't set pthread's scope");

	if(pthread_attr_setdetachstate(&attr_thr, PTHREAD_CREATE_JOINABLE) != 0)
		ERROR_LOG("can't set pthread's join state");

	pthread_mutex_lock(&trans->lock); // lock will be unlocked on cond_wait when recv_thread is ready
	pthread_create(&send_thread, &attr_thr, msk_send_thread, trans);
	pthread_create(&recv_thread, &attr_thr, msk_recv_thread, trans);
	msk_semop(trans, SHM_SEM, 1);

	return 0;
}



/**
 * msk_post_n_recv: Post a receive buffer.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param trans        [IN]
 * @param pdata        [OUT] the data buffer to be filled with received data
 * @param mr           [IN]  the mr in which the data lives
 * @param callback     [IN]  function that'll be called when done
 * @param err_callback [IN]  function that'll be called on error
 * @param callback_arg [IN]  argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
int msk_post_n_recv(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
//FIXME num_sge
	msk_ctx_t *rctx;
	msk_sem_t *sem;
	msk_list_t *elem;
	int i;

	pthread_mutex_lock(&trans->lock);

	do {
		for (i = 0, rctx = trans->recv_buf;
		     i < trans->rq_depth;
		     i++, rctx++)
			if (!rctx->used)
				break;

		if (i == trans->rq_depth) {
			INFO_LOG("Waiting for cond");
			pthread_cond_wait(&trans->cond, &trans->lock);
		}

	} while ( i == trans->rq_depth );
	INFO_LOG("got a free context");


	rctx->used = 1;
	rctx->pdata = pdata;
	rctx->callback = callback;
	rctx->callback_arg = callback_arg;

	sem = trans->qp->recv_cq->cq_context;
	elem = malloc(sizeof(msk_list_t));
	elem->ctx = rctx;
	elem->next = NULL;

	if (sem->ctx_last) {
		sem->ctx_last->next = elem;
	} else {
		sem->ctx_head = elem;
	}
	sem->ctx_last = elem;

	pthread_mutex_unlock(&trans->lock);

	return 0;
}

/**
 * Post a send buffer.
 *
 * @param trans        [IN]
 * @param data         [IN] the data buffer to be sent
 * @param mr           [IN] the mr in which the data lives
 * @param callback     [IN] function that'll be called when done
 * @param err_callback [IN] function that'll be called on error
 * @param callback_arg [IN] argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
int msk_post_n_send(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	msk_ctx_t *wctx;
	msk_sem_t *sem;
	msk_list_t *elem;
	int i;

	pthread_mutex_lock(&trans->lock);

	do {
		for (i = 0, wctx = trans->send_buf;
		     i < trans->sq_depth;
		     i++, wctx++)
			if (!wctx->used)
				break;

		if (i == trans->sq_depth) {
			INFO_LOG("Waiting for cond");
			pthread_cond_wait(&trans->cond, &trans->lock);
		}

	} while ( i == trans->sq_depth );
	INFO_LOG("got a free context");


	wctx->used = 1;
	wctx->pdata = pdata;
	wctx->callback = callback;
	wctx->callback_arg = callback_arg;

	sem = trans->qp->send_cq->cq_context;
	elem = malloc(sizeof(msk_list_t));
	elem->ctx = wctx;
	elem->next = NULL;

	if (sem->ctx_last) {
		sem->ctx_last->next = elem;
	} else {
		sem->ctx_head = elem;
	}
	sem->ctx_last = elem;

	pthread_cond_broadcast(&trans->cond);
	pthread_mutex_unlock(&trans->lock);


	return 0;
}

/**
 * msk_wait_callback: send/recv callback that just unlocks a mutex.
 *
 */
static void msk_wait_callback(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
	pthread_mutex_t *lock = arg;
	pthread_mutex_unlock(lock);
}

/**
 * Post a receive buffer and waits for _that one and not any other_ to be filled.
 * Generally a bad idea to use that one unless only that one is used.
 *
 * @param trans [IN]
 * @param pdata [OUT] the data buffer to be filled with the received data
 * @param mr    [IN]  the memory region in which data lives
 *
 * @return 0 on success, the value of errno on error
 */
int msk_wait_n_recv(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = msk_post_n_recv(trans, pdata, num_sge, mr, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}

/**
 * Post a send buffer and waits for that one to be completely sent
 * @param trans [IN]
 * @param data  [IN] the data to send
 * @param mr    [IN] the memory region in which data lives
 *
 * @return 0 on success, the value of errno on error
 */
int msk_wait_n_send(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = msk_post_n_send(trans, pdata, num_sge, mr, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}


int msk_post_n_read(msk_trans_t *trans, msk_data_t *data, int num_sge, struct ibv_mr *mr, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return 0;
}

int msk_post_n_write(msk_trans_t *trans, msk_data_t *data, int num_sge, struct ibv_mr *mr, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return 0;
}

int msk_wait_n_read(msk_trans_t *trans, msk_data_t *data, int num_sge, struct ibv_mr *mr, msk_rloc_t *rloc) {
	return 0;
}


int msk_wait_n_write(msk_trans_t *trans, msk_data_t *data, int num_sge, struct ibv_mr *mr, msk_rloc_t *rloc) {
	return 0;
}
