/*
 * Copyright (C) 2018 Ryan Houdek <Sonicadvance1@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __ir_defines_h__
#define __ir_defines_h__
#include "main/imports.h"
#include "compiler/nir/nir_builder.h"
#include "util/half_float.h"
#include "util/register_allocate.h"
#include "util/u_dynarray.h"
#include "util/list.h"
#include "main/mtypes.h"

#include "bifrost.h"
#include "bifrost_compile.h"

struct ssa_args {
        uint32_t dest;
        uint32_t src0, src1, src2;
};

enum bifrost_op_type {
        TAG_LOAD_STORE_UBO_1,
        TAG_LOAD_STORE_UBO_2,
        TAG_LOAD_STORE_UBO_3,
        TAG_LOAD_STORE_UBO_4,
        TAG_FMA_OP,
};

/**
 * @brief Singular unpacked instruction that lives outside of the clause bundle
 */
struct bifrost_instruction {
        // Must be first
        struct list_head link;

        unsigned type;

        /**
         * @brief Pre-RA arguments
         */
        struct ssa_args args;

        bool scheduled;

        // Unionize all the hardware encoded instructions
        union {
                struct bifrost_fma_inst fma;
                struct bifrost_add_inst add;
        };
};

/**
 * @brief Combination of bifrost instructions that fit within a clause bundle
 */
struct bifrost_clause {
        // Must be first
        struct list_head link;

        struct bifrost_header header;

        uint32_t instruction_count;
        /**
         * @brief List of bifrost_instructions for the clause
         */
        struct util_dynarray instructions;
};

struct bifrost_block {
        /* Link to next block. Must be first for mir_get_block */
        struct list_head link;

        /* List of bifrost_instructions emitted for the current block */
        struct list_head instructions;

        /* List of bifrost_clause emitted (after the scheduler has run) */
        struct util_dynarray clauses;

        struct bifrost_block *next_fallthrough;
};

struct compiler_context {
        nir_shader *nir;
        gl_shader_stage stage;

        /* Current NIR function */
        nir_function *func;

        /* Unordered list of bifrost_blocks */
        uint32_t block_count;
        struct list_head blocks;

        struct bifrost_block *initial_block;
        struct bifrost_block *previous_source_block;
        struct bifrost_block *final_block;

        /* List of bifrost_instructions emitted for the current block */
        struct bifrost_block *current_block;

        /* Constants which have been loaded, for later inlining */
        struct hash_table_u64 *ssa_constants;

        /* Actual SSA-to-register for RA */
        struct hash_table_u64 *ssa_to_register;

        /* Mapping of hashes computed from NIR indices to the sequential temp indices ultimately used in MIR */
        struct hash_table_u64 *hash_to_temp;
        uint32_t temp_count;
        uint32_t max_hash;

        /* Uniform IDs for mdg */
        struct hash_table_u64 *uniform_nir_to_bi;
        uint32_t uniform_count;

        struct hash_table_u64 *varying_nir_to_bi;
        uint32_t varying_count;

        /* Count of instructions emitted from NIR overall, across all blocks */
        uint32_t instruction_count;
};

#define mir_foreach_block(ctx, v) list_for_each_entry(struct bifrost_block, v, &ctx->blocks, link)
#define mir_foreach_block_from(ctx, from, v) list_for_each_entry_from(struct bifrost_block, v, from, &ctx->blocks, link)
#define mir_foreach_instr(ctx, v) list_for_each_entry(struct bifrost_instruction, v, &ctx->current_block->instructions, link)
#define mir_foreach_instr_in_block(block, v) list_for_each_entry(struct bifrost_instruction, v, &block->instructions, link)

#define SSA_FIXED_MINIMUM (1 << 24)

#endif
