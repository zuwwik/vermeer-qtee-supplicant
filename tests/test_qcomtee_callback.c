// SPDX-License-Identifier: MIT
#define qcomtee_memory_object_addr mock_memory_addr
#define qcomtee_memory_object_size mock_memory_size
#include "../src/qcomtee_callback.h"
#undef qcomtee_memory_object_addr
#undef qcomtee_memory_object_size
#include <assert.h>
static uint8_t memory[QS_FS_BUFFER_SIZE];
static struct qs_store test_store;
static unsigned calls;
void *mock_memory_addr(struct qcomtee_object *object)
{ assert(object == (void *)1); return memory; }
size_t mock_memory_size(struct qcomtee_object *object)
{ assert(object == (void *)1); return sizeof(memory); }
static int service_dispatch(struct qs_store *store, void *data, size_t size)
{
	assert(store == &test_store && data == memory && size == sizeof(memory));
	calls++; return 0;
}
int main(void)
{
	const struct qs_service service = { .dispatch = service_dispatch };
	struct native_listener listener = { .service = &service, .store = &test_store, .memory = (void *)1 };
	struct qcomtee_param params[6] = {
		{ .attr = QCOMTEE_UBUF_OUTPUT, .ubuf = { NULL, 16 } },
		{ .attr = QCOMTEE_UBUF_OUTPUT, .ubuf = { NULL, 4 } },
	};
	for (unsigned i = 2; i < 6; i++) {
		params[i].attr = QCOMTEE_OBJREF_OUTPUT; params[i].object = (void *)1;
	}
	assert(dispatch(&listener.callback, 0, params, 6) == QCOMTEE_OK && calls == 1);
	assert(params[0].ubuf.addr == listener.offsets && params[0].ubuf.size == 0);
	assert(params[1].ubuf.addr == &listener.is_64 && params[1].ubuf.size == 4 && listener.is_64 == 1);
	for (unsigned i = 2; i < 6; i++) assert(params[i].object == QCOMTEE_OBJECT_NULL);
	assert(dispatch(&listener.callback, 1, NULL, 0) == QCOMTEE_OK);
	assert(dispatch(&listener.callback, 0, params, 5) == QCOMTEE_ERROR_INVALID);
	params[1].ubuf.size = 3;
	assert(dispatch(&listener.callback, 0, params, 6) == QCOMTEE_ERROR_INVALID && calls == 1);
	puts("native QTEE callback output storage and service routing: OK");
}
