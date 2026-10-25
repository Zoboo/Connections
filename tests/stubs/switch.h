// Host-only service substitutes for testing update orchestration.
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t Result;
typedef uint32_t u32;
typedef int NifmInternetConnectionType;
typedef int NifmInternetConnectionStatus;
#define NifmServiceType_User 0
#define NifmInternetConnectionStatus_Connected 4
#define R_FAILED(r) ((r) != 0)
#define R_SUCCEEDED(r) ((r) == 0)
static bool test_nextload = true;
static int test_restarts;
static Result nifmInitialize(int type) { (void)type; return 0; }
static void nifmExit(void) {}
static Result nifmGetInternetConnectionStatus(int *type, u32 *strength, int *connection) {
    *type = 1; *strength = 3; *connection = 4; return 0;
}
static Result socketInitializeDefault(void) { return 0; }
static void socketExit(void) {}
static bool envHasNextLoad(void) { return test_nextload; }
static Result envSetNextLoad(const char *path, const char *args) {
    (void)path; (void)args; test_restarts++; return 0;
}
static void romfsExit(void) {}
static Result romfsInit(void) { return 0; }
static Result romfsMountFromFsdev(const char *path, uint64_t offset, const char *name) {
    (void)path; (void)offset; (void)name; return 0;
}
