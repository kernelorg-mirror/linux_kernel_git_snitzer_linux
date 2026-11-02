/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A private RPC service for nfsd's KUnit suites to take requests from, so
 * that decoding and dispatching run on the objects they run on in a server.
 */

#ifndef _NFSD_KUNIT_SVC_H
#define _NFSD_KUNIT_SVC_H

#include <kunit/test.h>

#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/in.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/sunrpc/svcsock.h>
#include <net/net_namespace.h>

struct nfsd_kunit_env {
	struct svc_serv		*serv;
	struct svc_xprt		*listener;
	struct svc_xprt		*xprt;
	struct socket		*client;
};

/*
 * A thread for a test to take a request from.  It never receives, so a
 * request stays the test's; it notices that it is to stop by asking.
 */
static int nfsd_kunit_thread(void *data)
{
	struct svc_rqst *rqstp = data;

	svc_thread_init_status(rqstp, 0);
	while (!svc_thread_should_stop(rqstp))
		msleep(1);
	svc_exit_thread(rqstp);
	return 0;
}

static void nfsd_kunit_env_free(void *data)
{
	struct nfsd_kunit_env *env = data;
	struct svc_pool *pool = &env->serv->sv_pools[0];
	struct svc_rqst *rqstp;

	/* svc_exit_thread() reads it: a test may have moved a request. */
	list_for_each_entry(rqstp, &pool->sp_all_threads, rq_all)
		rqstp->rq_server = env->serv;
	svc_set_num_threads(env->serv, 0, 0);
	if (env->xprt) {
		clear_bit(XPT_BUSY, &env->xprt->xpt_flags);
		svc_xprt_close(env->xprt);
		sock_release(env->client);
	}
	clear_bit(XPT_BUSY, &env->listener->xpt_flags);
	svc_xprt_destroy_all(env->serv, &init_net, false);
	svc_destroy(&env->serv);
}

/**
 * nfsd_kunit_env_init - give a test a service and a connected transport
 * @test: the test
 * @program: the service's program; each version hidden, so that it never
 *	asks rpcbind, with a vs_xdrsize that holds the suite's arguments
 *
 * Call it before anything else the test allocates: KUnit frees last in,
 * first out, and this has to go last.
 */
static void nfsd_kunit_env_init(struct kunit *test, struct svc_program *program)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct nfsd_kunit_env *env;
	struct svc_xprt *queued;
	struct module *owner;
	int port, ret;

	env = kunit_kzalloc(test, sizeof(*env), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, env);
	env->serv = svc_create(program, PAGE_SIZE * 4, nfsd_kunit_thread);
	KUNIT_ASSERT_NOT_NULL(test, env->serv);
	port = svc_xprt_create_from_sa(env->serv, "tcp", &init_net,
				       (struct sockaddr *)&addr,
				       SVC_SOCK_ANONYMOUS, current_cred());
	if (port <= 0)
		svc_destroy(&env->serv);
	KUNIT_ASSERT_GT(test, port, 0);
	addr.sin_port = htons(port);
	env->listener = list_first_entry(&env->serv->sv_permsocks,
					 struct svc_xprt, xpt_list);
	/*
	 * A new listener is queued for its first connection, with no thread
	 * to take it.  It stays busy, off the queue; the queue held no
	 * reference on it.
	 */
	queued = lwq_dequeue(&env->serv->sv_pools[0].sp_xprts, struct svc_xprt,
			     xpt_ready);
	ret = kunit_add_action_or_reset(test, nfsd_kunit_env_free, env);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_PTR_EQ(test, queued, env->listener);
	test->priv = env;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &env->client);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kernel_connect(env->client, (struct sockaddr_unsized *)&addr,
			     sizeof(addr), 0);
	if (ret)
		sock_release(env->client);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* As svc_handle_xprt() does: freeing the transport drops the module. */
	owner = env->listener->xpt_class->xcl_owner;
	__module_get(owner);
	env->xprt = env->listener->xpt_ops->xpo_accept(env->listener);
	if (!env->xprt) {
		module_put(owner);
		sock_release(env->client);
	}
	KUNIT_ASSERT_NOT_NULL(test, env->xprt);
	env->xprt->xpt_cred = get_cred(env->listener->xpt_cred);
	clear_bit(XPT_DATA, &env->xprt->xpt_flags);
}

/*
 * What svc_authenticate() leaves in rq_authop, which COMPOUND decoding reads
 * through nfsd_read_splice_ok(). sunrpc keeps its own flavors to itself, and
 * no request here is authenticated, so only the flavor is meaningful.
 */
static struct auth_ops nfsd_kunit_auth = {
	.name		= "nfsd_kunit",
	.flavour	= RPC_AUTH_NULL,
};

/*
 * A new thread's request on the transport, its arguments and results zeroed
 * as svc_process_common() does.  A thread owns rq_argp and rq_resp and frees
 * them when it exits, so a test uses them and never replaces them.
 */
static struct svc_rqst *nfsd_kunit_rqst(struct kunit *test)
{
	struct nfsd_kunit_env *env = test->priv;
	struct svc_pool *pool = &env->serv->sv_pools[0];
	struct svc_rqst *rqstp;

	KUNIT_ASSERT_EQ(test, svc_new_thread(env->serv, pool), 0);
	rqstp = list_first_entry(&pool->sp_all_threads, struct svc_rqst, rq_all);
	rqstp->rq_xprt = env->xprt;
	rqstp->rq_authop = &nfsd_kunit_auth;
	memset(rqstp->rq_argp, 0, env->serv->sv_xdrsize);
	memset(rqstp->rq_resp, 0, env->serv->sv_xdrsize);
	return rqstp;
}

#endif /* _NFSD_KUNIT_SVC_H */
