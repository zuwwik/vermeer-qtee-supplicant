CC ?= cc
CPPFLAGS += -D_GNU_SOURCE -Iinclude
ifneq ($(KERNEL_UAPI),)
CPPFLAGS += -I$(KERNEL_UAPI)
endif
ifneq ($(QCOMTEE_PREFIX),)
CPPFLAGS += -DHAVE_QCOMTEE -I$(QCOMTEE_PREFIX)/include
QCOMTEE_SRC = src/transport_qcomtee.c
QCOMTEE_HEADERS = src/qcomtee_callback.h
QCOMTEE_LIBS = $(QCOMTEE_PREFIX)/lib/libqcomtee.a $(QCOMTEE_PREFIX)/lib/libqcbor.a
endif
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror

COMMON = src/path.c src/services.c src/handle_db.c src/fs.c src/gpfs.c
NOTIFY = src/notify.c
APP_ACQUIRE = src/app_acquire.c
THREAD_LIBS = -pthread

PREFIX ?= /usr
SBINDIR ?= $(PREFIX)/sbin
UNITDIR ?= $(PREFIX)/lib/systemd/system
SYSCONFDIR ?= /etc
DOCDIR ?= $(PREFIX)/share/doc/qsee-supplicant
DESTDIR ?=

.PHONY: all check check-qcomtee clean install install-bin install-doc install-openrc install-systemd
all: qsee-supplicant qsee-app-loader

qsee-supplicant: src/main.c src/transport_qseecom.c $(QCOMTEE_SRC) $(COMMON) $(NOTIFY) $(QCOMTEE_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(THREAD_LIBS) $(QCOMTEE_LIBS) $(LDLIBS)

qsee-app-loader: src/app_loader.c $(APP_ACQUIRE) $(NOTIFY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-protocol: tests/test_protocol.c $(COMMON)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(THREAD_LIBS) $(LDLIBS)

test-notify: tests/test_notify.c $(NOTIFY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-app-acquire: tests/test_app_acquire.c $(APP_ACQUIRE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

check: test-protocol test-notify test-app-acquire
	./test-protocol
	./test-notify
	./test-app-acquire

test-qcomtee-callback: tests/test_qcomtee_callback.c src/qcomtee_callback.h
	@test -n "$(QCOMTEE_PREFIX)" || { echo 'Set QCOMTEE_PREFIX to upstream libqcomtee headers'; exit 1; }
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -ffunction-sections -fdata-sections \
		-o $@ $< -Wl,--gc-sections

check-qcomtee: test-qcomtee-callback
	./test-qcomtee-callback

install: install-bin install-systemd install-openrc install-doc

install-bin: qsee-supplicant qsee-app-loader
	install -d $(DESTDIR)$(SBINDIR)
	install -m 0755 qsee-supplicant $(DESTDIR)$(SBINDIR)/qsee-supplicant
	install -m 0755 qsee-app-loader $(DESTDIR)$(SBINDIR)/qsee-app-loader

install-systemd:
	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 packaging/qsee-supplicant.service $(DESTDIR)$(UNITDIR)/
	install -m 0644 packaging/qsee-app-loader@.service $(DESTDIR)$(UNITDIR)/

install-openrc:
	install -d $(DESTDIR)$(SYSCONFDIR)/init.d
	install -m 0755 packaging/qsee-supplicant.openrc $(DESTDIR)$(SYSCONFDIR)/init.d/qsee-supplicant
	install -m 0755 packaging/qsee-app-loader.openrc $(DESTDIR)$(SYSCONFDIR)/init.d/qsee-app-loader

install-doc:
	install -d $(DESTDIR)$(DOCDIR)
	install -m 0644 README.md $(DESTDIR)$(DOCDIR)/README.md

clean:
	rm -f qsee-supplicant qsee-app-loader test-protocol test-notify test-app-acquire test-qcomtee-callback
