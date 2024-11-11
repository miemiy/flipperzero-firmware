#include "pipe.h"
#include "stream_buffer.h"
#include "mutex.h"
#include "check.h"
#include "memmgr.h"
#include <m-array.h>

ARRAY_DEF(PipeSideArray, FuriPipeSide*, M_PTR_OPLIST);
#define M_OPL_PipeSideArray_t() ARRAY_OPLIST(PipeSideArray)

/**
 * A chain of pipes that have been welded together. Initially, pipes consist of
 * a chain of length 1.
 */
typedef struct {
    FuriStreamBuffer* alice_to_bob;
    FuriStreamBuffer* bob_to_alice;
    PipeSideArray_t pipe_sides; // <! in order of travel from Alice to Bob
} FuriPipeChain;

struct FuriPipeSide {
    FuriMutex* mutex;
    FuriPipeRole role;
    FuriPipeChain* chain;
    FuriStreamBuffer* sending;
    FuriStreamBuffer* receiving;
    FuriPipeDirectionSettings send_settings; // <! for restoring when a weld is undone
};

FuriPipe furi_pipe_alloc(size_t capacity, size_t trigger_level) {
    FuriPipeDirectionSettings settings = {
        .capacity = capacity,
        .trigger_level = trigger_level,
    };
    return furi_pipe_alloc_ex(FuriPipeWeldingCapEnabled, settings, settings);
}

FuriPipe furi_pipe_alloc_ex(
    FuriPipeWeldingCap welding_cap,
    FuriPipeDirectionSettings to_alice,
    FuriPipeDirectionSettings to_bob) {
    FuriStreamBuffer* alice_to_bob =
        furi_stream_buffer_alloc(to_bob.capacity, to_bob.trigger_level);
    FuriStreamBuffer* bob_to_alice =
        furi_stream_buffer_alloc(to_alice.capacity, to_alice.trigger_level);

    FuriPipeChain* chain = malloc(sizeof(FuriPipeChain));
    *chain = (FuriPipeChain){
        .alice_to_bob = alice_to_bob,
        .bob_to_alice = bob_to_alice,
    };
    PipeSideArray_init(chain->pipe_sides);

    FuriPipeSide* alices_side = malloc(sizeof(FuriPipeSide));
    FuriPipeSide* bobs_side = malloc(sizeof(FuriPipeSide));
    PipeSideArray_push_back(chain->pipe_sides, alices_side);
    PipeSideArray_push_back(chain->pipe_sides, bobs_side);

    *alices_side = (FuriPipeSide){
        .mutex = (welding_cap == FuriPipeWeldingCapEnabled) ?
                     furi_mutex_alloc(FuriMutexTypeRecursive) :
                     NULL,
        .role = FuriPipeRoleAlice,
        .chain = chain,
        .sending = alice_to_bob,
        .receiving = bob_to_alice,
        .send_settings = to_bob,
    };
    *bobs_side = (FuriPipeSide){
        .mutex = (welding_cap == FuriPipeWeldingCapEnabled) ?
                     furi_mutex_alloc(FuriMutexTypeNormal) :
                     NULL,
        .role = FuriPipeRoleBob,
        .chain = chain,
        .sending = bob_to_alice,
        .receiving = alice_to_bob,
        .send_settings = to_alice,
    };

    return (FuriPipe){.alices_side = alices_side, .bobs_side = bobs_side};
}

static FURI_ALWAYS_INLINE void furi_pipe_side_lock(FuriPipeSide* pipe) {
    if(pipe->mutex) furi_mutex_acquire(pipe->mutex, FuriWaitForever);
}

static FURI_ALWAYS_INLINE void furi_pipe_side_unlock(FuriPipeSide* pipe) {
    if(pipe->mutex) furi_mutex_release(pipe->mutex);
}

FuriPipeRole furi_pipe_role(FuriPipeSide* pipe) {
    furi_check(pipe);
    furi_pipe_side_lock(pipe);
    FuriPipeRole role = pipe->role;
    furi_pipe_side_unlock(pipe);
    return role;
}

FuriPipeState furi_pipe_state(FuriPipeSide* pipe) {
    furi_check(pipe);
    furi_pipe_side_lock(pipe);
    size_t sides = PipeSideArray_size(pipe->chain->pipe_sides);
    furi_pipe_side_unlock(pipe);
    return (sides % 2) ? FuriPipeStateBroken : FuriPipeStateOpen;
}

