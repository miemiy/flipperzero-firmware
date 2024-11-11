/**
 * @file pipe.h
 * Furi pipe primitive
 * 
 * Pipes are used to send bytes between two threads in both directions. The two
 * threads are referred to as Alice and Bob and their abilities regarding what
 * they can do with the pipe are equal.
 * 
 * It is also possible to use both sides of the pipe within one thread.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "base.h"
#include <stddef.h>

/**
 * @brief The role of a pipe side
 * 
 * Alice and Bob are equal, as they can both read and write the data. This
 * status might be helpful in determining the role of a thread w.r.t. another
 * thread in an application that builds on the pipe.
 * 
 * Joints only allow the `unweld` operation. For more info, see
 * `furi_pipe_weld`.
 */
typedef enum {
    FuriPipeRoleAlice,
    FuriPipeRoleBob,
    FuriPipeRoleJoint,
} FuriPipeRole;

/**
 * @brief The state of a pipe
 * 
 *   - `FuriPipeStateOpen`: Both pipe sides are in place, meaning data that is
 *     sent down the pipe _might_ be read by the peer, and new data sent by the
 *     peer _might_ arrive.
 *   - `FuriPipeStateBroken`: The other side of the pipe has been freed, meaning
 *     data that is written will never reach its destination, and no new data
 *     will appear in the buffer.
 *   - `FuriPipeStateWelded`: The side of the pipe functions as a joint between
 *     two pipes. For more info, see `furi_pipe_weld`.
 * 
 * A broken pipe can never become open again, because there's no way to connect
 * a side of a pipe to another side of a pipe.
 */
typedef enum {
    FuriPipeStateOpen,
    FuriPipeStateBroken,
    FuriPipeStateWelded,
} FuriPipeState;

typedef struct FuriPipeSide FuriPipeSide;

typedef struct {
    FuriPipeSide* alices_side;
    FuriPipeSide* bobs_side;
} FuriPipe;

typedef struct {
    size_t capacity;
    size_t trigger_level;
} FuriPipeDirectionSettings;

/**
 * Controls whether the pipe should support welding or not. This decision should
 * depend on your use case for the pipes:
 *   - If you never want to weld pipes, use non-weldable pipes, as they will be
 *     faster.
 *   - If you want to copy data between pipes, use weldable pipes and weld them
 *     together, as that is faster and more memory efficient than manually
 *     copying data around.
 */
typedef enum {
    FuriPipeWeldingCapEnabled,
    FuriPipeWeldingCapDisabled,
} FuriPipeWeldingCap;

/**
 * @brief Allocates two connected sides of one pipe.
 * 
 * Creating a pair of sides using this function is the only way to connect two
 * pipe sides together. Two unrelated orphaned sides may never be connected back
 * together.
 * 
 * The capacity and trigger level for both directions are the same when the pipe
 * is created using this function. Welding support is enabled, which might be
 * undesirable. Use `furi_pipe_alloc_ex` if you want more control.
 */
FuriPipe furi_pipe_alloc(size_t capacity, size_t trigger_level);

/**
 * @brief Allocates two connected sides of one pipe.
 * 
 * Creating a pair of sides using this function is the only way to connect two
 * pipe sides together. Two unrelated orphaned sides may never be connected back
 * together.
 * 
 * The capacity and trigger level may be different for the two directions when
 * the pipe is created using this function. You can enable or disable welding
 * support, optimizing performance for your exact use case. Use
 * `furi_pipe_alloc` if you don't need control this fine.
 */
FuriPipe furi_pipe_alloc_ex(
    FuriPipeWeldingCap welding_cap,
    FuriPipeDirectionSettings to_alice,
    FuriPipeDirectionSettings to_bob);

/**
 * @brief Gets the role of a pipe side.
 * 
 * The roles (Alice and Bob) are equal, as both can send and receive data. This
 * status might be helpful in determining the role of a thread w.r.t. another
 * thread.
 */
FuriPipeRole furi_pipe_role(FuriPipeSide* pipe);

/**
 * @brief Gets the state of a pipe.
 * 
 * When the state is `FuriPipeStateOpen`, both sides are active and may send or
 * receive data. When the state is `FuriPipeStateBroken`, only one side is
 * active (the one that this method has been called on). If you find yourself in
 * that state, the data that you send will never be heard by anyone, and the
 * data you receive are leftovers in the buffer.
 */
FuriPipeState furi_pipe_state(FuriPipeSide* pipe);

/**
 * @brief Frees a side of a pipe.
 * 
 * When only one of the sides is freed, the pipe is transitioned from the "Open"
 * state into the "Broken" state. When both sides are freed, the underlying data
 * structures are freed too.
 */
void furi_pipe_free(FuriPipeSide* pipe);

/**
 * @brief Connects the pipe to the `stdin` and `stdout` of the current thread.
 * 
 * After performing this operation, you can use `getc`, `puts`, etc. to send and
 * receive data to and from the pipe. If the pipe becomes broken, C stdlib calls
 * will return `EOF` wherever possible.
 * 
 * You can disconnect the pipe by manually calling
 * `furi_thread_set_stdout_callback` and `furi_thread_set_stdin_callback` with
 * `NULL`.
 */
void furi_pipe_install_as_stdio(FuriPipeSide* pipe);

/**
 * @brief Receives data from the pipe.
 */
size_t furi_pipe_receive(FuriPipeSide* pipe, void* data, size_t length, FuriWait timeout);

/**
 * @brief Sends data into the pipe.
 */
size_t furi_pipe_send(FuriPipeSide* pipe, const void* data, size_t length, FuriWait timeout);

/**
 * @brief Determines how many bytes there are in the pipe available to be read.
 */
size_t furi_pipe_bytes_available(FuriPipeSide* pipe);

/**
 * @brief Determines how many space there is in the pipe for data to be written
 * into.
 */
size_t furi_pipe_spaces_available(FuriPipeSide* pipe);

/**
 * @brief Welds two sides of different pipes together.
 * 
 * When two sides of a pipe are welded together, data that appears at `side_1`
 * is automatically pushed into `side_2` and vice versa. This connection may be
 * undone using `furi_pipe_unweld`.
 * 
 * While a side of a pipe is welded to another, it is impossible to use any of
 * the methods to inspect and/or modify the data flowing through the joint:
 *   - `send` and `receive` become no-ops and return 0,
 *   - `bytes_available` and `spaces_available` return 0.
 * 
 * You cannot weld an Alice to an Alice or a Bob to a Bob. You can only weld an
 * Alice to a Bob.
 */
void furi_pipe_weld(FuriPipeSide* side_1, FuriPipeSide* side_2);

/**
 * @brief Undoes a `weld` operation.
 * 
 * See `furi_pipe_weld`.
 */
void furi_pipe_unweld(FuriPipeSide* side);

#ifdef __cplusplus
}
#endif
