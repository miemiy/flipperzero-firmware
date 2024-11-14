#include "cli_shell.h"
#include "cli_ansi.h"
#include <stdio.h>
#include <furi_hal_version.h>
#include <m-array.h>

#define TAG "CliShell"

#define HISTORY_DEPTH 10

ARRAY_DEF(ShellHistory, FuriString*, FURI_STRING_OPLIST);
#define M_OPL_ShellHistory_t() ARRAY_OPLIST(ShellHistory)

typedef struct {
    FuriEventLoop* event_loop;
    FuriPipeSide* pipe;
    CliAnsiParser* ansi_parser;

    size_t history_position;
    size_t line_position;
    ShellHistory_t history;
} CliShell;

static void cli_shell_tick(void* context) {
    CliShell* cli_shell = context;
    if(furi_pipe_state(cli_shell->pipe) == FuriPipeStateBroken) {
        furi_event_loop_stop(cli_shell->event_loop);
    }
}

static void cli_shell_prompt(CliShell* cli_shell) {
    UNUSED(cli_shell);
    printf("\r\n>: ");
    fflush(stdout);
}

static void cli_shell_dump_history(CliShell* cli_shell) {
    FURI_LOG_T(TAG, "history depth=%d, entries:", ShellHistory_size(cli_shell->history));
    for
        M_EACH(entry, cli_shell->history, ShellHistory_t) {
            FURI_LOG_T(TAG, "    \"%s\"", furi_string_get_cstr(*entry));
        }
}

/**
 * If a line from history has been selected, moves it into the active line
 */
static void cli_shell_ensure_not_overwriting_history(CliShell* cli_shell) {
    if(cli_shell->history_position > 0) {
        FuriString* source = *ShellHistory_cget(cli_shell->history, cli_shell->history_position);
        FuriString* destination = *ShellHistory_front(cli_shell->history);
        furi_string_set(destination, source);
        cli_shell->history_position = 0;
    }
}

static void cli_shell_data_available(FuriEventLoopObject* object, void* context) {
    UNUSED(object);
    CliShell* cli_shell = context;
    UNUSED(cli_shell);

    // process ANSI escape sequences
    int c = getchar();
    furi_assert(c >= 0);
    CliAnsiParserResult parse_result = cli_ansi_parser_feed(cli_shell->ansi_parser, c);
    if(!parse_result.is_done) return;
    CliKeyCombo key_combo = parse_result.result;
    if(key_combo.key == CliKeyUnrecognized) return;
    FURI_LOG_T(TAG, "mod=%d, key='%c'=%d", key_combo.modifiers, key_combo.key, key_combo.key);

    // do things the user requests
    if(key_combo.modifiers == 0 && key_combo.key == CliKeyETX) { // usually Ctrl+C
        // reset input
        furi_string_reset(*ShellHistory_front(cli_shell->history));
        cli_shell->line_position = 0;
        cli_shell->history_position = 0;
        printf("^C");
        cli_shell_prompt(cli_shell);

    } else if(key_combo.modifiers == 0 && key_combo.key == CliKeyLF) {
        // get command and update history
        cli_shell_dump_history(cli_shell);
        FuriString* command = furi_string_alloc();
        ShellHistory_pop_at(&command, cli_shell->history, cli_shell->history_position);
        if(cli_shell->history_position > 0) ShellHistory_pop_at(NULL, cli_shell->history, 0);
        if(!furi_string_empty(command)) ShellHistory_push_at(cli_shell->history, 0, command);
        ShellHistory_push_at(cli_shell->history, 0, furi_string_alloc());
        if(ShellHistory_size(cli_shell->history) > HISTORY_DEPTH) {
            ShellHistory_pop_back(NULL, cli_shell->history);
        }
        cli_shell_dump_history(cli_shell);

        // execute command
        cli_shell->line_position = 0;
        cli_shell->history_position = 0;
        printf("\r\ncommand input: \"%s\"", furi_string_get_cstr(command));
        cli_shell_prompt(cli_shell);

    } else if(key_combo.modifiers == 0 && (key_combo.key == CliKeyUp || key_combo.key == CliKeyDown)) {
        // go up and down in history
        int increment = (key_combo.key == CliKeyUp) ? 1 : -1;
        cli_shell->history_position = CLAMP(
            (int)cli_shell->history_position + increment,
            (int)ShellHistory_size(cli_shell->history) - 1,
            0);

        // print prompt with selected command
        FuriString* command = *ShellHistory_cget(cli_shell->history, cli_shell->history_position);
        printf(
            ANSI_CURSOR_HOR_POS("1") ">: %s" ANSI_ERASE_LINE(ANSI_ERASE_FROM_CURSOR_TO_END),
            furi_string_get_cstr(command));
        fflush(stdout);
        cli_shell->line_position = furi_string_size(command);

    } else if(key_combo.modifiers == 0 && key_combo.key >= CliKeySpace && key_combo.key < CliKeyDEL) {
        cli_shell_ensure_not_overwriting_history(cli_shell);

        // insert character
        FuriString* line = *ShellHistory_front(cli_shell->history);
        if(cli_shell->line_position == furi_string_size(line)) {
            furi_string_push_back(line, key_combo.key);
            putc(key_combo.key, stdout);
            fflush(stdout);
        } else {
            const char in_str[2] = {key_combo.key, 0};
            furi_string_replace_at(line, cli_shell->line_position, 0, in_str);
            printf("\e[4h%c\e[4l", key_combo.key);
            fflush(stdout);
        }
        cli_shell->line_position++;
    }
}

