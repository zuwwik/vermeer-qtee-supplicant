// SPDX-License-Identifier: MIT
#pragma once
#include "qsee_supplicant.h"
#include <qcomtee_object_types.h>

struct native_listener {
	struct qcomtee_object callback;
	struct qcomtee_object *registration, *memory;
	const struct qs_service *service;
	struct qs_store *store;
	uint32_t offsets[4], is_64;
	bool initialized;
};
static qcomtee_result_t dispatch(struct qcomtee_object *object, qcomtee_op_t op,
				 struct qcomtee_param *params, int count)
{
	struct native_listener *listener = container_of(object, struct native_listener, callback);
	if (op == 1) return count == 0 ? QCOMTEE_OK : QCOMTEE_ERROR_INVALID;
	if (op || count != 6 || params[0].attr != QCOMTEE_UBUF_OUTPUT ||
	    params[1].attr != QCOMTEE_UBUF_OUTPUT || params[1].ubuf.size < 4)
		return QCOMTEE_ERROR_INVALID;
	for (int i = 2; i < 6; i++) {
		if (params[i].attr != QCOMTEE_OBJREF_OUTPUT) return QCOMTEE_ERROR_INVALID;
		params[i].object = QCOMTEE_OBJECT_NULL;
	}
	/* Output-only UBUFs have no incoming storage. No embedded pointers. */
	params[0].ubuf.addr = listener->offsets;
	params[0].ubuf.size = 0;
	listener->is_64 = 1;
	params[1].ubuf.addr = &listener->is_64;
	params[1].ubuf.size = sizeof(listener->is_64);
	if (listener->service->dispatch(listener->store,
		qcomtee_memory_object_addr(listener->memory),
		qcomtee_memory_object_size(listener->memory)))
		return QCOMTEE_ERROR_INVALID;
	return QCOMTEE_OK;
}
