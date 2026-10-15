/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ctr_module.h"
#include <dlfcn.h>
#include <stdio.h>

void snx_ctr_native_close(SnxCtrNative* native) {
    if (!native) return;
    if (native->library) dlclose(native->library);
    *native = (SnxCtrNative){0};
}
bool snx_ctr_native_open(const char* path, const SnxCtrImage* image, SnxCtrNative* native,
                          char* error, size_t error_size) {
    if (!native) return false;
    *native = (SnxCtrNative){0};
    if (error_size) error[0] = 0;
    if (!path || !image) { snprintf(error, error_size, "invalid native module request"); return false; }
    native->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!native->library) { snprintf(error, error_size, "cannot load fresh native module: %s", dlerror()); return false; }
    native->module = dlsym(native->library, "snx_ctr_module_v1");
    if (!snx_ctr_module_compatible(native->module, image)) {
        snprintf(error, error_size, "native module ABI/profile/image identity mismatch");
        snx_ctr_native_close(native);
        return false;
    }
    return true;
}
