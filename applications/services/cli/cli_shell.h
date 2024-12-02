#pragma once

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLI_SHELL_STACK_SIZE   (1 * 1024U)
#define CLI_COMMAND_STACK_SIZE (3 * 1024U)

FuriThread* cli_shell_start(FuriPipeSide* pipe);

#ifdef __cplusplus
}
#endif
