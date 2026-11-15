#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void marker(const char* variable) {
    const char* path = getenv(variable);
    if (path != NULL) {
        int file = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        if (file >= 0) {
            close(file);
        }
    }
}

DIR* fdopendir(int descriptor) {
    DIR* (*native)(int) = NULL;
    void* address = dlsym(RTLD_NEXT, "fdopendir");
    _Static_assert(sizeof(native) == sizeof(address), "POSIX pointer representation");
    memcpy(&native, &address, sizeof(native));
    if (native == NULL) {
        _exit(65);
    }
    char descriptor_path[64];
    char path[4097];
    snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", descriptor);
    const ssize_t length = readlink(descriptor_path, path, sizeof(path) - 1);
    const char* root = getenv("RODAQ_CATALOG_BLOCK_ROOT");
    if (length >= 0) {
        path[length] = 0;
    }
    const int blocked = length >= 0 && root != NULL && strcmp(path, root) == 0;
    if (blocked) {
        marker("RODAQ_CATALOG_ENTERED");
        const char* gate = getenv("RODAQ_CATALOG_GATE");
        int file = gate == NULL ? -1 : open(gate, O_RDONLY | O_CLOEXEC);
        char byte = 0;
        if (file < 0 || read(file, &byte, 1) != 1) {
            _exit(66);
        }
        close(file);
    }
    DIR* result = native(descriptor);
    if (blocked) {
        marker("RODAQ_CATALOG_RETURNED");
    }
    return result;
}
