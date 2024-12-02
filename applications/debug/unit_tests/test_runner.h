#pragma once

#include <furi.h>

typedef struct TestRunner TestRunner;

TestRunner* test_runner_alloc(FuriPipeSide* pipe, FuriString* args);

void test_runner_free(TestRunner* instance);

void test_runner_run(TestRunner* instance);
