/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "board/board.h"
#include "loader/ctr_loader.h"
#include "common.h"
#ifdef SNX_CTR_HAVE_AOT
#include "aot/ctr_module.h"
#endif
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE* out) {
    fputs("Usage: scarletnx-3ds [--model old|new] [-x] [--output NEW_DIRECTORY] [--] game.cia\n"
          "       scarletnx-3ds [--model old|new] --board-info\n"
          "Stage 2a extraction + experimental opt-in ARM11 static AOT.\n"
          "  -x, --extract   extract only (no guest execution)\n"
          "  --output DIR    new output directory; default work/3ds/<title>-<sha256>\n"
          "  --max-blocks N  execution budget, 1..10000000 (default 100000)\n"
          "ARM11 static compilation/execution and encrypted content are not implemented.\n",
          out);
}
static bool cia_path(const char* path) {
    size_t n = strlen(path);
    return n >= 4 && path[n - 4] == '.' && tolower((unsigned char)path[n - 3]) == 'c' &&
           tolower((unsigned char)path[n - 2]) == 'i' && tolower((unsigned char)path[n - 1]) == 'a';
}
static bool visible_path(const char* path) {
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}
int main(int argc, char** argv) {
    SnxCtrModel model = SNX_CTR_OLD;
    const char *input = NULL, *output = NULL;
    bool info = false, positional = false, extract_only = false, budget_set = false;
    uint64_t max_blocks = 100000;
    SnxCtrBoard board;
    snx_tool_console();
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!positional && !strcmp(arg, "--"))
            positional = true;
        else if (!positional && (!strcmp(arg, "--help") || !strcmp(arg, "-h"))) {
            usage(stdout);
            return 0;
        } else if (!positional && !strcmp(arg, "--board-info"))
            info = true;
        else if (!positional && (!strcmp(arg, "-x") || !strcmp(arg, "--extract")))
            extract_only = true;
        else if (!positional && !strcmp(arg, "--output")) {
            if (++i == argc || !*argv[i]) {
                fputs("scarletnx-3ds: --output requires a directory\n", stderr);
                return 2;
            }
            output = argv[i];
        } else if (!positional && !strcmp(arg, "--max-blocks")) {
            char* end = NULL;
            if (++i == argc || argv[i][0] < '0' || argv[i][0] > '9') {
                fputs("scarletnx-3ds: --max-blocks requires 1..10000000\n", stderr); return 2;
            }
            errno = 0;
            unsigned long long n = strtoull(argv[i], &end, 10);
            if (errno || *end || !n || n > 10000000) {
                fputs("scarletnx-3ds: --max-blocks requires 1..10000000\n", stderr); return 2;
            }
            max_blocks = (uint64_t)n; budget_set = true;
        } else if (!positional && !strcmp(arg, "--model")) {
            if (++i == argc || (strcmp(argv[i], "old") && strcmp(argv[i], "new"))) {
                fputs("scarletnx-3ds: --model requires old or new\n", stderr);
                return 2;
            }
            model = !strcmp(argv[i], "old") ? SNX_CTR_OLD : SNX_CTR_NEW;
        } else if ((!positional && arg[0] == '-') || input) {
            fprintf(stderr, "scarletnx-3ds: unexpected argument: %s\n", arg);
            usage(stderr);
            return 2;
        } else
            input = arg;
    }
    if ((!input && !info) || (info && (input || extract_only || output || budget_set)) || (extract_only && budget_set)) {
        usage(stderr);
        return 2;
    }
    if (!snx_ctr_board_init(&board, model)) return 1;
    if (info) {
        printf("3DS board: %s\nARM11 cores: %u\nBase clock: %" PRIu64 " Hz\nFCRAM: %u MiB\n", board.profile->name,
               board.profile->arm11_cores, SNX_CTR_BASE_CLOCK_HZ, board.profile->fcram_size / (1024u * 1024u));
        for (size_t i = 0; i < board.profile->region_count; ++i) {
            const SnxCtrRegion* r = &board.profile->regions[i];
            printf("  %-10s physical 0x%08" PRIx32 " size 0x%08" PRIx32 "\n", r->name, r->base, r->size);
        }
        snx_ctr_board_shutdown(&board);
        return 0;
    }
    snx_ctr_board_shutdown(&board);
    /* Do not let a host filename inject progress/launch banners into the GUI. */
    if (!visible_path(input) || (output && !visible_path(output))) {
        fputs("scarletnx-3ds: input/output paths may not contain control characters\n", stderr);
        return 2;
    }
    if (!cia_path(input) || !snx_is_file(input)) {
        fputs("scarletnx-3ds: expected a readable .cia file\n", stderr);
        return 2;
    }
    char error[512], *published = NULL;
    printf("Extracting %s\n", input);
    SnxCtrExtractResult result = snx_ctr_extract(input, output, &published, error, sizeof error);
    if (result != SNX_CTR_EXTRACT_OK) {
        fprintf(stderr, "scarletnx-3ds: %s\n", error);
        return result == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
    }
    printf("Extracted %s\n", published);
    if (extract_only) { free(published); return 0; }
#ifdef SNX_CTR_HAVE_AOT
    int status = snx_ctr_aot_run(argv[0], published, model, max_blocks, error, sizeof error);
    free(published);
    if (status) fprintf(stderr, "scarletnx-3ds: %s\n", error);
    return status;
#else
    (void)max_blocks;
    free(published);
    /* Never silently substitute NX's A64 compiler or A32's MCJIT runner. */
    fputs("scarletnx-3ds: extraction complete; ARM11 static compilation/execution is not implemented in this build "
          "(enable SNX_3DS_AOT).\n", stderr);
    return 3;
#endif
}
