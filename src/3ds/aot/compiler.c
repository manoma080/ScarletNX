/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Separate offline compiler executable. Never creates an execution engine. */
#include "ctr_module.h"
#include "cpu/arm/llvm.h"
#include "common.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(SNX_ARM_CODEGEN_OPTIONS_ABI == SNX_CTR_MODULE_CODEGEN_ABI, "codegen ABI");
_Static_assert(SNX_ARM_MEMORY_CHECKED == SNX_CTR_MODULE_CHECKED_MODE, "memory ABI");
static uint32_t little(const unsigned char* p, unsigned size) {
    uint32_t result = 0;
    for (unsigned i = 0; i < size; ++i) result |= (uint32_t)p[i] << (i * 8);
    return result;
}
static LLVMValueRef table(LLVMModuleRef mod, const char* name, LLVMValueRef* functions, unsigned count) {
    LLVMTypeRef ptr = LLVMPointerTypeInContext(LLVMGetModuleContext(mod), 0);
    LLVMValueRef result = LLVMAddGlobal(mod, LLVMArrayType(ptr, count), name);
    LLVMSetInitializer(result, LLVMConstArray(ptr, functions, count));
    LLVMSetGlobalConstant(result, 1);
    LLVMSetLinkage(result, LLVMInternalLinkage);
    return result;
}
static bool descriptor(LLVMModuleRef mod, LLVMTargetDataRef layout, const SnxCtrImage* image,
                        LLVMValueRef arm, LLVMValueRef thumb) {
    LLVMContextRef c = LLVMGetModuleContext(mod);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(c), i32 = LLVMInt32TypeInContext(c);
    LLVMTypeRef types[18];
    LLVMValueRef values[18], hash[32];
    types[0] = LLVMInt64TypeInContext(c);
    values[0] = LLVMConstInt(types[0], SNX_CTR_MODULE_MAGIC, 0);
    uint32_t integers[] = {SNX_CTR_MODULE_ABI, sizeof(SnxCtrModule), sizeof(SnxArmCPU), sizeof(SnxArmCheckedCPU),
        SNX_ARM_ABI, SNX_ARM_CHECKED_ABI, SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM11,
        SNX_ARM_MEMORY_CHECKED, sizeof(void*), image->segment[0].address, image->segment[0].bytes,
        image->segment[0].bytes / 4, image->segment[0].bytes / 2};
    const size_t offsets[] = {
        offsetof(SnxCtrModule, magic), offsetof(SnxCtrModule, abi), offsetof(SnxCtrModule, descriptor_size),
        offsetof(SnxCtrModule, cpu_size), offsetof(SnxCtrModule, checked_size), offsetof(SnxCtrModule, arm_abi),
        offsetof(SnxCtrModule, checked_abi), offsetof(SnxCtrModule, codegen_abi), offsetof(SnxCtrModule, profile),
        offsetof(SnxCtrModule, memory_mode), offsetof(SnxCtrModule, pointer_size), offsetof(SnxCtrModule, text_address),
        offsetof(SnxCtrModule, text_bytes), offsetof(SnxCtrModule, arm_count), offsetof(SnxCtrModule, thumb_count),
        offsetof(SnxCtrModule, image_identity), offsetof(SnxCtrModule, arm_blocks), offsetof(SnxCtrModule, thumb_blocks)};
    for (unsigned i = 0; i < 14; ++i) { types[i + 1] = i32; values[i + 1] = LLVMConstInt(i32, integers[i], 0); }
    types[15] = LLVMArrayType(i8, 32);
    for (unsigned i = 0; i < 32; ++i) hash[i] = LLVMConstInt(i8, image->identity[i], 0);
    values[15] = LLVMConstArray(i8, hash, 32);
    types[16] = types[17] = LLVMPointerTypeInContext(c, 0);
    values[16] = arm; values[17] = thumb;
    LLVMTypeRef structure = LLVMStructTypeInContext(c, types, 18, 0);
    if (LLVMABISizeOfType(layout, structure) != sizeof(SnxCtrModule)) return false;
    for (unsigned i = 0; i < 18; ++i) if (LLVMOffsetOfElement(layout, structure, i) != offsets[i]) return false;
    LLVMValueRef global = LLVMAddGlobal(mod, structure, "snx_ctr_module_v1");
    LLVMSetInitializer(global, LLVMConstNamedStruct(structure, values, 18));
    LLVMSetGlobalConstant(global, 1);
    return true;
}
static bool emit(const SnxCtrImage* image, char* output, char* error, size_t size) {
    LLVMContextRef ctx = NULL;
    LLVMModuleRef mod = NULL;
    LLVMTargetMachineRef machine = NULL;
    LLVMTargetDataRef layout = NULL;
    LLVMTargetRef target;
    char *triple = NULL, *detail = NULL;
    bool ok = false;
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) goto done;
    ctx = LLVMContextCreate();
    mod = LLVMModuleCreateWithNameInContext("ctr-static-aot-v1", ctx);
    triple = LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(triple, &target, &detail)) goto done;
    machine = LLVMCreateTargetMachine(target, triple, "generic", "", LLVMCodeGenLevelDefault,
                                      LLVMRelocPIC, LLVMCodeModelDefault);
    if (!machine) goto done;
    layout = LLVMCreateTargetDataLayout(machine);
    LLVMSetTarget(mod, triple); LLVMSetModuleDataLayout(mod, layout);
    SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM11, SNX_ARM_MEMORY_CHECKED};
    LLVMValueRef functions[SNX_CTR_AOT_TEXT_LIMIT / 2], tables[2];
    for (unsigned mode = 0; mode < 2; ++mode) {
        unsigned width = mode ? 2 : 4, count = image->segment[0].bytes / width;
        for (unsigned i = 0; i < count; ++i) {
            char name[40];
            snprintf(name, sizeof name, "ctr_%c_%u", mode ? 't' : 'a', i);
            uint32_t word = little(image->code + i * width, width);
            uint16_t half = (uint16_t)word;
            uint32_t pc = image->segment[0].address + i * width;
            size_t consumed = 0;
            functions[i] = mode ? snx_thumb_llvm_block_ex(mod, name, &half, 1, pc, &consumed, &options)
                                : snx_arm_llvm_block_ex(mod, name, &word, 1, pc, &consumed, &options);
            if (!functions[i] || consumed != 1) goto done;
            LLVMSetLinkage(functions[i], LLVMInternalLinkage);
        }
        tables[mode] = table(mod, mode ? "ctr_thumb" : "ctr_arm", functions, count);
    }
    if (!descriptor(mod, layout, image, tables[0], tables[1]) ||
        LLVMVerifyModule(mod, LLVMReturnStatusAction, &detail)) goto done;
    LLVMDisposeMessage(detail); detail = NULL;
    LLVMPassBuilderOptionsRef passes = LLVMCreatePassBuilderOptions();
    LLVMErrorRef failure = LLVMRunPasses(mod, "default<O2>", machine, passes);
    LLVMDisposePassBuilderOptions(passes);
    if (failure) {
        char* message = LLVMGetErrorMessage(failure);
        snprintf(error, size, "%s", message);
        LLVMDisposeErrorMessage(message);
        goto cleanup;
    }
    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &detail)) goto done;
    LLVMDisposeMessage(detail); detail = NULL;
    if (LLVMTargetMachineEmitToFile(machine, mod, output, LLVMObjectFile, &detail)) goto done;
    ok = true;