void furi_pipe_free(FuriPipeSide* pipe) {
    furi_check(pipe);

    furi_pipe_side_lock(pipe);
    furi_check(pipe->role != FuriPipeRoleJoint); // unweld first

    size_t sides = PipeSideArray_size(pipe->chain->pipe_sides);
    furi_check(sides <= 2); // TODO: support chains!

    if(sides == 1) {
        // the other side is gone too
        furi_stream_buffer_free(pipe->sending);
        furi_stream_buffer_free(pipe->receiving);
        PipeSideArray_clear(pipe->chain->pipe_sides);
        if(pipe->mutex) furi_mutex_free(pipe->mutex);
        free(pipe->chain);
        free(pipe);
    } else {
        // the other side is still intact
        PipeSideArray_it_t it;
        for(PipeSideArray_it(it, pipe->chain->pipe_sides); !PipeSideArray_end_p(it);
            PipeSideArray_next(it)) {
            if(*PipeSideArray_cref(it) == pipe) break;
        }
        furi_check(!PipeSideArray_end_p(it));
        PipeSideArray_remove(pipe->chain->pipe_sides, it);
        if(pipe->mutex) furi_mutex_free(pipe->mutex);
        free(pipe);
    }
}

static void _furi_pipe_stdout_cb(const char* data, size_t size, void* context) {
    furi_assert(context);
    FuriPipeSide* pipe = context;
    furi_check(furi_stream_buffer_send(pipe->sending, data, size, FuriWaitForever) == size);
}

static size_t _furi_pipe_stdin_cb(char* data, size_t size, FuriWait timeout, void* context) {
    furi_assert(context);
    FuriPipeSide* pipe = context;
    return furi_stream_buffer_receive(pipe->receiving, data, size, timeout);
}

void furi_pipe_install_as_stdio(FuriPipeSide* pipe) {
    furi_check(pipe);
    furi_thread_set_stdout_callback(_furi_pipe_stdout_cb, pipe);
    furi_thread_set_stdin_callback(_furi_pipe_stdin_cb, pipe);
}

size_t furi_pipe_receive(FuriPipeSide* pipe, void* data, size_t length, FuriWait timeout) {
    furi_check(pipe);
    furi_pipe_side_lock(pipe);
    FuriStreamBuffer* buffer = pipe->receiving;
    size_t received = 0;
    if(buffer) received = furi_stream_buffer_receive(buffer, data, length, timeout);
    furi_pipe_side_unlock(pipe);
    return received;
}

size_t furi_pipe_send(FuriPipeSide* pipe, const void* data, size_t length, FuriWait timeout) {
    furi_check(pipe);
    furi_pipe_side_lock(pipe);
    FuriStreamBuffer* buffer = pipe->sending;
    size_t sent = 0;
    if(buffer) sent = furi_stream_buffer_send(buffer, data, length, timeout);
    furi_pipe_side_unlock(pipe);
    return sent;
}

size_t furi_pipe_bytes_available(FuriPipeSide* pipe) {
    furi_check(pipe);
    furi_pipe_side_lock(pipe);
    FuriStreamBuffer* buffer = pipe->receiving;
    size_t available = 0;
    if(buffer) available = furi_stream_buffer_bytes_available(buffer);
    furi_pipe_side_unlock(pipe);
    return available;
}

size_t furi_pipe_spaces_available(FuriPipeSide* pipe) {
    furi_check(pipe);
    furi_pipe_side_lock(pipe);
    FuriStreamBuffer* buffer = pipe->sending;
    size_t available = 0;
    if(buffer) available = furi_stream_buffer_spaces_available(buffer);
    furi_pipe_side_unlock(pipe);
    return available;
}

