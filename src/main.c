// SPDX-License-Identifier: BSD-2-Clause
#include "qsee_supplicant.h"
#include "notify.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop_handler(int signal_number) { (void)signal_number; stopping = 1; }
static void ready_handler(void *data)
{
	unsigned int *delay = data;

	*delay = 1;
	if (qs_notify("READY=1\nSTATUS=Listener services registered") < 0)
		fprintf(stderr, "event=notify_error errno=%d message=\"%s\"\n",
			errno, strerror(errno));
}

int main(int argc, char **argv)
{
	const char *state = "/var/lib/qsee-supplicant"; struct qs_store store = { .root_fd = -1 };
	const struct qs_transport *transport = &qs_qseecom_transport;
	unsigned int delay = 1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--state-dir") && i + 1 < argc) state = argv[++i];
		else if (!strcmp(argv[i], "--transport") && i + 1 < argc) {
			const char *name = argv[++i];
			if (!strcmp(name, "qseecom")) transport = &qs_qseecom_transport;
#ifdef HAVE_QCOMTEE
			else if (!strcmp(name, "qcomtee")) transport = &qs_qcomtee_transport;
#endif
			else { fprintf(stderr, "Unsupported transport: %s\n", name); return 2; }
		} else {
			fprintf(stderr, "usage: %s [--state-dir PATH] [--transport NAME]\n", argv[0]);
			return 2;
		}
	}
	umask(0077);
	if (qs_store_open(&store, state, 0600, 0700)) { perror("state directory"); return 1; }
	{
		struct sigaction action = { .sa_handler = stop_handler };
		sigemptyset(&action.sa_mask);
		/* No SA_RESTART: a signal must interrupt the blocking receive ioctl. */
		sigaction(SIGINT, &action, NULL);
		sigaction(SIGTERM, &action, NULL);
	}
	while (!stopping) {
		fprintf(stderr, "event=transport_start transport=%s state=%s\n", transport->name, state);
		if (!transport->serve(&store, qs_builtin_services,
					 qs_builtin_service_count, &stopping,
					 ready_handler, &delay)) break;
		fprintf(stderr, "event=transport_error transport=%s errno=%d message=\"%s\" retry=%u\n",
			transport->name, errno, strerror(errno), delay);
		for (unsigned int i = 0; i < delay && !stopping; i++) sleep(1);
		if (delay < 30) delay *= 2;
	}
	qs_store_close(&store);
	fprintf(stderr, "event=shutdown\n"); return 0;
}