done:
    if (!ok) snprintf(error, size, "%s", detail ? detail : "cannot emit native ARM11 object / incompatible host layout");
cleanup:
    LLVMDisposeMessage(detail);
    if (layout) LLVMDisposeTargetData(layout);
    if (machine) LLVMDisposeTargetMachine(machine);
    LLVMDisposeMessage(triple);
    if (mod) LLVMDisposeModule(mod);
    if (ctx) LLVMContextDispose(ctx);
    return ok;
}
int main(int argc, char** argv) {
    char error[512] = {0};
    SnxCtrImage image = {0}, check = {0};
    char *directory = NULL, *target = NULL, *stage = NULL, *object = NULL, *module = NULL;
    int result = 1;
    if (argc != 2) { fputs("Usage: scarletnx-3ds-aot EXTRACTED_DIRECTORY\n", stderr); return 2; }
    if (!snx_ctr_image_load(argv[1], &image, error, sizeof error)) goto done;
    if (image.segment[0].bytes > SNX_CTR_AOT_TEXT_LIMIT) {
        snprintf(error, sizeof error, "initial static AOT text limit is %u bytes", SNX_CTR_AOT_TEXT_LIMIT);
        result = 3; goto done;
    }
    directory = snx_absolute(argv[1]);
    if (!directory) { snprintf(error, sizeof error, "invalid extracted path"); goto done; }
    target = snx_path_join(directory, "native");
    if (snx_path_exists(target)) { snprintf(error, sizeof error, "native output already exists; no cache reuse or overwrite"); goto done; }
    stage = snx_temp_dir(directory);
    if (!stage) { snprintf(error, sizeof error, "cannot create private native staging directory"); goto done; }
    object = snx_path_join(stage, "code.o");
    module = snx_path_join(stage, "code.so");
    if (!emit(&image, object, error, sizeof error)) goto done;
    const char* command[] = {SNX_CTR_HOST_CC, "-shared", "-Wl,-z,defs,-z,noexecstack,-z,relro,-z,now",
                             object, "-o", module, NULL};
    if (snx_run_stage(command, NULL, "native linking")) {
        snprintf(error, sizeof error, "native linker failed; no runnable module published"); goto done;
    }
    if (!snx_ctr_image_load(directory, &check, error, sizeof error) || memcmp(image.identity, check.identity, 32)) {
        snprintf(error, sizeof error, "extracted image changed during compilation"); goto done;
    }
    if (snx_path_exists(target) || snx_replace(stage, target)) {
        snprintf(error, sizeof error, "cannot publish new native output"); goto done;
    }
    free(stage); stage = NULL;
    printf("Compiled ARM11 static module: %u ARM / %u Thumb entries\n", image.segment[0].bytes / 4,
           image.segment[0].bytes / 2);
    result = 0;
done:
    if (result) fprintf(stderr, "scarletnx-3ds-aot: %s\n", error);
    if (stage) snx_remove_tree(stage);
    free(module); free(object); free(stage); free(target); free(directory);
    snx_ctr_image_free(&check); snx_ctr_image_free(&image);
    return result;
}