static CliShell* cli_shell_alloc(FuriPipeSide* pipe) {
    CliShell* cli_shell = malloc(sizeof(CliShell));
    cli_shell->ansi_parser = cli_ansi_parser_alloc();
    cli_shell->event_loop = furi_event_loop_alloc();
    ShellHistory_init(cli_shell->history);
    ShellHistory_push_at(cli_shell->history, 0, furi_string_alloc());

    cli_shell->pipe = pipe;
    furi_pipe_install_as_stdio(cli_shell->pipe);
    furi_event_loop_subscribe_pipe(
        cli_shell->event_loop,
        cli_shell->pipe,
        FuriEventLoopEventIn,
        cli_shell_data_available,
        cli_shell);

    furi_event_loop_tick_set(cli_shell->event_loop, 1, cli_shell_tick, cli_shell);

    return cli_shell;
}

static void cli_shell_free(CliShell* cli_shell) {
    furi_event_loop_unsubscribe(cli_shell->event_loop, cli_shell->pipe);
    furi_event_loop_free(cli_shell->event_loop);
    furi_pipe_free(cli_shell->pipe);
    ShellHistory_clear(cli_shell->history);
    cli_ansi_parser_free(cli_shell->ansi_parser);
    free(cli_shell);
}

static void cli_shell_motd(void) {
    printf(ANSI_FLIPPER_BRAND_ORANGE
           "\r\n"
           "              _.-------.._                    -,\r\n"
           "          .-\"```\"--..,,_/ /`-,               -,  \\ \r\n"
           "       .:\"          /:/  /'\\  \\     ,_...,  `. |  |\r\n"
           "      /       ,----/:/  /`\\ _\\~`_-\"`     _;\r\n"
           "     '      / /`\"\"\"'\\ \\ \\.~`_-'      ,-\"'/ \r\n"
           "    |      | |  0    | | .-'      ,/`  /\r\n"
           "   |    ,..\\ \\     ,.-\"`       ,/`    /\r\n"
           "  ;    :    `/`\"\"\\`           ,/--==,/-----,\r\n"
           "  |    `-...|        -.___-Z:_______J...---;\r\n"
           "  :         `                           _-'\r\n"
           " _L_  _     ___  ___  ___  ___  ____--\"`___  _     ___\r\n"
           "| __|| |   |_ _|| _ \\| _ \\| __|| _ \\   / __|| |   |_ _|\r\n"
           "| _| | |__  | | |  _/|  _/| _| |   /  | (__ | |__  | |\r\n"
           "|_|  |____||___||_|  |_|  |___||_|_\\   \\___||____||___|\r\n"
           "\r\n" ANSI_FG_BR_WHITE "Welcome to Flipper Zero Command Line Interface!\r\n"
           "Read the manual: https://docs.flipper.net/development/cli\r\n"
           "Run `help` or `?` to list available commands\r\n"
           "\r\n" ANSI_RESET);

    const Version* firmware_version = furi_hal_version_get_firmware_version();
    if(firmware_version) {
        printf(
            "Firmware version: %s %s (%s%s built on %s)\r\n",
            version_get_gitbranch(firmware_version),
            version_get_version(firmware_version),
            version_get_githash(firmware_version),
            version_get_dirty_flag(firmware_version) ? "-dirty" : "",
            version_get_builddate(firmware_version));
    }
}

static int32_t cli_shell_thread(void* context) {
    FuriPipeSide* pipe = context;
    CliShell* cli_shell = cli_shell_alloc(pipe);

    FURI_LOG_D(TAG, "Started");
    cli_shell_motd();
    cli_shell_prompt(cli_shell);
    furi_event_loop_run(cli_shell->event_loop);
    FURI_LOG_D(TAG, "Stopped");

    cli_shell_free(cli_shell);
    return 0;
}

void cli_shell_start(FuriPipeSide* pipe) {
    FuriThread* thread =
        furi_thread_alloc_ex("CliShell", CLI_SHELL_STACK_SIZE, cli_shell_thread, pipe);
    furi_thread_start(thread);
}
