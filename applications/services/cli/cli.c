#include "cli.h"
#include "cli_i.h"
#include "cli_commands.h"
#include "cli_ansi.h"

struct Cli {
    CliCommandTree_t commands;
    FuriMutex* mutex;
};

Cli* cli_alloc(void) {
    Cli* cli = malloc(sizeof(Cli));
    CliCommandTree_init(cli->commands);
    cli->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    return cli;
}

// static void cli_handle_autocomplete(Cli* cli) {
//     cli_normalize_line(cli);

//     if(furi_string_size(cli->line) == 0) {
//         return;
//     }

//     cli_nl(cli);

//     // Prepare common base for autocomplete
//     FuriString* common;
//     common = furi_string_alloc();
//     // Iterate throw commands
//     for
//         M_EACH(cli_command, cli->commands, CliCommandTree_t) {
//             // Process only if starts with line buffer
//             if(furi_string_start_with(*cli_command->key_ptr, cli->line)) {
//                 // Show autocomplete option
//                 printf("%s\r\n", furi_string_get_cstr(*cli_command->key_ptr));
//                 // Process common base for autocomplete
//                 if(furi_string_size(common) > 0) {
//                     // Choose shortest string
//                     const size_t key_size = furi_string_size(*cli_command->key_ptr);
//                     const size_t common_size = furi_string_size(common);
//                     const size_t min_size = key_size > common_size ? common_size : key_size;
//                     size_t i = 0;
//                     while(i < min_size) {
//                         // Stop when do not match
//                         if(furi_string_get_char(*cli_command->key_ptr, i) !=
//                            furi_string_get_char(common, i)) {
//                             break;
//                         }
//                         i++;
//                     }
//                     // Cut right part if any
//                     furi_string_left(common, i);
//                 } else {
//                     // Start with something
//                     furi_string_set(common, *cli_command->key_ptr);
//                 }
//             }
//         }
//     // Replace line buffer if autocomplete better
//     if(furi_string_size(common) > furi_string_size(cli->line)) {
//         furi_string_set(cli->line, common);
//         cli->cursor_position = furi_string_size(cli->line);
//     }
//     // Cleanup
//     furi_string_free(common);
//     // Show prompt
//     cli_prompt(cli);
// }

void cli_add_command(
    Cli* cli,
    const char* name,
    CliCommandFlag flags,
    CliCallback callback,
    void* context) {
    furi_check(cli);
    FuriString* name_str;
    name_str = furi_string_alloc_set(name);
    furi_string_trim(name_str);

    size_t name_replace;
    do {
        name_replace = furi_string_replace(name_str, " ", "_");
    } while(name_replace != FURI_STRING_FAILURE);

    CliCommand c;
    c.callback = callback;
    c.context = context;
    c.flags = flags;

    furi_check(furi_mutex_acquire(cli->mutex, FuriWaitForever) == FuriStatusOk);
    CliCommandTree_set_at(cli->commands, name_str, c);
    furi_check(furi_mutex_release(cli->mutex) == FuriStatusOk);

    furi_string_free(name_str);
}

void cli_delete_command(Cli* cli, const char* name) {
    furi_check(cli);
    FuriString* name_str;
    name_str = furi_string_alloc_set(name);
    furi_string_trim(name_str);

    size_t name_replace;
    do {
        name_replace = furi_string_replace(name_str, " ", "_");
    } while(name_replace != FURI_STRING_FAILURE);

    furi_check(furi_mutex_acquire(cli->mutex, FuriWaitForever) == FuriStatusOk);
    CliCommandTree_erase(cli->commands, name_str);
    furi_check(furi_mutex_release(cli->mutex) == FuriStatusOk);

    furi_string_free(name_str);
}

bool cli_get_command(Cli* cli, FuriString* command, CliCommand* result) {
    furi_assert(cli);
    furi_check(furi_mutex_acquire(cli->mutex, FuriWaitForever) == FuriStatusOk);

    CliCommand* data = CliCommandTree_get(cli->commands, command);
    if(data) *result = *data;

    furi_check(furi_mutex_release(cli->mutex) == FuriStatusOk);

    return !!data;
}

void cli_lock_commands(Cli* cli) {
    furi_assert(cli);
    furi_check(furi_mutex_acquire(cli->mutex, FuriWaitForever) == FuriStatusOk);
}

void cli_unlock_commands(Cli* cli) {
    furi_assert(cli);
    furi_mutex_release(cli->mutex);
}

CliCommandTree_t* cli_get_commands(Cli* cli) {
    furi_assert(cli);
    return &cli->commands;
}

bool cli_app_should_stop(FuriPipeSide* side) {
    if(furi_pipe_state(side) == FuriPipeStateBroken) return true;
    if(!furi_pipe_bytes_available(side)) return false;
    char c = getchar();
    if(c == CliKeyETX) {
        return true;
    } else {
        ungetc(c, stdin);
        return false;
    }
}

void cli_print_usage(const char* cmd, const char* usage, const char* arg) {
    furi_check(cmd);
    furi_check(arg);
    furi_check(usage);

    printf("%s: illegal option -- %s\r\nusage: %s %s", cmd, arg, cmd, usage);
}

void cli_on_system_start(void) {
    Cli* cli = cli_alloc();
    cli_commands_init(cli);
    furi_record_create(RECORD_CLI, cli);
}
