#include "cli_shell.h"
#include "cli_ansi.h"
#include "cli_i.h"
#include <stdio.h>
#include <furi_hal_version.h>
#include <m-array.h>
#include <loader/loader.h>

#define TAG "CliShell"

#define HISTORY_DEPTH 10

ARRAY_DEF(ShellHistory, FuriString*, FURI_STRING_OPLIST); // -V524
#define M_OPL_ShellHistory_t() ARRAY_OPLIST(ShellHistory)

typedef struct {
    Cli* cli;

    FuriEventLoop* event_loop;
    FuriPipeSide* pipe;
    CliAnsiParser* ansi_parser;

    size_t history_position;
    size_t line_position;
    ShellHistory_t history;
} CliShell;

typedef struct {
    CliCommand* command;
    FuriPipeSide* pipe;
    FuriString* args;
} CliCommandThreadData;

static int32_t cli_command_thread(void* context) {
    CliCommandThreadData* thread_data = context;
    if(!(thread_data->command->flags & CliCommandFlagDontAttachStdio))
        furi_pipe_install_as_stdio(thread_data->pipe);

    thread_data->command->callback(
        thread_data->pipe, thread_data->args, thread_data->command->context);

    fflush(stdout);
    return 0;
}

static void cli_shell_execute_command(CliShell* cli_shell, FuriString* command) {
    // split command into command and args
    size_t space = furi_string_search_char(command, ' ');
    if(space == FURI_STRING_FAILURE) space = furi_string_size(command);
    FuriString* command_name = furi_string_alloc_set(command);
    furi_string_left(command_name, space);
    FuriString* args = furi_string_alloc_set(command);
    furi_string_right(args, space + 1); // FIXME:

    // find handler
    CliCommand command_data;
    if(!cli_get_command(cli_shell->cli, command_name, &command_data)) {
        printf(
            ANSI_FG_RED "could not find command `%s`" ANSI_RESET,
            furi_string_get_cstr(command_name));
        return;
    }

    // lock loader
    Loader* loader = furi_record_open(RECORD_LOADER);
    if(command_data.flags & CliCommandFlagParallelUnsafe) {
        bool success = loader_lock(loader);
        if(!success) {
            printf(ANSI_FG_RED
                   "this command cannot be run while an application is open" ANSI_RESET);
            return;
        }
    }

    // run command in separate thread
    CliCommandThreadData thread_data = {
        .command = &command_data,
        .pipe = cli_shell->pipe,
        .args = args,
    };
    FuriThread* thread = furi_thread_alloc_ex(
        furi_string_get_cstr(command_name), CLI_SHELL_STACK_SIZE, cli_command_thread, &thread_data);
    furi_thread_start(thread);
    furi_thread_join(thread);
    furi_thread_free(thread);

    furi_string_free(command_name);
    furi_string_free(args);

    // unlock loader
    if(command_data.flags & CliCommandFlagParallelUnsafe) loader_unlock(loader);
}

static void cli_shell_tick(void* context) {
    CliShell* cli_shell = context;
    if(furi_pipe_state(cli_shell->pipe) == FuriPipeStateBroken) {
        furi_event_loop_stop(cli_shell->event_loop);
    }
}

static size_t cli_shell_prompt_length(CliShell* cli_shell) {
    UNUSED(cli_shell);
    return strlen(">: ");
}

static void cli_shell_format_prompt(CliShell* cli_shell, char* buf, size_t length) {
    UNUSED(cli_shell);
    snprintf(buf, length - 1, ">: ");
}

static void cli_shell_prompt(CliShell* cli_shell) {
    char buffer[128];
    cli_shell_format_prompt(cli_shell, buffer, sizeof(buffer));
    printf("\r\n%s", buffer);
    fflush(stdout);
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

typedef enum {
    CliCharClassWord,
    CliCharClassSpace,
    CliCharClassOther,
} CliCharClass;

/**
 * @brief Determines the class that a character belongs to
 * 
 * The return value of this function should not be used on its own; it should
 * only be used for comparing it with other values returned by this function.
 * This function is used internally in `cli_skip_run`.
 */
static CliCharClass cli_char_class(char c) {
    if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
        return CliCharClassWord;
    } else if(c == ' ') {
        return CliCharClassSpace;
    } else {
        return CliCharClassOther;
    }
}

typedef enum {
    CliSkipDirectionLeft,
    CliSkipDirectionRight,
} CliSkipDirection;

/**
 * @brief Skips a run of a class of characters
 * 
 * @param string Input string
 * @param original_pos Position to start the search at
 * @param direction Direction in which to perform the search
 * @returns The position at which the run ends
 */
