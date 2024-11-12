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
 * Both roles are equal, as they can both read and write the data. This status
 * might be helpful in determining the role of a thread w.r.t. another thread in
 * an application that builds on the pipe.
 */
typedef enum {
    FuriPipeRoleAlice,
    FuriPipeRoleBob,
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
 * 
 * A broken pipe can never become open again, because there's no way to connect
 * a side of a pipe to another side of a pipe.
 */
typedef enum {
    FuriPipeStateOpen,
    FuriPipeStateBroken,
} FuriPipeState;

typedef struct FuriPipeSide FuriPipeSide;

typedef struct {
    FuriPipeSide* alices_side;
    FuriPipeSide* bobs_side;
} FuriPipe;

typedef struct {
    size_t capacity;
    size_t trigger_level;
} FuriPipeSideReceiveSettings;

/**
 * @brief Allocates two connected sides of one pipe.
 * 
 * Creating a pair of sides using this function is the only way to connect two
 * pipe sides together. Two unrelated orphaned sides may never be connected back
 * together.
 * 
 * The capacity and trigger level for both directions are the same when the pipe
 * is created using this function. Use `furi_pipe_alloc_ex` if you want more
 * control.
 * 
 * @param capacity Maximum number of bytes buffered in one direction
 * @param trigger_level Number of bytes that need to be available in the buffer
 *                      in order for a blocked thread to unblock
 * @returns Bundle with both sides of the pipe
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
 * the pipe is created using this function. Use `furi_pipe_alloc` if you don't
 * need control this fine.
 * 
 * @param alice `capacity` and `trigger_level` settings for Alice's receiving
 *              buffer
 * @param bob `capacity` and `trigger_level` settings for Bob's receiving buffer
 * @returns Bundle with both sides of the pipe
 */
FuriPipe furi_pipe_alloc_ex(FuriPipeSideReceiveSettings alice, FuriPipeSideReceiveSettings bob);

/**
 * @brief Gets the role of a pipe side.
 * 
 * The roles (Alice and Bob) are equal, as both can send and receive data. This
 * status might be helpful in determining the role of a thread w.r.t. another
 * thread.
 * 
 * @param [in] pipe Pipe side to query
 * @returns Role of provided pipe side
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
 * 
 * @param [in] pipe Pipe side to query
 * @returns State of the pipe
 */
FuriPipeState furi_pipe_state(FuriPipeSide* pipe);

/**
 * @brief Frees a side of a pipe.
 * 
 * When only one of the sides is freed, the pipe is transitioned from the "Open"
 * state into the "Broken" state. When both sides are freed, the underlying data
 * structures are freed too.
 * 
 * @param [in] pipe Pipe side to free
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
 * 
 * @param [in] pipe Pipe side to connect to the stdio
 */
void furi_pipe_install_as_stdio(FuriPipeSide* pipe);

/**
 * @brief Receives data from the pipe.
 * 
 * @param [in] pipe The pipe side to read data out of
 * @param [out] data The buffer to fill with data
 * @param length Maximum length of data to read
 * @param timeout The timeout (in ticks) after which the read operation is
 *                interrupted
 * @returns The number of bytes actually written into the provided buffer
 */
size_t furi_pipe_receive(FuriPipeSide* pipe, void* data, size_t length, FuriWait timeout);

/**
 * @brief Sends data into the pipe.
 * 
 * @param [in] pipe The pipe side to send data into
 * @param [out] data The buffer to get data from
 * @param length Maximum length of data to send
 * @param timeout The timeout (in ticks) after which the write operation is
 *                interrupted
 * @returns The number of bytes actually read from the provided buffer
 */
size_t furi_pipe_send(FuriPipeSide* pipe, const void* data, size_t length, FuriWait timeout);

/**
 * @brief Determines how many bytes there are in the pipe available to be read.
 * 
 * @param [in] Pipe side to query
 * @returns Number of bytes available to be read out from that side of the pipe
 */
size_t furi_pipe_bytes_available(FuriPipeSide* pipe);

/**
 * @brief Determines how many space there is in the pipe for data to be written
 * into.
 * 
 * @param [in] Pipe side to query
 * @returns Number of bytes available to be written into that side of the pipe
 */
size_t furi_pipe_spaces_available(FuriPipeSide* pipe);

#ifdef __cplusplus
}
#endif
