/**
 * @file cli.h
 * API for registering commands with the CLI
 */

#pragma once
#include <furi.h>
#include <m-bptree.h>
#include "cli_ansi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_CLI "cli"

typedef enum {
    CliCommandFlagDefault = 0, /**< Default */
    CliCommandFlagParallelUnsafe = (1 << 0), /**< Unsafe to run in parallel with other apps */
    CliCommandFlagInsomniaSafe = (1 << 1), /**< Safe to run with insomnia mode on */
    CliCommandFlagDontAttachStdio = (1 << 2), /**< Do no attach I/O pipe to thread stdio */
} CliCommandFlag;

/** Cli type anonymous structure */
typedef struct Cli Cli;

/** 
 * @brief CLI callback function pointer. Implement this interface and use
 * `add_cli_command`.
 * 
 * @param [in] pipe     Pipe that can be used to send and receive data. If
 *                      `CliCommandFlagDontAttachStdio` was not set, you can
 *                      also use standard C functions (printf, getc, etc.) to
 *                      access this pipe.
 * @param [in] args     String with what was passed after the command
 * @param [in] context  Whatever you provided to `cli_add_command`
 */
typedef void (*CliCallback)(FuriPipeSide* pipe, FuriString* args, void* context);

/**
 * @brief Registers a command with the CLI
 *
 * @param [in] cli       Pointer to CLI instance
 * @param [in] name      Command name
 * @param [in] flags     CliCommandFlag
 * @param [in] callback  Callback function
 * @param [in] context   Custom context
 */
void cli_add_command(
    Cli* cli,
    const char* name,
    CliCommandFlag flags,
    CliCallback callback,
    void* context);

/**
 * @brief Deletes a cli command
 *
 * @param [in] cli   pointer to cli instance
 * @param [in] name  command name
 */
void cli_delete_command(Cli* cli, const char* name);

/**
 * @brief Detects if Ctrl+C has been pressed or session has been terminated
 * 
 * @param [in] side Pointer to pipe side given to the command thread
 * @warning This function also assumes that the pipe is installed as the
 *          thread's stdio
 * @warning This function will consume 0 or 1 bytes from the pipe
 */
bool cli_app_should_stop(FuriPipeSide* side);

/** Print unified cmd usage tip
 *
 * @param      cmd    cmd name
 * @param      usage  usage tip
 * @param      arg    arg passed by user
 */
void cli_print_usage(const char* cmd, const char* usage, const char* arg);

#ifdef __cplusplus
}
#endif
