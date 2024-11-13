#pragma once

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLI_SHELL_STACK_SIZE (1 * 1024U)

void cli_shell_start(FuriPipeSide* pipe);

#ifdef __cplusplus
}
#endif
