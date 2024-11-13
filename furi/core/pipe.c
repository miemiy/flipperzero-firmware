#include "pipe.h"
#include "stream_buffer.h"
#include "semaphore.h"
#include "mutex.h"
#include "check.h"
#include "memmgr.h"
#include "event_loop_link_i.h"

/**
 * Data shared between both sides.
 */
typedef struct {
    FuriSemaphore* instance_count; // <! 1 = both sides, 0 = only one side
    FuriMutex* state_transition;
    FuriEventLoopLink alice_event_loop_link;
    FuriEventLoopLink bob_event_loop_link;
} FuriPipeShared;

/**
 * There are two PipeSides per pipe.
 */
struct FuriPipeSide {
    FuriPipeRole role;
    FuriPipeShared* shared;
    FuriStreamBuffer* sending;
    FuriStreamBuffer* receiving;
    FuriEventLoopLink* self_event_loop_link;
    FuriEventLoopLink* peer_event_loop_link;
};

FuriPipe furi_pipe_alloc(size_t capacity, size_t trigger_level) {
    FuriPipeSideReceiveSettings settings = {
        .capacity = capacity,
        .trigger_level = trigger_level,
    };
    return furi_pipe_alloc_ex(settings, settings);
}

FuriPipe furi_pipe_alloc_ex(FuriPipeSideReceiveSettings alice, FuriPipeSideReceiveSettings bob) {
    // the underlying primitives are shared
    FuriStreamBuffer* alice_to_bob = furi_stream_buffer_alloc(bob.capacity, bob.trigger_level);
    FuriStreamBuffer* bob_to_alice = furi_stream_buffer_alloc(alice.capacity, alice.trigger_level);

    FuriPipeShared* shared = malloc(sizeof(FuriPipeShared));
    *shared = (FuriPipeShared){
        .instance_count = furi_semaphore_alloc(1, 1),
        .state_transition = furi_mutex_alloc(FuriMutexTypeNormal),
    };

    FuriPipeSide* alices_side = malloc(sizeof(FuriPipeSide));
    FuriPipeSide* bobs_side = malloc(sizeof(FuriPipeSide));

    *alices_side = (FuriPipeSide){
        .role = FuriPipeRoleAlice,
        .shared = shared,
        .sending = alice_to_bob,
        .receiving = bob_to_alice,
        .self_event_loop_link = &shared->alice_event_loop_link,
        .peer_event_loop_link = &shared->bob_event_loop_link,
    };
    *bobs_side = (FuriPipeSide){
        .role = FuriPipeRoleBob,
        .shared = shared,
        .sending = bob_to_alice,
        .receiving = alice_to_bob,
        .self_event_loop_link = &shared->bob_event_loop_link,
        .peer_event_loop_link = &shared->alice_event_loop_link,
    };

    return (FuriPipe){.alices_side = alices_side, .bobs_side = bobs_side};
}

FuriPipeRole furi_pipe_role(FuriPipeSide* pipe) {
    furi_check(pipe);
    return pipe->role;
}

FuriPipeState furi_pipe_state(FuriPipeSide* pipe) {
    furi_check(pipe);
    uint32_t count = furi_semaphore_get_count(pipe->shared->instance_count);
    return (count == 1) ? FuriPipeStateOpen : FuriPipeStateBroken;
}

void furi_pipe_free(FuriPipeSide* pipe) {
    furi_check(pipe);

    // Event Loop must be disconnected
    furi_check(!pipe->self_event_loop_link->item_in);
    furi_check(!pipe->self_event_loop_link->item_out);

    furi_mutex_acquire(pipe->shared->state_transition, FuriWaitForever);
    FuriStatus status = furi_semaphore_acquire(pipe->shared->instance_count, 0);

    if(status == FuriStatusOk) {
        // the other side is still intact
        furi_mutex_release(pipe->shared->state_transition);
        free(pipe);
    } else {
        // the other side is gone too
        furi_stream_buffer_free(pipe->sending);
        furi_stream_buffer_free(pipe->receiving);
        furi_semaphore_free(pipe->shared->instance_count);
        furi_mutex_free(pipe->shared->state_transition);
        free(pipe->shared);
        free(pipe);
    }
}

static void _furi_pipe_stdout_cb(const char* data, size_t size, void* context) {
    furi_assert(context);
    FuriPipeSide* pipe = context;
    while(size) {
        size_t sent = furi_pipe_send(pipe, data, size, FuriWaitForever);
        data += sent;
        size -= sent;
    }
}

static size_t _furi_pipe_stdin_cb(char* data, size_t size, FuriWait timeout, void* context) {
    furi_assert(context);
    FuriPipeSide* pipe = context;
    return furi_pipe_receive(pipe, data, size, timeout);
}

void furi_pipe_install_as_stdio(FuriPipeSide* pipe) {
    furi_check(pipe);
    furi_thread_set_stdout_callback(_furi_pipe_stdout_cb, pipe);
    furi_thread_set_stdin_callback(_furi_pipe_stdin_cb, pipe);
}

size_t furi_pipe_receive(FuriPipeSide* pipe, void* data, size_t length, FuriWait timeout) {
    furi_check(pipe);
    furi_event_loop_link_notify(pipe->peer_event_loop_link, FuriEventLoopEventOut);
    return furi_stream_buffer_receive(pipe->receiving, data, length, timeout);
}

size_t furi_pipe_send(FuriPipeSide* pipe, const void* data, size_t length, FuriWait timeout) {
    furi_check(pipe);
    size_t sent = furi_stream_buffer_send(pipe->sending, data, length, timeout);
    if(furi_stream_buffer_bytes_available(pipe->sending) >=
       furi_stream_get_trigger_level(pipe->sending))
        furi_event_loop_link_notify(pipe->peer_event_loop_link, FuriEventLoopEventIn);
    return sent;
}

size_t furi_pipe_bytes_available(FuriPipeSide* pipe) {
    furi_check(pipe);
    return furi_stream_buffer_bytes_available(pipe->receiving);
}

size_t furi_pipe_spaces_available(FuriPipeSide* pipe) {
    furi_check(pipe);
    return furi_stream_buffer_spaces_available(pipe->sending);
}

static FuriEventLoopLink* furi_pipe_event_loop_get_link(FuriEventLoopObject* object) {
    FuriPipeSide* instance = object;
    furi_assert(instance);
    return instance->self_event_loop_link;
}

static bool furi_pipe_event_loop_get_level(FuriEventLoopObject* object, FuriEventLoopEvent event) {
    FuriPipeSide* instance = object;
    furi_assert(instance);

    if(event == FuriEventLoopEventIn) {
        return furi_stream_buffer_bytes_available(instance->receiving);
    } else if(event == FuriEventLoopEventOut) {
        return furi_stream_buffer_spaces_available(instance->sending);
    } else {
        furi_crash();
    }
}

const FuriEventLoopContract furi_pipe_event_loop_contract = {
    .get_link = furi_pipe_event_loop_get_link,
    .get_level = furi_pipe_event_loop_get_level,
};