void furi_pipe_weld(FuriPipeSide* side_1, FuriPipeSide* side_2) {
    // Here's a pipe:
    //
    //     |         |
    //   s |=========| r
    // ----|---->----|----
    // ----|----<----|----
    //   r |=========| s
    //     |         |
    //     A         B
    //
    // It's got two sides (_A_lice and _B_ob) and two StreamBuffers backing it (A to B and B to A).
    // From Alice's perspective, A>B is the _s_ending stream, and A<B is the _r_eceiving stream.
    // It's the other way around for Bob.
    //
    // Here's two pipes:
    //
    //     |         |          |         |
    //   s |=========| r      s |=========| r
    // ----|---->----|----  ----|---->----|----
    // ----|----<----|----  ----|----<----|----
    //   r |=========| s      r |=========| s
    //     |         |          |         |
    //     A         B          A         B
    //
    // We want to "weld" faces iB and iA ("intermediate" Alice and Bob), forming a new "pipe chain"
    // with the ends cA and cB ("chain" Alice and Bob):
    //
    //     |         |     |         |
    //   s |=========|=====|=========| r
    // ----|------------>------------|----
    // ----|------------<------------|----
    //   r |=========|=====|=========| s
    //     |         |     |         |
    //    cA        iB    iA        cB
    //
    // By only using one StreamBuffer per direction for two welded pipes, we can avoid having to
    // copy data around and increase performance. At the cost, of course, of no longer being able
    // to inspect, inject, modify or otherwise do anything with the data at faces iB and iA.

    furi_check(side_1);
    furi_check(side_2);

    // both sides must be weldable
    furi_check(side_1->mutex);
    furi_check(side_2->mutex);

    furi_pipe_side_lock(side_1);
    furi_pipe_side_lock(side_2);

    // cannot weld an already welded side
    furi_check(side_1->role != FuriPipeRoleJoint);
    furi_check(side_2->role != FuriPipeRoleJoint);

    // can only weld an Alice to a Bob
    furi_check(side_1->role != side_2->role);

    FuriPipeSide* intermediate_alice = (side_1->role == FuriPipeRoleAlice) ? side_1 : side_2;
    FuriPipeSide* intermediate_bob = (side_2->role == FuriPipeRoleBob) ? side_2 : side_1;

    // cannot weld two ends of the same chain
    furi_check(intermediate_alice->chain != intermediate_bob->chain);

    FuriPipeChain* left_chain = intermediate_bob->chain;
    FuriPipeChain* right_chain = intermediate_alice->chain;

    // lock all sides in both chains
    for
        M_EACH(side, left_chain->pipe_sides, PipeSideArray_t) {
            // already locked Bob near the beginning
            if(*side != intermediate_bob) furi_pipe_side_lock(*side);
        }
    for
        M_EACH(side, right_chain->pipe_sides, PipeSideArray_t) {
            // already locked Alice near the beginning
            if(*side != intermediate_alice) furi_pipe_side_lock(*side);
        }

    // copy residual data
    do {
        size_t buf_size =
            MAX(furi_stream_buffer_bytes_available(left_chain->alice_to_bob),
                furi_stream_buffer_bytes_available(right_chain->bob_to_alice));
        uint8_t buf[buf_size];

        size_t to_copy = furi_stream_buffer_receive(left_chain->alice_to_bob, buf, buf_size, 0);
        furi_stream_buffer_send(right_chain->alice_to_bob, buf, to_copy, 0);
        furi_check(
            furi_stream_buffer_bytes_available(left_chain->alice_to_bob) == 0); // all data copied

        to_copy = furi_stream_buffer_receive(right_chain->bob_to_alice, buf, buf_size, 0);
        furi_stream_buffer_send(left_chain->bob_to_alice, buf, to_copy, 0);
        furi_check(
            furi_stream_buffer_bytes_available(right_chain->bob_to_alice) == 0); // all data copied
    } while(0);

    // concat right chain to left chain
    for
        M_EACH(side, right_chain->pipe_sides, PipeSideArray_t) {
            (*side)->chain = left_chain;
            PipeSideArray_push_back(left_chain->pipe_sides, *side);
        }

    // free unneeded things
    furi_stream_buffer_free(left_chain->alice_to_bob);
    furi_stream_buffer_free(right_chain->bob_to_alice);
    left_chain->alice_to_bob = right_chain->alice_to_bob;
    PipeSideArray_clear(right_chain->pipe_sides);
    free(right_chain);

    // update intermediate sides
    intermediate_bob->role = FuriPipeRoleJoint;
    intermediate_bob->sending = NULL;
    intermediate_bob->receiving = NULL;
    intermediate_alice->role = FuriPipeRoleJoint;
    intermediate_alice->sending = NULL;
    intermediate_alice->receiving = NULL;

    // update endpoint (chain) sides
    FuriPipeSide* chain_alice = *PipeSideArray_front(left_chain->pipe_sides);
    FuriPipeSide* chain_bob = *PipeSideArray_back(left_chain->pipe_sides);
    chain_alice->sending = left_chain->alice_to_bob;
    chain_bob->sending = left_chain->bob_to_alice;

    // unlock all sides
    for
        M_EACH(side, left_chain->pipe_sides, PipeSideArray_t) {
            furi_pipe_side_unlock(*side);
        }
}

void furi_pipe_unweld(FuriPipeSide* side) {
    UNUSED(side);
    furi_crash("unimplemented"); // TODO:
}