static size_t cli_skip_run(FuriString* string, size_t original_pos, CliSkipDirection direction) {
    if(furi_string_size(string) == 0) return original_pos;
    if(direction == CliSkipDirectionLeft && original_pos == 0) return original_pos;
    if(direction == CliSkipDirectionRight && original_pos == furi_string_size(string))
        return original_pos;

    int8_t look_offset = (direction == CliSkipDirectionLeft) ? -1 : 0;
    int8_t increment = (direction == CliSkipDirectionLeft) ? -1 : 1;
    int32_t position = original_pos;
    CliCharClass start_class =
        cli_char_class(furi_string_get_char(string, position + look_offset));

    while(true) {
        position += increment;
        if(position < 0) break;
        if(position >= (int32_t)furi_string_size(string)) break;
        if(cli_char_class(furi_string_get_char(string, position + look_offset)) != start_class)
            break;
    }

    return MAX(0, position);
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
    FURI_LOG_T(
        TAG, "mod=%d, key=%d='%c'", key_combo.modifiers, key_combo.key, (char)key_combo.key);

    // do things the user requests
    if(key_combo.modifiers == 0 && key_combo.key == CliKeyETX) { // usually Ctrl+C
        // reset input
        furi_string_reset(*ShellHistory_front(cli_shell->history));
        cli_shell->line_position = 0;
        cli_shell->history_position = 0;
        printf("^C");
        cli_shell_prompt(cli_shell);

    } else if(key_combo.modifiers == 0 && key_combo.key == CliKeyFF) { // usually Ctrl+L
        // clear screen
        FuriString* command = *ShellHistory_cget(cli_shell->history, cli_shell->history_position);
        char prompt[128];
        cli_shell_format_prompt(cli_shell, prompt, sizeof(prompt));
        printf(
            ANSI_ERASE_DISPLAY(ANSI_ERASE_ENTIRE) ANSI_ERASE_SCROLLBACK_BUFFER ANSI_CURSOR_POS(
                "1", "1") "%s%s" ANSI_CURSOR_HOR_POS("%zu"),
            prompt,
            furi_string_get_cstr(command),
            strlen(prompt) + cli_shell->line_position + 1 /* 1-based column indexing */);
        fflush(stdout);

    } else if(key_combo.modifiers == 0 && key_combo.key == CliKeyCR) {
        // get command and update history
        FuriString* command = furi_string_alloc();
        ShellHistory_pop_at(&command, cli_shell->history, cli_shell->history_position);
        furi_string_trim(command);
        if(cli_shell->history_position > 0) ShellHistory_pop_at(NULL, cli_shell->history, 0);
        if(!furi_string_empty(command)) ShellHistory_push_at(cli_shell->history, 0, command);
        FuriString* new_command = furi_string_alloc();
        ShellHistory_push_at(cli_shell->history, 0, new_command);
        furi_string_free(new_command);
        if(ShellHistory_size(cli_shell->history) > HISTORY_DEPTH) {
            ShellHistory_pop_back(NULL, cli_shell->history);
        }

        // execute command
        cli_shell->line_position = 0;
        cli_shell->history_position = 0;
        printf("\r\n");
        cli_shell_execute_command(cli_shell, command);
        furi_string_free(command);
        cli_shell_prompt(cli_shell);

    } else if(key_combo.modifiers == 0 && (key_combo.key == CliKeyUp || key_combo.key == CliKeyDown)) {
        // go up and down in history
        int increment = (key_combo.key == CliKeyUp) ? 1 : -1;
        size_t new_pos = CLAMP(
            (int)cli_shell->history_position + increment,
            (int)ShellHistory_size(cli_shell->history) - 1,
            0);

        // print prompt with selected command
        if(new_pos != cli_shell->history_position) {
            cli_shell->history_position = new_pos;
            FuriString* command =
                *ShellHistory_cget(cli_shell->history, cli_shell->history_position);
            printf(
                ANSI_CURSOR_HOR_POS("1") ">: %s" ANSI_ERASE_LINE(ANSI_ERASE_FROM_CURSOR_TO_END),
                furi_string_get_cstr(command));
            fflush(stdout);
            cli_shell->line_position = furi_string_size(command);
        }

    } else if(
        key_combo.modifiers == 0 &&
        (key_combo.key == CliKeyLeft || key_combo.key == CliKeyRight)) {
        // go left and right in the current line
        FuriString* command = *ShellHistory_cget(cli_shell->history, cli_shell->history_position);
        int increment = (key_combo.key == CliKeyRight) ? 1 : -1;
        size_t new_pos =
            CLAMP((int)cli_shell->line_position + increment, (int)furi_string_size(command), 0);

        // move cursor
        if(new_pos != cli_shell->line_position) {
            cli_shell->line_position = new_pos;
            printf("%s", (increment == 1) ? ANSI_CURSOR_RIGHT_BY("1") : ANSI_CURSOR_LEFT_BY("1"));
            fflush(stdout);
        }

    } else if(key_combo.modifiers == 0 && key_combo.key == CliKeyHome) {
        // go to the start
        cli_shell->line_position = 0;
        printf(ANSI_CURSOR_HOR_POS("%d"), cli_shell_prompt_length(cli_shell) + 1);
        fflush(stdout);

    } else if(key_combo.modifiers == 0 && key_combo.key == CliKeyEnd) {
        // go to the end
        FuriString* line = *ShellHistory_cget(cli_shell->history, cli_shell->history_position);
        cli_shell->line_position = furi_string_size(line);
        printf(
            ANSI_CURSOR_HOR_POS("%zu"),
            cli_shell_prompt_length(cli_shell) + cli_shell->line_position + 1);
        fflush(stdout);

    } else if(
        key_combo.modifiers == 0 &&
        (key_combo.key == CliKeyBackspace || key_combo.key == CliKeyDEL)) {
        // erase one character
        cli_shell_ensure_not_overwriting_history(cli_shell);
        FuriString* line = *ShellHistory_front(cli_shell->history);
        if(cli_shell->line_position == 0) {
            putc(CliKeyBell, stdout);
            fflush(stdout);
            return;
        }
        cli_shell->line_position--;
        furi_string_replace_at(line, cli_shell->line_position, 1, "");

        // move cursor, print the rest of the line, restore cursor
        printf(
            ANSI_CURSOR_LEFT_BY("1") "%s" ANSI_ERASE_LINE(ANSI_ERASE_FROM_CURSOR_TO_END),
            furi_string_get_cstr(line) + cli_shell->line_position);
        size_t left_by = furi_string_size(line) - cli_shell->line_position;
        if(left_by) // apparently LEFT_BY("0") still shifts left by one ._ .
            printf(ANSI_CURSOR_LEFT_BY("%zu"), left_by);
        fflush(stdout);

    } else if(
        key_combo.modifiers == CliModKeyCtrl &&
        (key_combo.key == CliKeyLeft || key_combo.key == CliKeyRight)) {
        // skip run of similar chars to the left or right
        FuriString* line = *ShellHistory_cget(cli_shell->history, cli_shell->history_position);
        CliSkipDirection direction = (key_combo.key == CliKeyLeft) ? CliSkipDirectionLeft :
                                                                     CliSkipDirectionRight;
        cli_shell->line_position = cli_skip_run(line, cli_shell->line_position, direction);
        printf(
            ANSI_CURSOR_HOR_POS("%zu"),
            cli_shell_prompt_length(cli_shell) + cli_shell->line_position + 1);
        fflush(stdout);

    } else if(key_combo.modifiers == 0 && key_combo.key == CliKeyETB) {
        // delete run of similar chars to the left
        cli_shell_ensure_not_overwriting_history(cli_shell);
        FuriString* line = *ShellHistory_cget(cli_shell->history, cli_shell->history_position);
        size_t run_start = cli_skip_run(line, cli_shell->line_position, CliSkipDirectionLeft);
        furi_string_replace_at(line, run_start, cli_shell->line_position - run_start, "");
        cli_shell->line_position = run_start;
        printf(
            ANSI_CURSOR_HOR_POS("%zu") "%s" ANSI_ERASE_LINE(ANSI_ERASE_FROM_CURSOR_TO_END)
                ANSI_CURSOR_HOR_POS("%zu"),
            cli_shell_prompt_length(cli_shell) + cli_shell->line_position + 1,
            furi_string_get_cstr(line) + run_start,
            cli_shell_prompt_length(cli_shell) + run_start + 1);
        fflush(stdout);

    } else if(key_combo.modifiers == 0 && key_combo.key >= CliKeySpace && key_combo.key < CliKeyDEL) {
        // insert character
        cli_shell_ensure_not_overwriting_history(cli_shell);
        FuriString* line = *ShellHistory_front(cli_shell->history);
        if(cli_shell->line_position == furi_string_size(line)) {
            furi_string_push_back(line, key_combo.key);
            printf("%c", key_combo.key);
        } else {
            const char in_str[2] = {key_combo.key, 0};
            furi_string_replace_at(line, cli_shell->line_position, 0, in_str);
            printf(ANSI_INSERT_MODE_ENABLE "%c" ANSI_INSERT_MODE_DISABLE, key_combo.key);
        }
        fflush(stdout);
        cli_shell->line_position++;
    }
}

static CliShell* cli_shell_alloc(FuriPipeSide* pipe) {
    CliShell* cli_shell = malloc(sizeof(CliShell));
    cli_shell->cli = furi_record_open(RECORD_CLI);
    cli_shell->ansi_parser = cli_ansi_parser_alloc();
    cli_shell->event_loop = furi_event_loop_alloc();
    ShellHistory_init(cli_shell->history);
    FuriString* new_command = furi_string_alloc();
    ShellHistory_push_at(cli_shell->history, 0, new_command);
    furi_string_free(new_command);

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
    furi_pipe_free(cli_shell->pipe);
    ShellHistory_clear(cli_shell->history);
    furi_event_loop_free(cli_shell->event_loop);
    cli_ansi_parser_free(cli_shell->ansi_parser);
    furi_record_close(RECORD_CLI);
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

FuriThread* cli_shell_start(FuriPipeSide* pipe) {
    FuriThread* thread =
        furi_thread_alloc_ex("CliShell", CLI_SHELL_STACK_SIZE, cli_shell_thread, pipe);
    furi_thread_start(thread);
    return thread;
}
