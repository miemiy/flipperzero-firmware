#include "cli_shell.h"
#include <stdio.h>

#define TAG "CliShell"

typedef struct {
    FuriEventLoop* event_loop;
    FuriPipeSide* pipe;
} CliShell;

static void cli_shell_tick(void* context) {
    CliShell* cli_shell = context;
    if(furi_pipe_state(cli_shell->pipe) == FuriPipeStateBroken) {
        furi_event_loop_stop(cli_shell->event_loop);
    }
}

static void cli_shell_data_available(FuriEventLoopObject* object, void* context) {
    UNUSED(object);
    CliShell* cli_shell = context;
    UNUSED(cli_shell);

    int c = getchar();
    printf("You typed: %c\n", c);
}

static CliShell* cli_shell_alloc(FuriPipeSide* pipe) {
    CliShell* cli_shell = malloc(sizeof(CliShell));
    cli_shell->event_loop = furi_event_loop_alloc();

    cli_shell->pipe = pipe;
    furi_pipe_install_as_stdio(cli_shell->pipe);
    furi_event_loop_subscribe_pipe(
        cli_shell->event_loop,
        cli_shell->pipe,
        FuriEventLoopEventIn,
        cli_shell_data_available,
        cli_shell);

    furi_event_loop_pend_callback(cli_shell->event_loop, cli_shell_tick, cli_shell);

    return cli_shell;
}

static void cli_shell_free(CliShell* cli_shell) {
    furi_event_loop_unsubscribe(cli_shell->event_loop, cli_shell->pipe);
    furi_event_loop_free(cli_shell->event_loop);
    furi_pipe_free(cli_shell->pipe);
    free(cli_shell);
}

static int32_t cli_shell_thread(void* context) {
    FuriPipeSide* pipe = context;
    CliShell* cli_shell = cli_shell_alloc(pipe);

    FURI_LOG_T(TAG, "Started");

    const char* long_str =
        "Hello, World! This is a very long string to test out how my new VCP service handles oddly-sized blocks. In addition to this, I'm going to make it longer than the pipe buffer, just to see how things work out. This string should be plenty long already, so I'm going to wrap this up!";
    char buf[strlen(long_str) + 3];
    for(size_t s = 1; s <= strlen(long_str); s++) {
        memcpy(buf, long_str, s);
        memcpy(buf + s, "|", 2);
        puts(buf);
    }

    furi_event_loop_run(cli_shell->event_loop);
    FURI_LOG_T(TAG, "Stopped");

    cli_shell_free(cli_shell);
    return 0;
}

void cli_shell_start(FuriPipeSide* pipe) {
    FuriThread* thread =
        furi_thread_alloc_ex("CliShell", CLI_SHELL_STACK_SIZE, cli_shell_thread, pipe);
    furi_thread_start(thread);
}
