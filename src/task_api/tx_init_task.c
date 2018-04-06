#include "task_api/tx_init_task.h"

#include "tx_init_shmem.h"
#include "tx_internal.h"
#include "tx_log.h"
#include "tx_params.h"
#include "tx_prototypes.h"
#include "tx_utils.h"

#include <assert.h>

// Try using the task APIs to create a new thread in the task for use by threadexec. The thread is
// returned empty and suspended.
static thread_t
create_thread(task_t task) {
	thread_t thread;
	kern_return_t kr = thread_create(task, &thread);
	if (kr != KERN_SUCCESS) {
		// Only log an error if we don't have the thread API as a fallback.
#if TX_HAVE_THREAD_API
		DEBUG_TRACE(1, "Failed to create thread using task API");
#else
		ERROR_CALL(thread_create, "%u", kr);
		ERROR("Could not create thread for task 0x%x", task);
#endif
		return MACH_PORT_NULL;
	}
	return thread;
}

// Set up the shared memory region. This is easy to do with the task API and doesn't require the
// Mach ports.
static bool
initialize_shared_memory(threadexec_t threadexec) {
	bool success = false;
	// First allocate the memory.
	const size_t shmem_size = TX_SHARED_MEMORY_SIZE;
	void *shmem;
	const void *shmem_remote;
	bool ok = threadexec_shared_vm_allocate(threadexec, &shmem_remote, &shmem, shmem_size);
	if (!ok) {
		ERROR("Could not create shared memory region");
		goto fail_0;
	}
	threadexec->shmem        = shmem;
	threadexec->shmem_remote = (word_t) shmem_remote;
	threadexec->shmem_size   = shmem_size;
	// Test the shared memory if we're debugging.
#if DEBUG_LEVEL(1)
	*(word_t *)((uint8_t *)shmem + 0x3210) = 0xaabbccdd;
	word_t remote_word;
	mach_vm_size_t read_size = sizeof(remote_word);
	kern_return_t kr = mach_vm_read_overwrite(threadexec->task,
			threadexec->shmem_remote + 0x3210, read_size,
			(mach_vm_address_t) &remote_word, &read_size);
	assert(kr == KERN_SUCCESS);
	assert(remote_word == 0xaabbccdd);
#endif
	tx_init_shmem_setup_regions(threadexec);
	// Success!
	success = true;
fail_0:
	return success;
}

// Initialize the remote Mach receive port and the local send right to it. We do this first because
// it allows us to immediately detect whether the task API is supported.
static bool
initialize_remote_port(threadexec_t threadexec) {
	// Allocate the remote port. If that fails, only log an error if we don't have the thread
	// API, since if we do have the thread API we will fall back to that and use those error
	// messages.
	kern_return_t kr = mach_port_allocate(threadexec->task, MACH_PORT_RIGHT_RECEIVE,
			&threadexec->remote_port_remote);
	if (kr != KERN_SUCCESS) {
#if TX_HAVE_THREAD_API
		DEBUG_TRACE(1, "Failed to allocate Mach port using task API");
#else
		ERROR_CALL(mach_port_allocate, "%u", kr);
#endif
		return false;
	}
	// If we make it here, the task API works :)
#if TX_HAVE_THREAD_API
	DEBUG_TRACE(1, "Using task API");
	threadexec->task_api = true;
#endif
	// Get a send right to the remote port.
	bool ok = threadexec_mach_port_extract(threadexec, threadexec->remote_port_remote,
			&threadexec->remote_port, MACH_MSG_TYPE_MAKE_SEND);
	if (!ok) {
		ERROR("Could not extract remote receive right");
		return false;
	}
	return true;
}

// Initialize the local Mach receive port and the remote send right to it.
static bool
initialize_local_port(threadexec_t threadexec) {
	mach_port_t local_port = mach_port_allocate_receive_and_send();
	if (local_port == MACH_PORT_NULL) {
		ERROR("Could not allocate Mach port");
		return false;
	}
	threadexec->local_port = local_port;
	bool ok = threadexec_mach_port_insert(threadexec, local_port,
			&threadexec->local_port_remote, MACH_MSG_TYPE_MOVE_SEND);
	if (!ok) {
		ERROR("Could not move send right to remote task");
		return false;
	}
	return true;
}

bool
tx_init_with_task_api(threadexec_t threadexec) {
	// Make sure we have a thread port. TODO: We should probably initialize pthread state too!
	if (threadexec->thread == MACH_PORT_NULL) {
		assert((threadexec->flags & (TX_PRESERVE | TX_RESUME | TX_BORROW_THREAD_PORT)) == 0);
		threadexec->thread = create_thread(threadexec->task);
		if (threadexec->thread == MACH_PORT_NULL) {
			goto fail_0;
		}
		threadexec->flags |= TX_KILL_THREAD;
	}
	// First try to set up the remote port. This will tell us whether the task_api is
	// supported.
	bool ok = initialize_remote_port(threadexec);
	if (!ok) {
		goto fail_1;
	}
	// Set up the shared memory region.
	ok = initialize_shared_memory(threadexec);
	if (!ok) {
		goto fail_1;
	}
	// Now set up the local Mach port. We need to do this after we initialize the shared memory
	// region because it relies on function calling.
	ok = initialize_local_port(threadexec);
	if (!ok) {
		goto fail_1;
	}
	// Return success.
	return true;
fail_1:
	tx_deinit_with_task_api(threadexec);
fail_0:
	return false;
}

bool
tx_deinit_with_task_api(threadexec_t threadexec) {
	// TODO: Right now we don't even bother trying to deinitialize threadexec instances created
	// using the thread API.
	if (!tx_supports_task_api(threadexec)) {
		return false;
	}
	// Tear down the shared memory.
	if (threadexec->shmem_size) {
		if (threadexec->shmem_remote != 0) {
			mach_vm_deallocate(threadexec->task, threadexec->shmem_remote,
					threadexec->shmem_size);
			threadexec->shmem_remote = 0;
		}
		if (threadexec->shmem != NULL) {
			mach_vm_deallocate(mach_task_self(), (mach_vm_address_t) threadexec->shmem,
					threadexec->shmem_size);
			threadexec->shmem = NULL;
		}
		threadexec->shmem_size = 0;
	}
	// Tear down the Mach ports.
	if (threadexec->local_port != MACH_PORT_NULL) {
		mach_port_destroy(mach_task_self(), threadexec->local_port);
		threadexec->local_port = MACH_PORT_NULL;
	}
	if (threadexec->remote_port_remote != MACH_PORT_NULL) {
		mach_port_destroy(threadexec->task, threadexec->remote_port_remote);
		threadexec->remote_port_remote = MACH_PORT_NULL;
	}
	if (threadexec->local_port_remote != MACH_PORT_NULL) {
		mach_port_deallocate(threadexec->task, threadexec->local_port_remote);
		threadexec->local_port_remote = MACH_PORT_NULL;
	}
	if (threadexec->remote_port != MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), threadexec->remote_port);
		threadexec->remote_port = MACH_PORT_NULL;
	}
	return true;
}