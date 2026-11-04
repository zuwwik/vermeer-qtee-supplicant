// SPDX-License-Identifier: MIT
/* Native Linux QTEE callback transport, using upstream libqcomtee. */
#include "qsee_supplicant.h"
#include <qcomtee_object_types.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/tee.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "qcomtee_callback.h"
static volatile sig_atomic_t *active_stop;
static volatile sig_atomic_t drain_expired;
static void drain_alarm(int sig) { (void)sig; drain_expired = 1; }
static int tee_call(int fd, unsigned long request, ...)
{
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	int ret;
	do { ret = ioctl(fd, request, arg); }
	while (ret < 0 && errno == EINTR && request == TEE_IOC_SUPPL_RECV &&
	       !*active_stop && !drain_expired);
	return ret;
}
static int invoke(struct qcomtee_object *object, unsigned op,
		  struct qcomtee_param *params, int count)
{
	qcomtee_result_t result;
	if (qcomtee_object_invoke(object, op, params, count, &result)) return -1;
	if (result) {
		fprintf(stderr, "event=qcomtee_operation_error op=%u result=%d\n", op, result);
		errno = EPROTO;
		return -1;
	}
	return 0;
}
static void release(struct qcomtee_object *object) { (void)object; }
static struct qcomtee_object_ops callback_ops = { .dispatch = dispatch, .release = release };
static int register_listener(struct qcomtee_object *root, struct qcomtee_object *env,
			     struct native_listener *listener)
{
	uint32_t uid = 87, id = listener->service->id;
	struct qcomtee_param service[] = {
		{ .attr = QCOMTEE_UBUF_INPUT, .ubuf = { &uid, sizeof(uid) } },
		{ .attr = QCOMTEE_OBJREF_OUTPUT },
	};
	if (invoke(env, 0, service, 2)) return -1;
	listener->registration = service[1].object;
	if (qcomtee_memory_object_alloc(listener->service->buffer_size, root,
				       &listener->memory)) return -1;
	if (qcomtee_object_cb_init(&listener->callback, &callback_ops, root)) return -1;
	listener->initialized = true;
	if (qcomtee_object_refs_inc(&listener->callback)) return -1;
	struct qcomtee_param registration[] = {
		{ .attr = QCOMTEE_UBUF_INPUT, .ubuf = { &id, sizeof(id) } },
		{ .attr = QCOMTEE_OBJREF_INPUT, .object = &listener->callback },
		{ .attr = QCOMTEE_OBJREF_INPUT, .object = listener->memory },
	};
	if (invoke(listener->registration, 0, registration, 3)) {
		qcomtee_object_refs_dec(&listener->callback);
		return -1;
	}
	fprintf(stderr, "event=listener_registered transport=qcomtee id=%u size=%zu\n",
		id, listener->service->buffer_size);
	return 0;
}
static void drain_releases(struct qcomtee_object *root)
{
	struct sigaction action = { .sa_handler = drain_alarm }, previous;
	struct itimerval timer = { .it_value = { 1, 0 } }, old;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGALRM, &action, &previous)) return;
	drain_expired = 0;
	if (!setitimer(ITIMER_REAL, &timer, &old)) {
		while (!drain_expired) {
			if (qcomtee_object_process_one(root) && errno != EINTR) break;
		}
		timer.it_value.tv_sec = 0;
		setitimer(ITIMER_REAL, &timer, NULL);
		sigaction(SIGALRM, &previous, NULL);
		setitimer(ITIMER_REAL, &old, NULL);
	} else {
		sigaction(SIGALRM, &previous, NULL);
	}
}
static int serve_qcomtee(struct qs_store *store, const struct qs_service *services,
			 size_t count, volatile sig_atomic_t *stop,
			 void (*ready)(void *), void *ready_data)
{
	struct native_listener *listeners = NULL;
	struct qcomtee_object *root = NULL, *env = NULL;
	char path[32];
	int status = -1, saved;
	if (!count || count > 16) { errno = E2BIG; return -1; }
	active_stop = stop; drain_expired = 0;
	for (unsigned i = 0; i < 8; i++) {
		struct tee_ioctl_version_data version = {};
		snprintf(path, sizeof(path), "/dev/tee%u", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		int ret = ioctl(fd, TEE_IOC_VERSION, &version);
		close(fd);
		if (!ret && version.impl_id == TEE_IMPL_ID_QTEE &&
		    (version.gen_caps & TEE_GEN_CAP_OBJREF)) {
			root = qcomtee_object_root_init(path, tee_call, NULL, NULL);
			break;
		}
	}
	if (!root) { errno = ENODEV; return -1; }
	struct timespec now;
	if (clock_gettime(CLOCK_REALTIME, &now)) goto out;
	uint64_t timestamp = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
	/* registerLegacy CBOR: {1: Unix uid, 6: timestamp}; root daemon only. */
	if (getuid()) { errno = EPERM; goto out; }
	uint8_t credentials[] = { 0xa2, 1, 0, 6, 0x1b, 0,0,0,0,0,0,0,0 };
	for (unsigned i = 0; i < 8; i++) credentials[5+i] = timestamp >> (56 - 8*i);
	struct qcomtee_param registration[] = {
		{ .attr = QCOMTEE_UBUF_INPUT, .ubuf = { credentials, sizeof(credentials) } },
		{ .attr = QCOMTEE_OBJREF_OUTPUT },
	};
	if (invoke(root, 1, registration, 2)) goto out;
	env = registration[1].object;
	listeners = calloc(count, sizeof(*listeners));
	if (!listeners) goto out;
	for (size_t i = 0; i < count; i++) {
		listeners[i].service = &services[i];
		listeners[i].store = store;
		if (register_listener(root, env, &listeners[i])) goto out;
	}
	if (ready) ready(ready_data);
	status = 0;
	while (!*stop) {
		if (qcomtee_object_process_one(root)) {
			if (errno == EINTR && *stop) break;
			status = -1;
			break;
		}
	}
out:
	saved = errno;
	if (listeners) {
		for (size_t i = 0; i < count; i++)
			if (listeners[i].registration) qcomtee_object_refs_dec(listeners[i].registration);
		drain_releases(root);
		for (size_t i = 0; i < count; i++) {
			if (services[i].reset) services[i].reset();
			if (listeners[i].memory) qcomtee_memory_object_release(listeners[i].memory);
			if (listeners[i].initialized) qcomtee_object_refs_dec(&listeners[i].callback);
		}
		free(listeners);
	}
	if (env) qcomtee_object_refs_dec(env);
	qcomtee_object_refs_dec(root);
	active_stop = NULL;
	errno = saved;
	return status;
}
const struct qs_transport qs_qcomtee_transport = { .name = "qcomtee", .serve = serve_qcomtee };
