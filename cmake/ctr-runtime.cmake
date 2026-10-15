# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Embed the project's PIC semantic-helper archive in the offline compiler.
# Installed tools need neither a build-tree path nor a separately located SDK archive.
file(READ "${INPUT}" data HEX)
string(REGEX REPLACE "(..)" "0x\\1," data "${data}")
file(WRITE "${OUTPUT}" "/* Generated from snx_arm_rt; do not edit. */\n#include \"aot/ctr_runtime.h\"\nconst unsigned char snx_ctr_aot_runtime[] = {${data}};\nconst size_t snx_ctr_aot_runtime_size = sizeof snx_ctr_aot_runtime;\n")
