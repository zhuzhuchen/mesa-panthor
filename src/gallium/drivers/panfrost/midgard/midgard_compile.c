/*
 * Copyright (C) 2018 Alyssa Rosenzweig <alyssa@rosenzweig.io>
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>

#include "compiler/glsl/glsl_to_nir.h"
#include "compiler/nir_types.h"
#include "main/imports.h"
#include "compiler/nir/nir_builder.h"
#include "util/half_float.h"
#include "util/register_allocate.h"
#include "util/u_dynarray.h"
#include "util/list.h"
#include "main/mtypes.h"

#include "midgard.h"
#include "midgard_nir.h"
#include "midgard_compile.h"
#include "helpers.h"

#include "disassemble.h"

#define NIR_DEBUG
//#define NIR_DEBUG_FINE
//#define MIR_DEBUG
#define MDG_DEBUG

/* Instruction arguments represented as block-local SSA indices, rather than
 * registers. Negative values mean unused. */

typedef struct {
        int src0;
        int src1;
        int dest;

        /* src1 is -not- SSA but instead a 16-bit inline constant to be smudged
         * in. Only valid for ALU ops. */
        bool inline_constant;
} ssa_args;

/* Forward declare so midgard_branch can reference */
struct midgard_block;

/* Target types. Defaults to TARGET_GOTO (the type corresponding directly to
 * the hardware), hence why that must be zero */

#define TARGET_GOTO 0
#define TARGET_BREAK 1
#define TARGET_CONTINUE 2

typedef struct midgard_branch {
        /* If conditional, the condition is specified in r31.w */
        bool conditional;

        /* For conditionals, if this is true, we branch on FALSE. If false, we  branch on TRUE. */
        bool invert_conditional;

        /* Branch targets: the start of a block, the start of a loop (continue), the end of a loop (break). Value is one of TARGET_ */
        unsigned target_type;

        /* The actual target */
        union {
                int target_block;
                int target_break;
                int target_continue;
        };
} midgard_branch;

/* Generic in-memory data type repesenting a single logical instruction, rather
 * than a single instruction group. This is the preferred form for code gen.
 * Multiple midgard_insturctions will later be combined during scheduling,
 * though this is not represented in this structure.  Its format bridges
 * the low-level binary representation with the higher level semantic meaning.
 *
 * Notably, it allows registers to be specified as block local SSA, for code
 * emitted before the register allocation pass.
 */

typedef struct midgard_instruction {
        /* Must be first for casting */
        struct list_head link;

        unsigned type; /* ALU, load/store, texture */

        /* If the register allocator has not run yet... */
        ssa_args ssa_args;

        /* Special fields for an ALU instruction */
        midgard_reg_info registers;

        /* I.e. (1 << alu_bit) */
        int unit;

        bool has_constants;
        float constants[4];
        uint16_t inline_constant;
        bool has_blend_constant;

        bool compact_branch;
        bool writeout;
        bool prepacked_branch;

        union {
                midgard_load_store_word load_store;
                midgard_vector_alu alu;
                midgard_texture_word texture;
                uint16_t br_compact;

                /* General branch, rather than packed br_compact. Higher level
                 * than the other components */
                midgard_branch branch;
        };
} midgard_instruction;

typedef struct midgard_block {
        /* Link to next block. Must be first for mir_get_block */
        struct list_head link;

        /* List of midgard_instructions emitted for the current block */
        struct list_head instructions;

        bool is_scheduled;

        /* List of midgard_bundles emitted (after the scheduler has run) */
        struct util_dynarray bundles;

        /* Number of quadwords _actually_ emitted, as determined after scheduling */
        unsigned quadword_count;

        struct midgard_block *next_fallthrough;
} midgard_block;

/* Helpers to generate midgard_instruction's using macro magic, since every
 * driver seems to do it that way */

#define EMIT(op, ...) emit_mir_instruction(ctx, v_##op(__VA_ARGS__));

#define M_LOAD_STORE(name, rname, uname) \
	static midgard_instruction m_##name(unsigned ssa, unsigned address) { \
		midgard_instruction i = { \
			.type = TAG_LOAD_STORE_4, \
			.ssa_args = { \
				.rname = ssa, \
				.uname = -1, \
				.src1 = -1 \
			}, \
			.load_store = { \
				.op = midgard_op_##name, \
				.mask = 0xF, \
				.swizzle = SWIZZLE(COMPONENT_X, COMPONENT_Y, COMPONENT_Z, COMPONENT_W), \
				.address = address \
			} \
		}; \
		\
		return i; \
	}

#define M_LOAD(name) M_LOAD_STORE(name, dest, src0)
#define M_STORE(name) M_LOAD_STORE(name, src0, dest)

const midgard_vector_alu_src blank_alu_src = {
        .swizzle = SWIZZLE(COMPONENT_X, COMPONENT_Y, COMPONENT_Z, COMPONENT_W),
};

const midgard_scalar_alu_src blank_scalar_alu_src = {
        .full = true
};

/* Used for encoding the unused source of 1-op instructions */
const midgard_vector_alu_src zero_alu_src = { 0 };

/* Coerce structs to integer */

static unsigned
vector_alu_srco_unsigned(midgard_vector_alu_src src)
{
        unsigned u;
        memcpy(&u, &src, sizeof(src));
        return u;
}

/* Inputs a NIR ALU source, with modifiers attached if necessary, and outputs
 * the corresponding Midgard source */

static midgard_vector_alu_src
vector_alu_modifiers(nir_alu_src *src)
{
        if (!src) return blank_alu_src;

        midgard_vector_alu_src alu_src = {
                .abs = src->abs,
                .negate = src->negate,
                .rep_low = 0,
                .rep_high = 0,
                .half = 0, /* TODO */
                .swizzle = SWIZZLE_FROM_ARRAY(src->swizzle)
        };

        return alu_src;
}

/* 'Intrinsic' move for misc aliasing uses independent of actual NIR ALU code */

static midgard_instruction
v_fmov(unsigned src, midgard_vector_alu_src mod, unsigned dest)
{
        midgard_instruction ins = {
                .type = TAG_ALU_4,
                .ssa_args = {
                        .src0 = SSA_UNUSED_1,
                        .src1 = src,
                        .dest = dest,
                },
                .alu = {
                        .op = midgard_alu_op_fmov,
                        .reg_mode = midgard_reg_mode_full,
                        .dest_override = midgard_dest_override_none,
                        .mask = 0xFF,
                        .src1 = vector_alu_srco_unsigned(zero_alu_src),
                        .src2 = vector_alu_srco_unsigned(mod)
                },
        };

        return ins;
}

/* load/store instructions have both 32-bit and 16-bit variants, depending on
 * whether we are using vectors composed of highp or mediump. At the moment, we
 * don't support half-floats -- this requires changes in other parts of the
 * compiler -- therefore the 16-bit versions are commented out. */

//M_LOAD(load_attr_16);
M_LOAD(load_attr_32);
//M_LOAD(load_vary_16);
M_LOAD(load_vary_32);
//M_LOAD(load_uniform_16);
M_LOAD(load_uniform_32);
M_LOAD(load_color_buffer_8);
//M_STORE(store_vary_16);
M_STORE(store_vary_32);

static midgard_instruction
v_alu_br_compact_cond(midgard_jmp_writeout_op op, unsigned tag, signed offset, unsigned cond)
{
        midgard_branch_cond branch = {
                .op = op,
                .dest_tag = tag,
                .offset = offset,
                .cond = cond
        };

        uint16_t compact;
        memcpy(&compact, &branch, sizeof(branch));

        midgard_instruction ins = {
                .type = TAG_ALU_4,
                .unit = ALU_ENAB_BR_COMPACT,
                .prepacked_branch = true,
                .compact_branch = true,
                .br_compact = compact
        };

        if (op == midgard_jmp_writeout_op_writeout)
                ins.writeout = true;

        return ins;
}

static midgard_instruction
v_branch(bool conditional, bool invert)
{
        midgard_instruction ins = {
                .type = TAG_ALU_4,
                .unit = ALU_ENAB_BR_COMPACT,
                .compact_branch = true,
                .branch = {
                        .conditional = conditional,
                        .invert_conditional = invert
                }
        };

        return ins;
}

typedef struct midgard_bundle {
        /* Tag for the overall bundle */
        int tag;

        /* Instructions contained by the bundle */
        int instruction_count;
        midgard_instruction instructions[5];

        /* Bundle-wide ALU configuration */
        int padding;
        int control;
        bool has_embedded_constants;
        float constants[4];
        bool has_blend_constant;

        uint16_t register_words[8];
        int register_words_count;

        uint64_t body_words[8];
        size_t body_size[8];
        int body_words_count;
} midgard_bundle;

typedef struct compiler_context {
        nir_shader *nir;
        gl_shader_stage stage;

        /* Is internally a blend shader? Depends on stage == FRAGMENT */
        bool is_blend;

        /* Tracking for blend constant patching */
        int blend_constant_number;
        int blend_constant_offset;

        /* Current NIR function */
        nir_function *func;

        /* Unordered list of midgard_blocks */
        int block_count;
        struct list_head blocks;

        midgard_block *initial_block;
        midgard_block *previous_source_block;
        midgard_block *final_block;

        /* List of midgard_instructions emitted for the current block */
        midgard_block *current_block;

        /* The index corresponding to the current loop, e.g. for breaks/contineus */
        int current_loop;

        /* Constants which have been loaded, for later inlining */
        struct hash_table_u64 *ssa_constants;

        /* SSA indices to be outputted to corresponding varying offset */
        struct hash_table_u64 *ssa_varyings;

        /* SSA values / registers which have been aliased. Naively, these
         * demand a fmov output; instead, we alias them in a later pass to
         * avoid the wasted op.
         *
         * A note on encoding: to avoid dynamic memory management here, rather
         * than ampping to a pointer, we map to the source index; the key
         * itself is just the destination index. */

        struct hash_table_u64 *ssa_to_alias;
        struct set *leftover_ssa_to_alias;

        /* Actual SSA-to-register for RA */
        struct hash_table_u64 *ssa_to_register;

        /* Mapping of hashes computed from NIR indices to the sequential temp indices ultimately used in MIR */
        struct hash_table_u64 *hash_to_temp;
        int temp_count;
        int max_hash;

        /* Uniform IDs for mdg */
        struct hash_table_u64 *uniform_nir_to_mdg;
        int uniform_count;

        struct hash_table_u64 *varying_nir_to_mdg;
        int varying_count;

        /* Just the count of the max register used. Higher count => higher
         * register pressure */
        int work_registers;

        /* Used for cont/last hinting. Increase when a tex op is added.
         * Decrease when a tex op is removed. */
        int texture_op_count;

        /* Mapping of texture register -> SSA index for unaliasing */
        int texture_index[2];

        /* Count of special uniforms (viewport, etc) in vec4 units */
        int special_uniforms;

        /* If any path hits a discard instruction */
        bool can_discard;

        /* The number of uniforms allowable for the fast path */
        int uniform_cutoff;

        /* Count of instructions emitted from NIR overall, across all blocks */
        int instruction_count;
} compiler_context;

/* Append instruction to end of current block */

static midgard_instruction *
mir_upload_ins(struct midgard_instruction ins)
{
        midgard_instruction *heap = malloc(sizeof(ins));
        memcpy(heap, &ins, sizeof(ins));
        return heap;
}

static void
emit_mir_instruction(struct compiler_context *ctx, struct midgard_instruction ins)
{
        list_addtail(&(mir_upload_ins(ins))->link, &ctx->current_block->instructions);
}

static void
mir_insert_instruction_before(struct midgard_instruction *tag, struct midgard_instruction ins)
{
        list_addtail(&(mir_upload_ins(ins))->link, &tag->link);
}

static void
mir_remove_instruction(struct midgard_instruction *ins)
{
        list_del(&ins->link);
}

static midgard_instruction*
mir_prev_op(struct midgard_instruction *ins)
{
        return list_last_entry(&(ins->link), midgard_instruction, link);
}

static midgard_instruction*
mir_next_op(struct midgard_instruction *ins)
{
        return list_first_entry(&(ins->link), midgard_instruction, link);
}

static midgard_block *
mir_next_block(struct midgard_block *blk)
{
        return list_first_entry(&(blk->link), midgard_block, link);
}


#define mir_foreach_block(ctx, v) list_for_each_entry(struct midgard_block, v, &ctx->blocks, link) 
#define mir_foreach_block_from(ctx, from, v) list_for_each_entry_from(struct midgard_block, v, from, &ctx->blocks, link)

#define mir_foreach_instr(ctx, v) list_for_each_entry(struct midgard_instruction, v, &ctx->current_block->instructions, link) 
#define mir_foreach_instr_safe(ctx, v) list_for_each_entry_safe(struct midgard_instruction, v, &ctx->current_block->instructions, link) 
#define mir_foreach_instr_in_block(block, v) list_for_each_entry(struct midgard_instruction, v, &block->instructions, link) 
#define mir_foreach_instr_in_block_safe(block, v) list_for_each_entry_safe(struct midgard_instruction, v, &block->instructions, link) 
#define mir_foreach_instr_in_block_safe_rev(block, v) list_for_each_entry_safe_rev(struct midgard_instruction, v, &block->instructions, link) 
#define mir_foreach_instr_in_block_from(block, v, from) list_for_each_entry_from(struct midgard_instruction, v, from, &block->instructions, link) 


static midgard_instruction *
mir_last_in_block(struct midgard_block *block)
{
        return list_last_entry(&block->instructions, struct midgard_instruction, link);
}

static midgard_block *
mir_get_block(compiler_context *ctx, int idx)
{
        struct list_head *lst = &ctx->blocks;

        while ((idx--) + 1)
                lst = lst->next;

        return (struct midgard_block *) lst;
}

/* Pretty printer for internal Midgard IR */

static void
print_mir_source(int source)
{
        if (source >= SSA_FIXED_MINIMUM) {
                /* Specific register */
                int reg = SSA_REG_FROM_FIXED(source);

                /* TODO: Moving threshold */
                if (reg > 16 && reg < 24)
                        printf("u%d", 23 - reg);
                else
                        printf("r%d", reg);
        } else {
                printf("%d", source);
        }
}

static void
print_mir_instruction(midgard_instruction *ins)
{
        printf("\t");

        switch (ins->type) {
        case TAG_ALU_4: {
                midgard_alu_op op = ins->alu.op;
                const char *name = alu_opcode_names[op];

                if (ins->unit)
                        printf("%d.", ins->unit);

                printf("%s", name ? name : "??");
                break;
        }

        case TAG_LOAD_STORE_4: {
                midgard_load_store_op op = ins->load_store.op;
                const char *name = load_store_opcode_names[op];

                assert(name);
                printf("%s", name);
                break;
        }

        case TAG_TEXTURE_4: {
                printf("texture");
                break;
        }

        default:
                assert(0);
        }

        ssa_args *args = &ins->ssa_args;

        printf(" %d, ", args->dest);

        print_mir_source(args->src0);
        printf(", ");

        if (args->inline_constant)
                printf("#%d", ins->inline_constant);
        else
                print_mir_source(args->src1);

        if (ins->has_constants)
                printf(" <%f, %f, %f, %f>", ins->constants[0], ins->constants[1], ins->constants[2], ins->constants[3]);

        printf("\n");
}

static void
print_mir_block(midgard_block *block)
{
        printf("{\n");

        mir_foreach_instr_in_block(block, ins) {
                print_mir_instruction(ins);
        }

        printf("}\n");
}



static void
attach_constants(compiler_context *ctx, midgard_instruction *ins, void *constants, int name)
{
        ins->has_constants = true;
        memcpy(&ins->constants, constants, 16);

        /* If this is the special blend constant, mark this instruction */

        if (ctx->is_blend && ctx->blend_constant_number == name)
                ins->has_blend_constant = true;
}

static int
glsl_type_size(const struct glsl_type *type)
{
        return glsl_count_attribute_slots(type, false);
}

/* Lower fdot2 to a vector multiplication followed by channel addition  */
static void
midgard_nir_lower_fdot2_body(nir_builder *b, nir_alu_instr *alu)
{
        if (alu->op != nir_op_fdot2)
                return;

        b->cursor = nir_before_instr(&alu->instr);

        nir_ssa_def *src0 = nir_ssa_for_alu_src(b, alu, 0);
        nir_ssa_def *src1 = nir_ssa_for_alu_src(b, alu, 1);

        nir_ssa_def *product = nir_fmul(b, src0, src1);

        nir_ssa_def *sum = nir_fadd(b, 
                        nir_channel(b, product, 0), 
                        nir_channel(b, product, 1));

        /* Replace the fdot2 with this sum */
        nir_ssa_def_rewrite_uses(&alu->dest.dest.ssa, nir_src_for_ssa(sum));
}

static bool
midgard_nir_lower_fdot2(nir_shader *shader)
{
        bool progress = false;

        nir_foreach_function(function, shader) {
                if (!function->impl) continue;

                nir_builder _b;
                nir_builder *b = &_b;
                nir_builder_init(b, function->impl);

                nir_foreach_block(block, function->impl) {
                        nir_foreach_instr_safe(instr, block) {
                                if (instr->type != nir_instr_type_alu) continue;

                                nir_alu_instr *alu = nir_instr_as_alu(instr);
                                midgard_nir_lower_fdot2_body(b, alu);

                                progress |= true;
                        }
                }

                nir_metadata_preserve(function->impl, nir_metadata_block_index | nir_metadata_dominance);

        }

        return progress;
}

static void
optimise_nir(nir_shader *nir)
{
        bool progress;

        NIR_PASS(progress, nir, nir_lower_regs_to_ssa);
        NIR_PASS(progress, nir, midgard_nir_lower_fdot2);

        nir_lower_tex_options lower_tex_options = {
                .lower_rect = true
        };

        NIR_PASS(progress, nir, nir_lower_tex, &lower_tex_options);

        do {
                progress = false;

                NIR_PASS(progress, nir, midgard_nir_lower_algebraic);
                NIR_PASS(progress, nir, nir_lower_io, nir_var_all, glsl_type_size, 0);
                NIR_PASS(progress, nir, nir_lower_var_copies);
                NIR_PASS(progress, nir, nir_lower_vars_to_ssa);

                NIR_PASS(progress, nir, nir_copy_prop);
                NIR_PASS(progress, nir, nir_opt_dce);
                NIR_PASS(progress, nir, nir_opt_vectorize);
                NIR_PASS(progress, nir, nir_opt_dead_cf);
                NIR_PASS(progress, nir, nir_opt_cse);
                NIR_PASS(progress, nir, nir_opt_peephole_select, 64, false, true);
                NIR_PASS(progress, nir, nir_opt_algebraic);
                NIR_PASS(progress, nir, nir_opt_constant_folding);
                NIR_PASS(progress, nir, nir_opt_undef);
                NIR_PASS(progress, nir, nir_opt_loop_unroll,
                         nir_var_shader_in |
                         nir_var_shader_out |
                         nir_var_local);
        } while (progress);

        /* Must be run at the end to prevent creation of fsin/fcos ops */
        NIR_PASS(progress, nir, midgard_nir_scale_trig);

        do {
                progress = false;

                NIR_PASS(progress, nir, nir_opt_dce);
                NIR_PASS(progress, nir, nir_opt_algebraic);
                NIR_PASS(progress, nir, nir_opt_constant_folding);
                NIR_PASS(progress, nir, nir_copy_prop);
        } while (progress);

        NIR_PASS(progress, nir, nir_opt_algebraic_late);

        /* Lower mods */
        NIR_PASS(progress, nir, nir_lower_to_source_mods, nir_lower_all_source_mods);
        NIR_PASS(progress, nir, nir_copy_prop);
        NIR_PASS(progress, nir, nir_opt_dce);

        /* Take us out of SSA */
        NIR_PASS(progress, nir, nir_lower_locals_to_regs);
        NIR_PASS(progress, nir, nir_convert_from_ssa, true);

        /* We are a vector architecture; write combine where possible */
        NIR_PASS(progress, nir, nir_move_vec_src_uses_to_dest);
        NIR_PASS(progress, nir, nir_lower_vec_to_movs);

        NIR_PASS(progress, nir, nir_opt_dce);
}

/* Front-half of aliasing the SSA slots, merely by inserting the flag in the
 * appropriate hash table. Intentional off-by-one to avoid confusing NULL with
 * r0. See the comments in compiler_context */

static void
alias_ssa(compiler_context *ctx, int dest, int src)
{
        _mesa_hash_table_u64_insert(ctx->ssa_to_alias, dest + 1, (void *) ((uintptr_t) src + 1));
        _mesa_set_add(ctx->leftover_ssa_to_alias, (void *) (uintptr_t) (dest + 1));
}

/* ...or undo it, after which the original index will be used (dummy move should be emitted alongside this) */

static void
unalias_ssa(compiler_context *ctx, int dest)
{
        _mesa_hash_table_u64_remove(ctx->ssa_to_alias, dest + 1);
        /* TODO: Remove from leftover or no? */
}

static void
midgard_pin_output(compiler_context *ctx, int index, int reg)
{
        _mesa_hash_table_u64_insert(ctx->ssa_to_register, index + 1, (void *) ((uintptr_t) reg + 1));
}

static bool
midgard_is_pinned(compiler_context *ctx, int index)
{
        return _mesa_hash_table_u64_search(ctx->ssa_to_register, index + 1) != NULL;
}

/* Do not actually emit a load; instead, cache the constant for inlining */

static void
emit_load_const(compiler_context *ctx, nir_load_const_instr *instr)
{
        nir_ssa_def def = instr->def;

        float *v = ralloc_array(NULL, float, 4);
        memcpy(v, &instr->value.f32, 4 * sizeof(float));
        _mesa_hash_table_u64_insert(ctx->ssa_constants, def.index + 1, v);
}

/* Duplicate bits to convert sane 4-bit writemask to obscure 8-bit format (or
 * do the inverse) */

static unsigned
expand_writemask(unsigned mask)
{
        unsigned o = 0;

        for (int i = 0; i < 4; ++i)
                if (mask & (1 << i))
                        o |= (3 << (2 * i));

        return o;
}

static unsigned
squeeze_writemask(unsigned mask)
{
        unsigned o = 0;

        for (int i = 0; i < 4; ++i)
                if (mask & (3 << (2 * i)))
                        o |= (1 << i);

        return o;

}

/* Determines effective writemask, taking quirks and expansion into account */
static unsigned
effective_writemask(midgard_vector_alu *alu)
{
        /* Channel count is off-by-one to fit in two-bits (0 channel makes no
         * sense) */

        unsigned channel_count = GET_CHANNEL_COUNT(alu_opcode_props[alu->op]);

        /* If there is a fixed channel count, construct the appropriate mask */

        if (channel_count)
                return (1 << channel_count) - 1;

        /* Otherwise, just squeeze the existing mask */
        return squeeze_writemask(alu->mask);
}

static unsigned
find_or_allocate_temp(compiler_context *ctx, unsigned hash)
{
        if ((hash < 0) || (hash >= SSA_FIXED_MINIMUM))
                return hash;

        unsigned temp = (uintptr_t) _mesa_hash_table_u64_search(ctx->hash_to_temp, hash + 1);

        if (temp)
                return temp - 1;

        /* If no temp is find, allocate one */
        temp = ctx->temp_count++;
        ctx->max_hash = MAX2(ctx->max_hash, hash);

        _mesa_hash_table_u64_insert(ctx->hash_to_temp, hash + 1, (void *) ((uintptr_t) temp + 1));

        return temp;
}

static unsigned
nir_src_index(nir_src *src)
{
        if (src->is_ssa)
                return src->ssa->index;
        else
                return 4096 + src->reg.reg->index;
}

static unsigned
nir_dest_index(nir_dest *dst)
{
        if (dst->is_ssa)
                return dst->ssa.index;
        else
                return 4096 + dst->reg.reg->index;
}

static unsigned
nir_alu_src_index(nir_alu_src *src)
{
        return nir_src_index(&src->src);
}

/* Midgard puts conditionals in r31.w; move an arbitrary source (the output of
 * a conditional test) into that register */

static void
emit_condition(compiler_context *ctx, nir_src *src, bool for_branch)
{
        /* XXX: Force component correct */
        int condition = nir_src_index(src);

        const midgard_vector_alu_src alu_src = {
                .swizzle = SWIZZLE(COMPONENT_X, COMPONENT_X, COMPONENT_X, COMPONENT_X),
        };

        /* There is no boolean move instruction. Instead, we simulate a move by
         * ANDing the condition with itself to get it into r31.w */

        midgard_instruction ins = {
                .type = TAG_ALU_4,
                .unit = for_branch ? UNIT_SMUL : UNIT_SADD, /* TODO: DEDUCE THIS */
                .ssa_args = {
                        .src0 = condition,
                        .src1 = condition,
                        .dest = SSA_FIXED_REGISTER(31),
                },
                .alu = {
                        .op = midgard_alu_op_iand,
                        .reg_mode = midgard_reg_mode_full,
                        .dest_override = midgard_dest_override_none,
                        .mask = (0x3 << 6), /* w */
                        .src1 = vector_alu_srco_unsigned(alu_src),
                        .src2 = vector_alu_srco_unsigned(alu_src)
                },
        };

        emit_mir_instruction(ctx, ins);
}

/* Components: Number/style of arguments:
 * 	3: One-argument op with r24 (i2f, f2i)
 * 	2: Standard two argument op (fadd, fmul)
 * 	1: Flipped one-argument op (fmov, imov)
 * 	0: Standard one-argument op (frcp)
 * NIR: NIR instruction op.
 * Op: Midgard instruction op.
 */

#define ALU_CASE(_components, nir, _op) \
	case nir_op_##nir: \
		components = _components; \
		op = midgard_alu_op_##_op; \
		break;

static void
emit_alu(compiler_context *ctx, nir_alu_instr *instr)
{
        bool is_ssa = instr->dest.dest.is_ssa;

        unsigned dest = nir_dest_index(&instr->dest.dest);
        unsigned nr_components = is_ssa ? instr->dest.dest.ssa.num_components : instr->dest.dest.reg.reg->num_components;

        /* Most Midgard ALU ops have a 1:1 correspondance to NIR ops; these are
         * supported. A few do not and are commented for now. Also, there are a
         * number of NIR ops which Midgard does not support and need to be
         * lowered, also TODO. This switch block emits the opcode and calling
         * convention of the Midgard instruction; actual packing is done in
         * emit_alu below */

        unsigned op, components;

        switch (instr->op) {
                ALU_CASE(2, fadd, fadd);
                ALU_CASE(2, fmul, fmul);
                ALU_CASE(2, fmin, fmin);
                ALU_CASE(2, fmax, fmax);
                ALU_CASE(2, imin, imin);
                ALU_CASE(2, imax, imax);
                ALU_CASE(1, fmov, fmov);
                ALU_CASE(0, ffloor, ffloor);
                ALU_CASE(0, fceil, fceil);
                ALU_CASE(2, fdot3, fdot3);
                //ALU_CASE(2, fdot3r);
                ALU_CASE(2, fdot4, fdot4);
                //ALU_CASE(2, freduce);
                ALU_CASE(2, iadd, iadd);
                ALU_CASE(2, isub, isub);
                ALU_CASE(2, imul, imul);

                /* XXX: Use fmov, not imov, since imov was causing major
                 * issues with texture precision? XXX research */
                ALU_CASE(1, imov, fmov);

                ALU_CASE(2, feq, feq);
                ALU_CASE(2, fne, fne);
                ALU_CASE(2, flt, flt);
                ALU_CASE(2, ieq, ieq);
                ALU_CASE(2, ine, ine);
                ALU_CASE(2, ilt, ilt);
                //ALU_CASE(2, icsel, icsel);
                ALU_CASE(0, frcp, frcp);
                ALU_CASE(0, frsq, frsqrt);
                ALU_CASE(0, fsqrt, fsqrt);
                ALU_CASE(0, fexp2, fexp2);
                ALU_CASE(0, flog2, flog2);

                ALU_CASE(3, f2i32, f2i);
                ALU_CASE(3, f2u32, f2u);
                ALU_CASE(3, i2f32, i2f);
                ALU_CASE(3, u2f32, u2f);

                ALU_CASE(0, fsin, fsin);
                ALU_CASE(0, fcos, fcos);

                ALU_CASE(2, iand, iand);
                ALU_CASE(2, ior, ior);
                ALU_CASE(2, ixor, ixor);
                ALU_CASE(0, inot, inot);
                ALU_CASE(2, ishl, ishl);
                ALU_CASE(2, ishr, iasr);
                ALU_CASE(2, ushr, ilsr);
                //ALU_CASE(2, ilsr, ilsr);

                ALU_CASE(2, ball_fequal4, fball_eq);
                ALU_CASE(2, bany_fnequal4, fbany_neq);
                ALU_CASE(2, ball_iequal4, iball_eq);
                ALU_CASE(2, bany_inequal4, ibany_neq);

        /* For greater-or-equal, we use less-or-equal and flip the
         * arguments */

        case nir_op_ige: {
                components = 2;
                op = midgard_alu_op_ile;

                /* Swap via temporary */
                nir_alu_src temp = instr->src[1];
                instr->src[1] = instr->src[0];
                instr->src[0] = temp;

                break;
        }

        case nir_op_bcsel: {
                components = 2;
                op = midgard_alu_op_fcsel;

                emit_condition(ctx, &instr->src[0].src, false);

                /* The condition is the first argument; move the other
                 * arguments up one to be a binary instruction for
                 * Midgard */

                memmove(instr->src, instr->src + 1, 2 * sizeof(nir_alu_src));
                break;
        }

        default:
                printf("Unhandled ALU op %s\n", nir_op_infos[instr->op].name);
                assert(0);
                return;
        }

        int _unit = alu_opcode_props[op];

        /* Initialise fields common between scalar/vector instructions */
        midgard_outmod outmod = instr->dest.saturate ? midgard_outmod_sat : midgard_outmod_none;

        /* src0 will always exist afaik, but src1 will not for 1-argument
         * instructions. The latter can only be fetched if the instruction
         * needs it, or else we may segfault. */

        unsigned src0 = nir_alu_src_index(&instr->src[0]);
        unsigned src1 = components == 2 ? nir_alu_src_index(&instr->src[1]) : SSA_UNUSED_0;

        /* Rather than use the instruction generation helpers, we do it
         * ourselves here to avoid the mess */

        midgard_instruction ins = {
                .type = TAG_ALU_4,
                .ssa_args = {
                        .src0 = components == 3 || components == 2 || components == 0 ? src0 : SSA_UNUSED_1,
                        .src1 = components == 2 ? src1 : components == 1 ? src0 : components == 0 ? SSA_UNUSED_0 : SSA_UNUSED_1,
                        .dest = dest,
                        .inline_constant = components == 0
                }
        };

        nir_alu_src *nirmod0 = NULL;
        nir_alu_src *nirmod1 = NULL;

        if (components == 2) {
                nirmod0 = &instr->src[0];
                nirmod1 = &instr->src[1];
        } else if (components == 1) {
                nirmod1 = &instr->src[0];
        } else if (components == 0) {
                nirmod0 = &instr->src[0];
        }

        midgard_vector_alu alu = {
                .op = op,
                .reg_mode = midgard_reg_mode_full,
                .dest_override = midgard_dest_override_none,
                .outmod = outmod,

                /* Writemask only valid for non-SSA NIR */
                .mask = expand_writemask((1 << nr_components) - 1),

                .src1 = vector_alu_srco_unsigned(vector_alu_modifiers(nirmod0)),
                .src2 = vector_alu_srco_unsigned(vector_alu_modifiers(nirmod1)),
        };

        /* Apply writemask if non-SSA, keeping in mind that we can't write to components that don't exist */

        if (!is_ssa)
                alu.mask &= expand_writemask(instr->dest.write_mask);

        ins.alu = alu;

        if (_unit == UNIT_VLUT) {
                /* To avoid duplicating the LUTs (we think?), LUT instructions can only
                 * operate as if they were scalars. Lower them here by changing the
                 * component. */

                assert(components == 0);

                uint8_t original_swizzle[4];
                memcpy(original_swizzle, nirmod0->swizzle, sizeof(nirmod0->swizzle));

                for (int i = 0; i < nr_components; ++i) {
                        ins.alu.mask = (0x3) << (2 * i); /* Mask the associated component */

                        for (int j = 0; j < 4; ++j)
                                nirmod0->swizzle[j] = original_swizzle[i]; /* Pull from the correct component */

                        ins.alu.src1 = vector_alu_srco_unsigned(vector_alu_modifiers(nirmod0));
                        emit_mir_instruction(ctx, ins);
                }
        } else {
                emit_mir_instruction(ctx, ins);
        }
}

static void
emit_intrinsic(compiler_context *ctx, nir_intrinsic_instr *instr)
{
        nir_const_value *const_offset;
        unsigned offset, reg;

        switch (instr->intrinsic) {
        case nir_intrinsic_discard_if:
                emit_condition(ctx, &instr->src[0], true);

        /* fallthrough */

        case nir_intrinsic_discard: {
                midgard_condition cond = instr->intrinsic == nir_intrinsic_discard_if ?
                                         midgard_condition_true : midgard_condition_always;

                EMIT(alu_br_compact_cond, midgard_jmp_writeout_op_discard, 0, 2, cond);
                ctx->can_discard = true;
                break;
        }

        case nir_intrinsic_load_uniform:
        case nir_intrinsic_load_input:
                const_offset = nir_src_as_const_value(instr->src[0]);
                assert (const_offset && "no indirect inputs");

                offset = nir_intrinsic_base(instr) + const_offset->u32[0];

                reg = nir_dest_index(&instr->dest);

                if (instr->intrinsic == nir_intrinsic_load_uniform && !ctx->is_blend) {
                        /* TODO: half-floats */

                        int uniform_offset = 0;

                        if (offset >= SPECIAL_UNIFORM_BASE) {
                                /* XXX: Resolve which uniform */
                                uniform_offset = 0;
                        } else {
                                /* Offset away from the special
                                 * uniform block */

                                void *entry = _mesa_hash_table_u64_search(ctx->uniform_nir_to_mdg, offset + 1);

                                /* XXX */
                                if (!entry) {
                                        printf("WARNING: Unknown uniform %d\n", offset);
                                        break;
                                }

                                uniform_offset = (uintptr_t) (entry) - 1;
                                uniform_offset += ctx->special_uniforms;
                        }

                        if (uniform_offset < ctx->uniform_cutoff) {
                                /* Fast path: For the first 16 uniform,
                                 * accesses are 0-cycle, since they're
                                 * just a register fetch in the usual
                                 * case.  So, we alias the registers
                                 * while we're still in SSA-space */

                                int reg_slot = 23 - uniform_offset;
                                alias_ssa(ctx, reg, SSA_FIXED_REGISTER(reg_slot));
                        } else {
                                /* Otherwise, read from the 'special'
                                 * UBO to access higher-indexed
                                 * uniforms, at a performance cost */

                                midgard_instruction ins = m_load_uniform_32(reg, uniform_offset);

                                /* TODO: Don't split */
                                ins.load_store.varying_parameters = (uniform_offset & 7) << 7;
                                ins.load_store.address = uniform_offset >> 3;

                                ins.load_store.unknown = 0x1E00; /* xxx: what is this? */
                                emit_mir_instruction(ctx, ins);
                        }
                } else if (ctx->stage == MESA_SHADER_FRAGMENT && !ctx->is_blend) {
                        /* XXX: Half-floats? */
                        /* TODO: swizzle, mask */

                        midgard_instruction ins = m_load_vary_32(reg, offset);

                        midgard_varying_parameter p = {
                                .is_varying = 1,
                                .interpolation = midgard_interp_default,
                                .flat = /*var->data.interpolation == INTERP_MODE_FLAT*/ 0
                        };

                        unsigned u;
                        memcpy(&u, &p, sizeof(p));
                        ins.load_store.varying_parameters = u;

                        ins.load_store.unknown = 0x1e9e; /* xxx: what is this? */
                        emit_mir_instruction(ctx, ins);
                } else if (ctx->is_blend && instr->intrinsic == nir_intrinsic_load_uniform) {
                        /* Constant encoded as a pinned constant */

                        midgard_instruction ins = v_fmov(SSA_FIXED_REGISTER(REGISTER_CONSTANT), blank_alu_src, reg);
                        ins.has_constants = true;
                        ins.has_blend_constant = true;
                        emit_mir_instruction(ctx, ins);
                } else if (ctx->is_blend) {
                        /* For blend shaders, a load might be
                         * translated various ways depending on what
                         * we're loading. Figure out how this is used */

                        nir_variable *out = NULL;

                        nir_foreach_variable(var, &ctx->nir->inputs) {
                                int drvloc = var->data.driver_location;

                                if (nir_intrinsic_base(instr) == drvloc) {
                                        out = var;
                                        break;
                                }
                        }

                        assert(out);

                        if (out->data.location == VARYING_SLOT_COL0) {
                                /* Source color preloaded to r0 */

                                midgard_pin_output(ctx, reg, 0);
                        } else if (out->data.location == VARYING_SLOT_COL1) {
                                /* Destination color must be read from framebuffer */

                                midgard_instruction ins = m_load_color_buffer_8(reg, 0);
                                ins.load_store.swizzle = 0; /* xxxx */

                                /* Read each component sequentially */

                                for (int c = 0; c < 4; ++c) {
                                        ins.load_store.mask = (1 << c);
                                        ins.load_store.unknown = c;
                                        emit_mir_instruction(ctx, ins);
                                }

                                /* vadd.u2f hr2, abs(hr2), #0 */

                                midgard_vector_alu_src alu_src = blank_alu_src;
                                alu_src.abs = true;
                                alu_src.half = true;

                                midgard_instruction u2f = {
                                        .type = TAG_ALU_4,
                                        .ssa_args = {
                                                .src0 = reg,
                                                .src1 = SSA_UNUSED_0,
                                                .dest = reg,
                                                .inline_constant = true
                                        },
                                        .alu = {
                                                .op = midgard_alu_op_u2f,
                                                .reg_mode = midgard_reg_mode_half,
                                                .dest_override = midgard_dest_override_none,
                                                .mask = 0xF,
                                                .src1 = vector_alu_srco_unsigned(alu_src),
                                                .src2 = vector_alu_srco_unsigned(blank_alu_src),
                                        }
                                };

                                emit_mir_instruction(ctx, u2f);

                                /* vmul.fmul.sat r1, hr2, #0.00392151 */

                                alu_src.abs = false;

                                midgard_instruction fmul = {
                                        .type = TAG_ALU_4,
                                        .inline_constant = _mesa_float_to_half(1.0 / 255.0),
                                        .ssa_args = {
                                                .src0 = reg,
                                                .dest = reg,
                                                .src1 = SSA_UNUSED_0,
                                                .inline_constant = true
                                        },
                                        .alu = {
                                                .op = midgard_alu_op_fmul,
                                                .reg_mode = midgard_reg_mode_full,
                                                .dest_override = midgard_dest_override_none,
                                                .outmod = midgard_outmod_sat,
                                                .mask = 0xFF,
                                                .src1 = vector_alu_srco_unsigned(alu_src),
                                                .src2 = vector_alu_srco_unsigned(blank_alu_src),
                                        }
                                };

                                emit_mir_instruction(ctx, fmul);
                        } else {
                                printf("Unknown input in blend shader\n");
                                assert(0);
                        }
                } else if (ctx->stage == MESA_SHADER_VERTEX) {
                        midgard_instruction ins = m_load_attr_32(reg, offset);
                        ins.load_store.unknown = 0x1E1E; /* XXX: What is this? */
                        ins.load_store.mask = (1 << instr->num_components) - 1;
                        emit_mir_instruction(ctx, ins);
                } else {
                        printf("Unknown load\n");
                        assert(0);
                }

                break;

        case nir_intrinsic_store_output:
                const_offset = nir_src_as_const_value(instr->src[1]);
                assert(const_offset && "no indirect outputs");

                offset = nir_intrinsic_base(instr) + const_offset->u32[0];

                reg = nir_src_index(&instr->src[0]);

                if (ctx->stage == MESA_SHADER_FRAGMENT) {
                        /* gl_FragColor is not emitted with load/store
                         * instructions. Instead, it gets plonked into
                         * r0 at the end of the shader and we do the
                         * framebuffer writeout dance. TODO: Defer
                         * writes */

                        midgard_pin_output(ctx, reg, 0);
                } else if (ctx->stage == MESA_SHADER_VERTEX) {
                        /* Varyings are written into one of two special
                         * varying register, r26 or r27. The register itself is selected as the register
                         * in the st_vary instruction, minus the base of 26. E.g. write into r27 and then call st_vary(1)
                         *
                         * Normally emitting fmov's is frowned upon,
                         * but due to unique constraints of
                         * REGISTER_VARYING, fmov emission + a
                         * dedicated cleanup pass is the only way to
                         * guarantee correctness when considering some
                         * (common) edge cases XXX: FIXME */

                        /* Look up how it was actually laid out */

                        void *entry = _mesa_hash_table_u64_search(ctx->varying_nir_to_mdg, offset + 1);

                        if (!entry) {
                                printf("WARNING: skipping varying\n");
                                break;
                        }

                        offset = (uintptr_t) (entry) - 1;

                        /* Compute offset: gl_Position is zero. The
                         * first varying is two. The nth varying is 2+n
                         * for zero-indexed n. Varying #1 is unused.
                         * See the corresponding structures in the
                         * command stream. */

                        if (offset > 0)
                                offset += 1;

                        /* Do not emit the varying yet -- instead, just mark down that we need to later */

                        _mesa_hash_table_u64_insert(ctx->ssa_varyings, reg + 1, (void *) ((uintptr_t) (offset + 1)));
                } else {
                        printf("Unknown store\n");
                        assert(0);
                }

                break;

        default:
                printf ("Unhandled intrinsic\n");
                assert(0);
                break;
        }
}

static unsigned
midgard_tex_format(enum glsl_sampler_dim dim)
{
        switch (dim) {
        case GLSL_SAMPLER_DIM_2D:
                return TEXTURE_2D;

        case GLSL_SAMPLER_DIM_3D:
                return TEXTURE_3D;

        case GLSL_SAMPLER_DIM_CUBE:
                return TEXTURE_CUBE;

        default:
                printf("Unknown sampler dim type\n");
                assert(0);
                return 0;
        }
}

static void
emit_tex(compiler_context *ctx, nir_tex_instr *instr)
{
        /* TODO */
        //assert (!instr->sampler);
        //assert (!instr->texture_array_size);
        assert (instr->op == nir_texop_tex);

        /* Allocate registers via a round robin scheme to alternate between the two registers */
        int reg = ctx->texture_op_count & 1;
        int in_reg = reg, out_reg = reg;

        /* Make room for the reg */

        if (ctx->texture_index[reg] > -1)
                unalias_ssa(ctx, ctx->texture_index[reg]);

        int texture_index = instr->texture_index;
        int sampler_index = texture_index;

        for (unsigned i = 0; i < instr->num_srcs; ++i) {
                switch (instr->src[i].src_type) {
                case nir_tex_src_coord: {
                        int index = nir_src_index(&instr->src[i].src);

                        midgard_vector_alu_src alu_src = blank_alu_src;
                        alu_src.swizzle = (COMPONENT_Y << 2);

                        midgard_instruction ins = v_fmov(index, alu_src, SSA_FIXED_REGISTER(REGISTER_TEXTURE_BASE + in_reg));
                        emit_mir_instruction(ctx, ins);

                        //midgard_pin_output(ctx, index, REGISTER_TEXTURE_BASE + in_reg);

                        break;
                }

                default: {
                        printf("Unknown source type\n");
                        assert(0);
                        break;
                }
                }
        }

        /* No helper to build texture words -- we do it all here */
        midgard_instruction ins = {
                .type = TAG_TEXTURE_4,
                .texture = {
                        .op = TEXTURE_OP_NORMAL,
                        .format = midgard_tex_format(instr->sampler_dim),
                        .texture_handle = texture_index,
                        .sampler_handle = sampler_index,

                        /* TODO: Don't force xyzw */
                        .swizzle = SWIZZLE(COMPONENT_X, COMPONENT_Y, COMPONENT_Z, COMPONENT_W),
                        .mask = 0xF,

                        /* TODO: half */
                        //.in_reg_full = 1,
                        .out_full = 1,

                        .filter = 1,

                        /* Always 1 */
                        .unknown7 = 1,

                        /* Assume we can continue; hint it out later */
                        .cont = 1,
                }
        };

        /* Set registers to read and write from the same place */
        ins.texture.in_reg_select = in_reg;
        ins.texture.out_reg_select = out_reg;

        /* TODO: Dynamic swizzle input selection, half-swizzles? */
        if (instr->sampler_dim == GLSL_SAMPLER_DIM_3D) {
                ins.texture.in_reg_swizzle_right = COMPONENT_X;
                ins.texture.in_reg_swizzle_left = COMPONENT_Y;
                //ins.texture.in_reg_swizzle_third = COMPONENT_Z;
        } else {
                ins.texture.in_reg_swizzle_left = COMPONENT_X;
                ins.texture.in_reg_swizzle_right = COMPONENT_Y;
                //ins.texture.in_reg_swizzle_third = COMPONENT_X;
        }

        emit_mir_instruction(ctx, ins);

        /* Simultaneously alias the destination and emit a move for it. The move will be eliminated if possible */

        int o_reg = REGISTER_TEXTURE_BASE + out_reg, o_index = nir_dest_index(&instr->dest);
        alias_ssa(ctx, o_index, SSA_FIXED_REGISTER(o_reg));
        ctx->texture_index[reg] = o_index;

        midgard_instruction ins2 = v_fmov(SSA_FIXED_REGISTER(o_reg), blank_alu_src, o_index);
        emit_mir_instruction(ctx, ins2);

        /* Used for .cont and .last hinting */
        ctx->texture_op_count++;
}

static void
emit_jump(compiler_context *ctx, nir_jump_instr *instr)
{
        switch (instr->type) {
                case nir_jump_break: {
                        /* Emit a branch out of the loop */
                        struct midgard_instruction br = v_branch(false, false);
                        br.branch.target_type = TARGET_BREAK;
                        br.branch.target_break = ctx->current_loop;
                        emit_mir_instruction(ctx, br);

                        printf("break..\n");
                        break;
                }

                default:
                        printf("Unknown jump type %d\n", instr->type);
                        break;
        }
}

static void
emit_instr(compiler_context *ctx, struct nir_instr *instr)
{
#ifdef NIR_DEBUG_FINE
        nir_print_instr(instr, stdout);
        putchar('\n');
#endif

        switch (instr->type) {
        case nir_instr_type_load_const:
                emit_load_const(ctx, nir_instr_as_load_const(instr));
                break;

        case nir_instr_type_intrinsic:
                emit_intrinsic(ctx, nir_instr_as_intrinsic(instr));
                break;

        case nir_instr_type_alu:
                emit_alu(ctx, nir_instr_as_alu(instr));
                break;

        case nir_instr_type_tex:
                emit_tex(ctx, nir_instr_as_tex(instr));
                break;

        case nir_instr_type_jump:
                emit_jump(ctx, nir_instr_as_jump(instr));
                break;

        case nir_instr_type_ssa_undef:
                /* Spurious */
                break;

        default:
                printf("Unhandled instruction type\n");
                break;
        }
}

/* Determine the actual hardware from the index based on the RA results or special values */

static int
dealias_register(compiler_context *ctx, struct ra_graph *g, int reg, int maxreg)
{
        if (reg >= SSA_FIXED_MINIMUM)
                return SSA_REG_FROM_FIXED(reg);

        if (reg >= 0) {
                assert(reg < maxreg);
                int r = ra_get_node_reg(g, reg);
                ctx->work_registers = MAX2(ctx->work_registers, r);
                return r;
        }

        switch (reg) {
        /* fmov style unused */
        case SSA_UNUSED_0:
                return REGISTER_UNUSED;

        /* lut style unused */
        case SSA_UNUSED_1:
                return REGISTER_UNUSED;

        default:
                printf("Unknown SSA register alias %d\n", reg);
                assert(0);
                return 31;
        }
}

static unsigned int
midgard_ra_select_callback(struct ra_graph *g, BITSET_WORD *regs, void *data)
{
        /* Choose the first available register to minimise reported register pressure */

        for (int i = 0; i < 16; ++i) {
                if (BITSET_TEST(regs, i)) {
                        return i;
                }
        }

        assert(0);
        return 0;
}

static bool
midgard_is_live_in_instr(midgard_instruction *ins, int src)
{
        if (ins->ssa_args.src0 == src) return true;
        if (ins->ssa_args.src1 == src) return true;

        return false;
}

static bool
is_live_after(compiler_context *ctx, midgard_block *block, midgard_instruction *start, int src)
{
        /* Check the rest of the block for liveness */
        mir_foreach_instr_in_block_from(block, ins, mir_next_op(start)) {
                if (midgard_is_live_in_instr(ins, src))
                        return true;
        }

        /* Check the rest of the blocks for liveness */
        mir_foreach_block_from(ctx, mir_next_block(block), b) {
                mir_foreach_instr_in_block(b, ins) {
                        if (midgard_is_live_in_instr(ins, src))
                                return true;
                }
        }

        /* TODO: How does control flow interact in complex shaders? */

        return false;
}

static void
allocate_registers(compiler_context *ctx)
{
        /* First, initialize the RA */
        struct ra_regs *regs = ra_alloc_reg_set(NULL, 32, true);

        /* Create a primary (general purpose) class, as well as special purpose
         * pipeline register classes */

        int primary_class = ra_alloc_reg_class(regs);
        int varying_class  = ra_alloc_reg_class(regs);

        /* Add the full set of work registers */
        int work_count = 16 - MAX2((ctx->uniform_cutoff - 8), 0);
        for (int i = 0; i < work_count; ++i)
                ra_class_add_reg(regs, primary_class, i);

        /* Add special registers */
        ra_class_add_reg(regs, varying_class, REGISTER_VARYING_BASE);
        ra_class_add_reg(regs, varying_class, REGISTER_VARYING_BASE + 1);

        /* We're done setting up */
        ra_set_finalize(regs, NULL);

        /* Transform the MIR into squeezed index form */
        mir_foreach_block(ctx, block) {
                mir_foreach_instr_in_block(block, ins) {
                        if (ins->compact_branch) continue;

                        ins->ssa_args.src0 = find_or_allocate_temp(ctx, ins->ssa_args.src0);
                        ins->ssa_args.src1 = find_or_allocate_temp(ctx, ins->ssa_args.src1);
                        ins->ssa_args.dest = find_or_allocate_temp(ctx, ins->ssa_args.dest);
                }

                print_mir_block(block);
        }

        /* Let's actually do register allocation */
        int nodes = ctx->temp_count;
        struct ra_graph *g = ra_alloc_interference_graph(regs, nodes);

        /* Set everything to the work register class, unless it has somewhere
         * special to go */

        mir_foreach_block(ctx, block) {
                mir_foreach_instr_in_block(block, ins) {
                        if (ins->compact_branch) continue;

                        if (ins->ssa_args.dest < 0) continue;

                        if (ins->ssa_args.dest >= SSA_FIXED_MINIMUM) continue;

                        int class = primary_class;

                        ra_set_node_class(g, ins->ssa_args.dest, class);
                }
        }

        for (int index = 0; index <= ctx->max_hash; ++index) {
                unsigned temp = (uintptr_t) _mesa_hash_table_u64_search(ctx->ssa_to_register, index + 1);

                if (temp) {
                        unsigned reg = temp - 1;
                        int t = find_or_allocate_temp(ctx, index);
                        ra_set_node_reg(g, t, reg);
                }
        }

        /* Determine liveness */

        int *live_start = malloc(nodes * sizeof(int));
        int *live_end = malloc(nodes * sizeof(int));

        /* Initialize as non-existent */

        for (int i = 0; i < nodes; ++i) {
                live_start[i] = live_end[i] = -1;
        }

        int d = 0;

        mir_foreach_block(ctx, block) {
                mir_foreach_instr_in_block(block, ins) {
                        if (ins->compact_branch) continue;

                        if (ins->ssa_args.dest < SSA_FIXED_MINIMUM) {
                                /* If this destination is not yet live, it is now since we just wrote it */

                                int dest = ins->ssa_args.dest;

                                if (live_start[dest] == -1)
                                        live_start[dest] = d;
                        }

                        /* Since we just used a source, the source might be
                         * dead now. Scan the rest of the block for
                         * invocations, and if there are none, the source dies
                         * */

                        int sources[2] = { ins->ssa_args.src0, ins->ssa_args.src1 };

                        for (int src = 0; src < 2; ++src) {
                                int s = sources[src];

                                if (s < 0) continue;

                                if (s >= SSA_FIXED_MINIMUM) continue;

                                if (!is_live_after(ctx, block, ins, s)) {
                                        live_end[s] = d;
                                }
                        }

                        ++d;
                }
        }

        /* If a node still hasn't been killed, kill it now */

        for (int i = 0; i < nodes; ++i) {
                /* live_start == -1 most likely indicates a pinned output */

                if (live_end[i] == -1)
                        live_end[i] = d;
        }

        /* Setup interference between nodes that are live at the same time */

        for (int i = 0; i < nodes; ++i) {
                for (int j = i + 1; j < nodes; ++j) {
                        if (!(live_start[i] >= live_end[j] || live_start[j] >= live_end[i]))
                                ra_add_node_interference(g, i, j);
                }
        }

        ra_set_select_reg_callback(g, midgard_ra_select_callback, NULL);

        if (!ra_allocate(g)) {
                printf("Error allocating registers\n");
                assert(0);
        }

        /* Cleanup */
        free(live_start);
        free(live_end);

        mir_foreach_block(ctx, block) {
                mir_foreach_instr_in_block(block, ins) {
                        if (ins->compact_branch) continue;

                        ssa_args args = ins->ssa_args;

                        switch (ins->type) {
                        case TAG_ALU_4:
                                ins->registers.src1_reg = dealias_register(ctx, g, args.src0, nodes);

                                ins->registers.src2_imm = args.inline_constant;

                                if (args.inline_constant) {
                                        /* Encode inline 16-bit constant as a vector by default */

                                        ins->registers.src2_reg = ins->inline_constant >> 11;

                                        int lower_11 = ins->inline_constant & ((1 << 12) - 1);

                                        uint16_t imm = ((lower_11 >> 8) & 0x7) | ((lower_11 & 0xFF) << 3);
                                        ins->alu.src2 = imm << 2;
                                } else {
                                        ins->registers.src2_reg = dealias_register(ctx, g, args.src1, nodes);
                                }

                                ins->registers.out_reg = dealias_register(ctx, g, args.dest, nodes);

                                break;

                        case TAG_LOAD_STORE_4: {
                                if (OP_IS_STORE(ins->load_store.op)) {
                                        /* TODO: use ssa_args for store_vary */
                                        ins->load_store.reg = 0;
                                } else {
                                        bool has_dest = args.dest >= 0;
                                        int ssa_arg = has_dest ? args.dest : args.src0;

                                        ins->load_store.reg = dealias_register(ctx, g, ssa_arg, nodes);
                                }

                                break;
                        }

                        default:
                                break;
                        }
                }
        }
}

/* Midgard IR only knows vector ALU types, but we sometimes need to actually
 * use scalar ALU instructions, for functional or performance reasons. To do
 * this, we just demote vector ALU payloads to scalar. */

static int
component_from_mask(unsigned mask)
{
        for (int c = 0; c < 4; ++c) {
                if (mask & (3 << (2 * c)))
                        return c;
        }

        assert(0);
        return 0;
}

static bool
is_single_component_mask(unsigned mask)
{
        int components = 0;

        for (int c = 0; c < 4; ++c)
                if (mask & (3 << (2 * c)))
                        components++;

        return components == 1;
}

/* Create a mask of accessed components from a swizzle to figure out vector
 * dependencies */

static unsigned
swizzle_to_access_mask(unsigned swizzle)
{
        unsigned component_mask = 0;

        for (int i = 0; i < 4; ++i) {
                unsigned c = (swizzle >> (2 * i)) & 3;
                component_mask |= (1 << c);
        }

        return component_mask;
}

static unsigned
vector_to_scalar_source(unsigned u)
{
        midgard_vector_alu_src v;
        memcpy(&v, &u, sizeof(v));

        midgard_scalar_alu_src s = {
                .abs = v.abs,
                .negate = v.negate,
                .full = !v.half,
                .component = (v.swizzle & 3) << 1
        };

        unsigned o;
        memcpy(&o, &s, sizeof(s));

        return o & ((1 << 6) - 1);
}

static midgard_scalar_alu
vector_to_scalar_alu(midgard_vector_alu v, midgard_instruction *ins)
{
        /* The output component is from the mask */
        midgard_scalar_alu s = {
                .op = v.op,
                .src1 = vector_to_scalar_source(v.src1),
                .src2 = vector_to_scalar_source(v.src2),
                .unknown = 0,
                .outmod = v.outmod,
                .output_full = 1, /* TODO: Half */
                .output_component = component_from_mask(v.mask) << 1,
        };

        /* Inline constant is passed along rather than trying to extract it
         * from v */

        if (ins->ssa_args.inline_constant) {
                uint16_t imm = 0;
                int lower_11 = ins->inline_constant & ((1 << 12) - 1);
                imm |= (lower_11 >> 9) & 3;
                imm |= (lower_11 >> 6) & 4;
                imm |= (lower_11 >> 2) & 0x38;
                imm |= (lower_11 & 63) << 6;

                s.src2 = imm;
        }

        return s;
}

/* Midgard prefetches instruction types, so during emission we need to
 * lookahead too. Unless this is the last instruction, in which we return 1. Or
 * if this is the second to last and the last is an ALU, then it's also 1... */

#define IS_ALU(tag) (tag == TAG_ALU_4 || tag == TAG_ALU_8 ||  \
		     tag == TAG_ALU_12 || tag == TAG_ALU_16)

#define EMIT_AND_COUNT(type, val) util_dynarray_append(emission, type, val); \
				  bytes_emitted += sizeof(type)

static void
emit_binary_vector_instruction(midgard_instruction *ains,
                               uint16_t *register_words, int *register_words_count,
                               uint64_t *body_words, size_t *body_size, int *body_words_count,
                               size_t *bytes_emitted)
{
        memcpy(&register_words[(*register_words_count)++], &ains->registers, sizeof(ains->registers));
        *bytes_emitted += sizeof(midgard_reg_info);

        body_size[*body_words_count] = sizeof(midgard_vector_alu);
        memcpy(&body_words[(*body_words_count)++], &ains->alu, sizeof(ains->alu));
        *bytes_emitted += sizeof(midgard_vector_alu);
}

/* Checks for an SSA data hazard between two adjacent instructions, keeping in
 * mind that we are a vector architecture and we can write to different
 * components simultaneously */

static bool
can_run_concurrent_ssa(midgard_instruction *first, midgard_instruction *second)
{
        /* Each instruction reads some registers and writes to a register. See
         * where the first writes */

        /* Figure out where exactly we wrote to */
        int source = first->ssa_args.dest;
        int source_mask = first->type == TAG_ALU_4 ? squeeze_writemask(first->alu.mask) : 0xF;

        /* As long as the second doesn't read from the first, we're okay */
        if (second->ssa_args.src0 == source) {
                if (first->type == TAG_ALU_4) {
                        /* Figure out which components we just read from */

                        int q = second->alu.src1;
                        midgard_vector_alu_src *m = (midgard_vector_alu_src *) &q;

                        /* Check if there are components in common, and fail if so */
                        if (swizzle_to_access_mask(m->swizzle) & source_mask)
                                return false;
                } else
                        return false;

        }

        if (second->ssa_args.src1 == source)
                return false;

        /* Otherwise, it's safe in that regard. Another data hazard is both
         * writing to the same place, of course */

        if (second->ssa_args.dest == source) {
                /* ...but only if the components overlap */
                int dest_mask = second->type == TAG_ALU_4 ? squeeze_writemask(second->alu.mask) : 0xF;

                if (dest_mask & source_mask)
                        return false;
        }

        /* ...That's it */
        return true;
}

/* Schedules, but does not emit, a single basic block. After scheduling, the
 * final tag and size of the block are known, which are necessary for branching
 * */

static midgard_bundle
schedule_bundle(compiler_context *ctx, midgard_block *block, midgard_instruction *ins, int *skip)
{
        int instructions_emitted = 0, instructions_consumed = -1;
        midgard_bundle bundle = { 0 };

        uint8_t tag = ins->type;

        /* Default to the instruction's tag */
        bundle.tag = tag;

        switch (ins->type) {
        case TAG_ALU_4: {
                uint32_t control = 0;
                size_t bytes_emitted = sizeof(control);

                /* TODO: Constant combining */
                int index = 0, last_unit = 0;

                /* Previous instructions, for the purpose of parallelism */
                midgard_instruction *segment[4] = {0};
                int segment_size = 0;

                instructions_emitted = -1;
                midgard_instruction *pins = ins;

                for (;;) {
                        midgard_instruction *ains = pins;

                        /* Advance instruction pointer */
                        if (index) {
                                ains = mir_next_op(pins);
                                pins = ains;
                        }

                        /* Out-of-work condition */
                        if ((struct list_head *) ains == &block->instructions)
                                break;

                        /* Ensure that the chain can continue */
                        if (ains->type != TAG_ALU_4) break;

                        /* According to the presentation "The ARM
                         * Mali-T880 Mobile GPU" from HotChips 27,
                         * there are two pipeline stages. Branching
                         * position determined experimentally. Lines
                         * are executed in parallel:
                         *
                         * [ VMUL ] [ SADD ]
                         * [ VADD ] [ SMUL ] [ LUT ] [ BRANCH ]
                         *
                         * Verify that there are no ordering dependencies here.
                         *
                         * TODO: Allow for parallelism!!!
                         */

                        /* Pick a unit for it if it doesn't force a particular unit */

                        int unit = ains->unit;

                        if (!unit) {
                                int op = ains->alu.op;
                                int units = alu_opcode_props[op];

                                /* TODO: Promotion of scalars to vectors */
                                int vector = ((!is_single_component_mask(ains->alu.mask)) || ((units & UNITS_SCALAR) == 0)) && (units & UNITS_ANY_VECTOR);

                                if (!vector)
                                        assert(units & UNITS_SCALAR);

                                if (vector) {
                                        if (last_unit >= UNIT_VADD) {
                                                if (units & UNIT_VLUT)
                                                        unit = UNIT_VLUT;
                                                else
                                                        break;
                                        } else {
                                                if ((units & UNIT_VMUL) && !(control & UNIT_VMUL))
                                                        unit = UNIT_VMUL;
                                                else if ((units & UNIT_VADD) && !(control & UNIT_VADD))
                                                        unit = UNIT_VADD;
                                                else if (units & UNIT_VLUT)
                                                        unit = UNIT_VLUT;
                                                else
                                                        break;
                                        }
                                } else {
                                        if (last_unit >= UNIT_VADD) {
                                                if ((units & UNIT_SMUL) && !(control & UNIT_SMUL))
                                                        unit = UNIT_SMUL;
                                                else if (units & UNIT_VLUT)
                                                        unit = UNIT_VLUT;
                                                else
                                                        break;
                                        } else {
                                                if ((units & UNIT_SADD) && !(control & UNIT_SADD))
                                                        unit = UNIT_SADD;
                                                else if (units & UNIT_SMUL)
                                                        unit = UNIT_SMUL;
                                                else if ((units & UNIT_VADD) && !(control & UNIT_VADD))
                                                        unit = UNIT_VADD;
                                                else
                                                        break;
                                        }
                                }

                                assert(unit & units);
                        }

                        /* Late unit check, this time for encoding (not parallelism) */
                        if (unit <= last_unit) break;

                        /* Clear the segment */
                        if (last_unit < UNIT_VADD && unit >= UNIT_VADD)
                                segment_size = 0;

                        /* Check for data hazards */
                        int has_hazard = false;

                        for (int s = 0; s < segment_size; ++s)
                                if (!can_run_concurrent_ssa(segment[s], ains))
                                        has_hazard = true;

                        if (has_hazard)
                                break;

                        /* We're good to go -- emit the instruction */
                        ains->unit = unit;

                        segment[segment_size++] = ains;

                        /* Only one set of embedded constants per
                         * bundle possible; if we have more, we must
                         * break the chain early, unfortunately */

                        if (ains->has_constants) {
                                if (bundle.has_embedded_constants) {
                                        /* ...but if there are already
                                         * constants but these are the
                                         * *same* constants, we let it
                                         * through */

                                        if (memcmp(bundle.constants, ains->constants, sizeof(bundle.constants)))
                                                break;
                                } else {
                                        bundle.has_embedded_constants = true;
                                        memcpy(bundle.constants, ains->constants, sizeof(bundle.constants));

                                        /* If this is a blend shader special constant, track it for patching */
                                        if (ains->has_blend_constant)
                                                bundle.has_blend_constant = true;
                                }
                        }

                        if (ains->unit & UNITS_ANY_VECTOR) {
                                emit_binary_vector_instruction(ains, bundle.register_words,
                                                               &bundle.register_words_count, bundle.body_words,
                                                               bundle.body_size, &bundle.body_words_count, &bytes_emitted);
                        } else if (ains->compact_branch) {
                                /* All of r0 has to be written out
                                 * along with the branch writeout.
                                 * (slow!) */

                                if (ains->writeout) {
                                        if (index == 0) {
                                                midgard_instruction ins = v_fmov(0, blank_alu_src, SSA_FIXED_REGISTER(0));
                                                ins.unit = UNIT_VMUL;

                                                control |= ins.unit;

                                                emit_binary_vector_instruction(&ins, bundle.register_words,
                                                                               &bundle.register_words_count, bundle.body_words,
                                                                               bundle.body_size, &bundle.body_words_count, &bytes_emitted);
                                        } else {
                                                /* Analyse the group to see if r0 is written in full, on-time, without hanging dependencies*/
                                                bool written_late = false;
                                                bool components[4] = { 0 };
                                                uint16_t register_dep_mask = 0;
                                                uint16_t written_mask = 0;

                                                midgard_instruction *qins = ins;
                                                for (int t = 0; t < index; ++t) {
                                                        if (qins->registers.out_reg != 0) {
                                                                /* Mark down writes */

                                                                written_mask |= (1 << qins->registers.out_reg);
                                                        } else {
                                                                /* Mark down the register dependencies for errata check */

                                                                if (qins->registers.src1_reg < 16)
                                                                        register_dep_mask |= (1 << qins->registers.src1_reg);

                                                                if (qins->registers.src2_reg < 16)
                                                                        register_dep_mask |= (1 << qins->registers.src2_reg);

                                                                int mask = qins->alu.mask;

                                                                for (int c = 0; c < 4; ++c)
                                                                        if (mask & (0x3 << (2 * c)))
                                                                                components[c] = true;

                                                                /* ..but if the writeout is too late, we have to break up anyway... for some reason */

                                                                if (qins->unit == UNIT_VLUT)
                                                                        written_late = true;
                                                        }

                                                        /* Advance instruction pointer */
                                                        qins = mir_next_op(qins);
                                                }


                                                /* ERRATA (?): In a bundle ending in a fragment writeout, the register dependencies of r0 cannot be written within this bundle (discovered in -bshading:shading=phong) */
                                                if (register_dep_mask & written_mask) {
                                                        printf("ERRATA WORKAROUND: Breakup for writeout dependency masks %X vs %X (common %X)\n", register_dep_mask, written_mask, register_dep_mask & written_mask);
                                                        break;
                                                }

                                                if (written_late)
                                                        break;

                                                /* If even a single component is not written, break it up (conservative check). */
                                                bool breakup = false;

                                                for (int c = 0; c < 4; ++c)
                                                        if (!components[c])
                                                                breakup = true;

                                                if (breakup)
                                                        break;

                                                /* Otherwise, we're free to proceed */
                                        }
                                }

                                bundle.body_size[bundle.body_words_count] = sizeof(ains->br_compact);
                                memcpy(&bundle.body_words[bundle.body_words_count++], &ains->br_compact, sizeof(ains->br_compact));
                                bytes_emitted += sizeof(ains->br_compact);
                        } else {
                                memcpy(&bundle.register_words[bundle.register_words_count++], &ains->registers, sizeof(ains->registers));
                                bytes_emitted += sizeof(midgard_reg_info);

                                bundle.body_size[bundle.body_words_count] = sizeof(midgard_scalar_alu);
                                bundle.body_words_count++;
                                bytes_emitted += sizeof(midgard_scalar_alu);
                        }

                        /* Defer marking until after writing to allow for break */
                        control |= ains->unit;
                        last_unit = ains->unit;
                        ++instructions_emitted;
                        ++index;
                }

                /* Bubble up the number of instructions for skipping */
                instructions_consumed = index - 1;

                int padding = 0;

                /* Pad ALU op to nearest word */

                if (bytes_emitted & 15) {
                        padding = 16 - (bytes_emitted & 15);
                        bytes_emitted += padding;
                }

                /* Constants must always be quadwords */
                if (bundle.has_embedded_constants)
                        bytes_emitted += 16;

                /* Size ALU instruction for tag */
                bundle.tag = (TAG_ALU_4) + (bytes_emitted / 16) - 1;
                bundle.padding = padding;
                bundle.control = bundle.tag | control;

                break;
        }

        case TAG_LOAD_STORE_4: {
                /* Load store instructions have two words at once. If
                 * we only have one queued up, we need to NOP pad.
                 * Otherwise, we store both in succession to save space
                 * and cycles -- letting them go in parallel -- skip
                 * the next. The usefulness of this optimisation is
                 * greatly dependent on the quality of the instruction
                 * scheduler.
                 */

                midgard_instruction *next_op = mir_next_op(ins);

                if ((struct list_head *) next_op != &block->instructions && next_op->type == TAG_LOAD_STORE_4) {
                        /* As the two operate concurrently, make sure
                         * they are not dependent */

                        if (can_run_concurrent_ssa(ins, next_op) || true) {
                                /* Skip ahead, since it's redundant with the pair */
                                instructions_consumed = 1 + (instructions_emitted++);
                        }
                }

                break;
        }

#if 0

        case TAG_TEXTURE_4:
                /* TODO: Schedule texture ops */
                break;
#endif

        default:
                /* XXX: What happens with textures? */
                break;
        }

        /* Copy the instructions into the bundle */
        bundle.instruction_count = instructions_emitted + 1;

        int used_idx = 0;

        midgard_instruction *uins = ins;
        for (int i = 0; used_idx < bundle.instruction_count; ++i) {
                bundle.instructions[used_idx++] = *uins;
                uins = mir_next_op(uins);
        }

        *skip = (instructions_consumed == -1) ? instructions_emitted : instructions_consumed;

        return bundle;
}

static int
quadword_size(int tag)
{
        switch (tag) {
        case TAG_ALU_4:
                return 1;

        case TAG_ALU_8:
                return 2;

        case TAG_ALU_12:
                return 3;

        case TAG_ALU_16:
                return 4;

        case TAG_LOAD_STORE_4:
                return 1;

        case TAG_TEXTURE_4:
                return 1;

        default:
                assert(0);
                return 0;
        }
}

/* Schedule a single block by iterating its instruction to create bundles.
 * While we go, tally about the bundle sizes to compute the block size. */

static void
schedule_block(compiler_context *ctx, midgard_block *block)
{
        util_dynarray_init(&block->bundles, NULL);

        block->quadword_count = 0;

        mir_foreach_instr_in_block(block, ins) {
                int skip;
                midgard_bundle bundle = schedule_bundle(ctx, block, ins, &skip);
                util_dynarray_append(&block->bundles, midgard_bundle, bundle);

                if (bundle.has_blend_constant) {
                        /* TODO: Multiblock? */
                        int quadwords_within_block = block->quadword_count + quadword_size(bundle.tag) - 1;
                        ctx->blend_constant_offset = quadwords_within_block * 0x10;
                }

                while(skip--)
                        ins = mir_next_op(ins);

                block->quadword_count += quadword_size(bundle.tag);
        }

        block->is_scheduled = true;
}

static void
schedule_program(compiler_context *ctx)
{
        allocate_registers(ctx);

        mir_foreach_block(ctx, block) {
                schedule_block(ctx, block);
        }
}

/* After everything is scheduled, emit whole bundles at a time */

static void
emit_binary_bundle(compiler_context *ctx, midgard_bundle *bundle, struct util_dynarray *emission, int next_tag)
{
        int lookahead = next_tag << 4;

        switch (bundle->tag) {
        case TAG_ALU_4:
        case TAG_ALU_8:
        case TAG_ALU_12:
        case TAG_ALU_16: {
                /* Actually emit each component */
                util_dynarray_append(emission, uint32_t, bundle->control | lookahead);

                for (int i = 0; i < bundle->register_words_count; ++i)
                        util_dynarray_append(emission, uint16_t, bundle->register_words[i]);

                /* Emit body words based on the instructions bundled */
                for (int i = 0; i < bundle->instruction_count; ++i) {
                        midgard_instruction *ins = &bundle->instructions[i];

                        if (ins->unit & UNITS_ANY_VECTOR) {
                                memcpy(util_dynarray_grow(emission, sizeof(midgard_vector_alu)), &ins->alu, sizeof(midgard_vector_alu));
                        } else if (ins->compact_branch) {
                                /* Dummy move, XXX DRY */
                                if ((i == 0) && ins->writeout) {
                                        midgard_instruction ins = v_fmov(0, blank_alu_src, SSA_FIXED_REGISTER(0));
                                        memcpy(util_dynarray_grow(emission, sizeof(midgard_vector_alu)), &ins.alu, sizeof(midgard_vector_alu));
                                }

                                memcpy(util_dynarray_grow(emission, sizeof(ins->br_compact)), &ins->br_compact, sizeof(ins->br_compact));
                        } else {
                                /* Scalar */
                                midgard_scalar_alu scalarised = vector_to_scalar_alu(ins->alu, ins);
                                memcpy(util_dynarray_grow(emission, sizeof(scalarised)), &scalarised, sizeof(scalarised));
                        }
                }

                /* Emit padding (all zero) */
                memset(util_dynarray_grow(emission, bundle->padding), 0, bundle->padding);

                /* Tack on constants */

                if (bundle->has_embedded_constants) {
                        util_dynarray_append(emission, float, bundle->constants[0]);
                        util_dynarray_append(emission, float, bundle->constants[1]);
                        util_dynarray_append(emission, float, bundle->constants[2]);
                        util_dynarray_append(emission, float, bundle->constants[3]);
                }

                break;
        }

        case TAG_LOAD_STORE_4: {
                /* One or two composing instructions */

                uint64_t current64, next64 = LDST_NOP;

                memcpy(&current64, &bundle->instructions[0].load_store, sizeof(current64));

                if (bundle->instruction_count == 2)
                        memcpy(&next64, &bundle->instructions[1].load_store, sizeof(next64));

                midgard_load_store instruction = {
                        .type = bundle->tag,
                        .next_type = next_tag,
                        .word1 = current64,
                        .word2 = next64
                };

                util_dynarray_append(emission, midgard_load_store, instruction);

                break;
        }

        case TAG_TEXTURE_4: {
                /* Texture instructions are easy, since there is no
                 * pipelining nor VLIW to worry about. We may need to set the .last flag */

                midgard_instruction *ins = &bundle->instructions[0];

                ins->texture.type = TAG_TEXTURE_4;
                ins->texture.next_type = next_tag;

                ctx->texture_op_count--;

                if (!ctx->texture_op_count) {
                        ins->texture.cont = 0;
                        ins->texture.last = 1;
                }

                util_dynarray_append(emission, midgard_texture_word, ins->texture);
                break;
        }

        default:
                printf("Unknown midgard instruction type\n");
                assert(0);
                break;
        }
}


/* ALU instructions can inline or embed constants, which decreases register
 * pressure and saves space. */

#define CONDITIONAL_ATTACH(src) { \
	void *entry = _mesa_hash_table_u64_search(ctx->ssa_constants, alu->ssa_args.src + 1); \
\
	if (entry) { \
		attach_constants(ctx, alu, entry, alu->ssa_args.src + 1); \
		alu->ssa_args.src = SSA_FIXED_REGISTER(REGISTER_CONSTANT); \
	} \
}

static void
inline_alu_constants(compiler_context *ctx)
{
        mir_foreach_instr(ctx, alu) {
                /* Other instructions cannot inline constants */
                if (alu->type != TAG_ALU_4) continue;

                /* If there is already a constant here, we can do nothing */
                if (alu->has_constants) continue;

                CONDITIONAL_ATTACH(src0);

                if (!alu->has_constants) {
                        if (!alu->ssa_args.inline_constant)
                                CONDITIONAL_ATTACH(src1)
                        } else if (!alu->ssa_args.inline_constant) {
                        /* Corner case: _two_ vec4 constants, for instance with a
                         * csel. For this case, we can only use a constant
                         * register for one, we'll have to emit a move for the
                         * other. Note, if both arguments are constants, then
                         * necessarily neither argument depends on the value of
                         * any particular register. As the destination register
                         * will be wiped, that means we can spill the constant
                         * to the destination register.
                         */

                        void *entry = _mesa_hash_table_u64_search(ctx->ssa_constants, alu->ssa_args.src1 + 1);

                        if (entry) {
                                midgard_instruction ins = v_fmov(SSA_FIXED_REGISTER(REGISTER_CONSTANT), blank_alu_src, 4096 + alu->ssa_args.src1);
                                attach_constants(ctx, &ins, entry, alu->ssa_args.src1 + 1);

                                /* Force a break XXX Defer r31 writes */
                                ins.unit = UNIT_VLUT;

                                /* Set the source */
                                alu->ssa_args.src1 = 4096 + alu->ssa_args.src1;

                                /* Inject us -before- the last instruction which set r31 */
                                mir_insert_instruction_before(mir_prev_op(alu), ins);
                        }
                }
        }
}

/* Midgard supports two types of constants, embedded constants (128-bit) and
 * inline constants (16-bit). Sometimes, especially with scalar ops, embedded
 * constants can be demoted to inline constants, for space savings and
 * sometimes a performance boost */

static void
embedded_to_inline_constant(compiler_context *ctx)
{
        mir_foreach_instr(ctx, ins) {
                if (!ins->has_constants) continue;

                if (ins->ssa_args.inline_constant) continue;

                /* Blend constants must not be inlined by definition */
                if (ins->has_blend_constant) continue;

                /* src1 cannot be an inline constant due to encoding
                 * restrictions. So, if possible we try to flip the arguments
                 * in that case */

                int op = ins->alu.op;

                if (ins->ssa_args.src0 == SSA_FIXED_REGISTER(REGISTER_CONSTANT)) {
                        /* Flip based on op. Fallthrough intentional */

                        switch (op) {
                        /* These ops require an operational change to flip their arguments TODO */
                        case midgard_alu_op_flt:
                        case midgard_alu_op_fle:
                        case midgard_alu_op_ilt:
                        case midgard_alu_op_ile:
                        case midgard_alu_op_fcsel:
                        case midgard_alu_op_icsel:
                        case midgard_alu_op_isub:
                                printf("Missed non-commutative flip (%s)\n", alu_opcode_names[op]);
                                break;

                        /* These ops are commutative and Just Flip */
                        case midgard_alu_op_fne:
                        case midgard_alu_op_fadd:
                        case midgard_alu_op_fmul:
                        case midgard_alu_op_fmin:
                        case midgard_alu_op_fmax:
                        case midgard_alu_op_iadd:
                        case midgard_alu_op_imul:
                        case midgard_alu_op_feq:
                        case midgard_alu_op_ieq:
                        case midgard_alu_op_ine:
                        case midgard_alu_op_iand:
                        case midgard_alu_op_ior:
                        case midgard_alu_op_ixor:
                                /* Flip the SSA numbers */
                                ins->ssa_args.src0 = ins->ssa_args.src1;
                                ins->ssa_args.src1 = SSA_FIXED_REGISTER(REGISTER_CONSTANT);

                                /* And flip the modifiers */

                                unsigned src_temp;

                                src_temp = ins->alu.src2;
                                ins->alu.src2 = ins->alu.src1;
                                ins->alu.src1 = src_temp;

                        default:
                                break;
                        }
                }

                if (ins->ssa_args.src1 == SSA_FIXED_REGISTER(REGISTER_CONSTANT)) {
                        /* Extract the source information */

                        midgard_vector_alu_src *src;
                        int q = ins->alu.src2;
                        midgard_vector_alu_src *m = (midgard_vector_alu_src *) &q;
                        src = m;

                        /* Component is from the swizzle, e.g. r26.w -> w component. TODO: What if x is masked out? */
                        int component = src->swizzle & 3;

                        /* Scale constant appropriately, if we can legally */
                        uint16_t scaled_constant = 0;

                        /* XXX: Check legality */
                        if (midgard_is_integer_op(op)) {
                                /* TODO: Inline integer */
                                continue;

                                unsigned int *iconstants = (unsigned int *) ins->constants;
                                scaled_constant = (uint16_t) iconstants[component];

                                /* Constant overflow after resize */
                                if (scaled_constant != iconstants[component])
                                        continue;
                        } else {
                                scaled_constant = _mesa_float_to_half((float) ins->constants[component]);
                        }

                        /* We don't know how to handle these with a constant */

                        if (src->abs || src->negate || src->half || src->rep_low || src->rep_high) {
                                printf("Bailing inline constant...\n");
                                continue;
                        }

                        /* Make sure that the constant is not itself a
                         * vector by checking if all accessed values
                         * (by the swizzle) are the same. */

                        uint32_t *cons = (uint32_t *) ins->constants;
                        uint32_t value = cons[component];

                        bool is_vector = false;
                        unsigned mask = effective_writemask(&ins->alu);

                        for (int c = 1; c < 4; ++c) {
                                /* We only care if this component is actually used */
                                if (!(mask & (1 << c)))
                                        continue;

                                uint32_t test = cons[(src->swizzle >> (2 * c)) & 3];

                                if (test != value) {
                                        is_vector = true;
                                        break;
                                }
                        }

                        if (is_vector)
                                continue;

                        /* Get rid of the embedded constant */
                        ins->has_constants = false;
                        ins->ssa_args.src1 = SSA_UNUSED_0;
                        ins->ssa_args.inline_constant = true;
                        ins->inline_constant = scaled_constant;
                }
        }
}

/* Map normal SSA sources to other SSA sources / fixed registers (like
 * uniforms) */

static void
map_ssa_to_alias(compiler_context *ctx, int *ref)
{
        unsigned int alias = (uintptr_t) _mesa_hash_table_u64_search(ctx->ssa_to_alias, *ref + 1);

        if (alias) {
                /* Remove entry in leftovers to avoid a redunant fmov */

                struct set_entry *leftover = _mesa_set_search(ctx->leftover_ssa_to_alias, ((void *) (uintptr_t) (*ref + 1)));

                if (leftover)
                        _mesa_set_remove(ctx->leftover_ssa_to_alias, leftover);

                /* Assign the alias map */
                *ref = alias - 1;
                return;
        }
}

#define AS_SRC(to, u) \
	int q##to = ins->alu.src2; \
	midgard_vector_alu_src *to = (midgard_vector_alu_src *) &q##to;

/* Removing unused moves is necessary to clean up the texture pipeline results.
 *
 * To do so, we find moves in the MIR. We check if their destination is live later. If it's not, the move is redundant. */

static void
midgard_eliminate_orphan_moves(compiler_context *ctx, midgard_block *block)
{
        mir_foreach_instr_in_block_safe(block, ins) {
                if (ins->type != TAG_ALU_4) continue;

                if (ins->alu.op != midgard_alu_op_fmov) continue;

                if (ins->ssa_args.dest >= SSA_FIXED_MINIMUM) continue;

                if (midgard_is_pinned(ctx, ins->ssa_args.dest)) continue;

                if (is_live_after(ctx, block, ins, ins->ssa_args.dest)) continue;

                mir_remove_instruction(ins);
        }
}

/* The following passes reorder MIR instructions to enable better scheduling */

static void
midgard_pair_load_store(compiler_context *ctx, midgard_block *block)
{
        mir_foreach_instr_in_block_safe(block, ins) {
                if (ins->type != TAG_LOAD_STORE_4) continue;

                /* We've found a load/store op. Check if next is also load/store. */
                midgard_instruction *next_op = mir_next_op(ins);
                if (&next_op->link != &block->instructions) {
                        if (next_op->type == TAG_LOAD_STORE_4) {
                                /* If so, we're done since we're a pair */
                                ins = mir_next_op(ins);
                                continue;
                        }

                        /* Maximum search distance to pair, to avoid register pressure disasters */
                        int search_distance = 8;

                        /* Otherwise, we have an orphaned load/store -- search for another load */
                        mir_foreach_instr_in_block_from(block, c, mir_next_op(ins)) {
                                /* Terminate search if necessary */
                                if (!(search_distance--)) break;

                                if (c->type != TAG_LOAD_STORE_4) continue;

                                if (OP_IS_STORE(c->load_store.op)) continue;

                                /* We found one! Move it up to pair and remove it from the old location */

                                mir_insert_instruction_before(ins, *c);
                                mir_remove_instruction(c);

                                break;
                        }
                }
        }
}

/* Emit varying stores late */

static void
midgard_emit_store(compiler_context *ctx, midgard_block *block) {
        /* Iterate in reverse to get the final write, rather than the first */

        mir_foreach_instr_in_block_safe_rev(block, ins) {
                /* Check if what we just wrote needs a store */
                int idx = ins->ssa_args.dest;
                uintptr_t varying = ((uintptr_t) _mesa_hash_table_u64_search(ctx->ssa_varyings, idx + 1));

                if (!varying) continue;

                varying -= 1;

                /* We need to store to the appropriate varying, so emit the
                 * move/store */

                /* TODO: Integrate with special purpose RA (and scheduler?) */
                bool high_varying_register = false;

                midgard_instruction mov = v_fmov(idx, blank_alu_src, SSA_FIXED_REGISTER(REGISTER_VARYING_BASE + high_varying_register));

                midgard_instruction st = m_store_vary_32(SSA_FIXED_REGISTER(high_varying_register), varying);
                st.load_store.unknown = 0x1E9E; /* XXX: What is this? */

                mir_insert_instruction_before(mir_next_op(ins), st);
                mir_insert_instruction_before(mir_next_op(ins), mov);

                /* We no longer need to store this varying */
                _mesa_hash_table_u64_remove(ctx->ssa_varyings, idx + 1);
        }
}

/* If there are leftovers after the below pass, emit actual fmov
 * instructions for the slow-but-correct path */

static void
emit_leftover_move(compiler_context *ctx)
{
        set_foreach(ctx->leftover_ssa_to_alias, leftover) {
                int base = ((uintptr_t) leftover->key) - 1;
                int mapped = base;

                map_ssa_to_alias(ctx, &mapped);
                EMIT(fmov, mapped, blank_alu_src, base);
        }
}

static void
actualise_ssa_to_alias(compiler_context *ctx)
{
        mir_foreach_instr(ctx, ins) {
                map_ssa_to_alias(ctx, &ins->ssa_args.src0);
                map_ssa_to_alias(ctx, &ins->ssa_args.src1);
        }

        emit_leftover_move(ctx);
}

/* Vertex shaders do not write gl_Position as is; instead, they write a
 * transformed screen space position as a varying. See section 12.5 "Coordinate
 * Transformation" of the ES 3.2 full specification for details.
 *
 * This transformation occurs early on, as NIR and prior to optimisation, in
 * order to take advantage of NIR optimisation passes of the transform itself.
 * */

static void
write_transformed_position(nir_builder *b, nir_src input_point_src, int uniform_no)
{
        nir_ssa_def *input_point = nir_ssa_for_src(b, input_point_src, 4);

        /* Get viewport from the uniforms */
        nir_intrinsic_instr *load;
        load = nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_uniform);
        load->num_components = 4;
        load->src[0] = nir_src_for_ssa(nir_imm_int(b, uniform_no));
        nir_ssa_dest_init(&load->instr, &load->dest, 4, 32, NULL);
        nir_builder_instr_insert(b, &load->instr);

        /* Formatted as <width, height, centerx, centery> */
        nir_ssa_def *viewport_vec4 = &load->dest.ssa;
        nir_ssa_def *viewport_width_2 = nir_channel(b, viewport_vec4, 0);
        nir_ssa_def *viewport_height_2 = nir_channel(b, viewport_vec4, 1);
        nir_ssa_def *viewport_offset = nir_channels(b, viewport_vec4, 0x8 | 0x4);

        /* XXX: From uniforms? */
        nir_ssa_def *depth_near = nir_imm_float(b, 0.0);
        nir_ssa_def *depth_far = nir_imm_float(b, 1.0);

        /* World space to normalised device coordinates */

        nir_ssa_def *w_recip = nir_frcp(b, nir_channel(b, input_point, 3));
        nir_ssa_def *ndc_point = nir_fmul(b, nir_channels(b, input_point, 0x7), w_recip);

        /* Normalised device coordinates to screen space */

        nir_ssa_def *viewport_multiplier = nir_vec2(b, viewport_width_2, viewport_height_2);
        nir_ssa_def *viewport_xy = nir_fadd(b, nir_fmul(b, nir_channels(b, ndc_point, 0x3), viewport_multiplier), viewport_offset);

        nir_ssa_def *depth_multiplier = nir_fmul(b, nir_fsub(b, depth_far, depth_near), nir_imm_float(b, 0.5f));
        nir_ssa_def *depth_offset     = nir_fmul(b, nir_fadd(b, depth_far, depth_near), nir_imm_float(b, 0.5f));
        nir_ssa_def *screen_depth     = nir_fadd(b, nir_fmul(b, nir_channel(b, ndc_point, 2), depth_multiplier), depth_offset);

        nir_ssa_def *screen_space = nir_vec4(b,
                                             nir_channel(b, viewport_xy, 0),
                                             nir_channel(b, viewport_xy, 1),
                                             screen_depth,
                                             nir_imm_float(b, 0.0));

        /* Finally, write out the transformed values to the varying */

        nir_intrinsic_instr *store;
        store = nir_intrinsic_instr_create(b->shader, nir_intrinsic_store_output);
        store->num_components = 4;
        nir_intrinsic_set_base(store, 0);
        nir_intrinsic_set_write_mask(store, 0xf);
        store->src[0].ssa = screen_space;
        store->src[0].is_ssa = true;
        store->src[1] = nir_src_for_ssa(nir_imm_int(b, 0));
        nir_builder_instr_insert(b, &store->instr);
}

static void
transform_position_writes(nir_shader *shader)
{
        nir_foreach_function(func, shader) {
                nir_foreach_block(block, func->impl) {
                        nir_foreach_instr_safe(instr, block) {
                                if (instr->type != nir_instr_type_intrinsic) continue;

                                nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
                                nir_variable *out = NULL;

                                switch (intr->intrinsic) {
                                case nir_intrinsic_store_output:
                                        /* already had i/o lowered.. lookup the matching output var: */
                                        nir_foreach_variable(var, &shader->outputs) {
                                                int drvloc = var->data.driver_location;

                                                if (nir_intrinsic_base(intr) == drvloc) {
                                                        out = var;
                                                        break;
                                                }
                                        }

                                        break;

                                default:
                                        break;
                                }

                                if (!out) continue;

                                if (out->data.mode != nir_var_shader_out)
                                        continue;

                                if (out->data.location != VARYING_SLOT_POS)
                                        continue;

                                nir_builder b;
                                nir_builder_init(&b, func->impl);
                                b.cursor = nir_before_instr(instr);

                                write_transformed_position(&b, intr->src[0], UNIFORM_VIEWPORT);
                                nir_instr_remove(instr);
                        }
                }
        }
}

static void
emit_fragment_epilogue(compiler_context *ctx)
{
        /* See the docs for why this works. TODO: gl_FragDepth */

        EMIT(alu_br_compact_cond, midgard_jmp_writeout_op_writeout, TAG_ALU_4, 0, midgard_condition_always);
        EMIT(alu_br_compact_cond, midgard_jmp_writeout_op_writeout, TAG_ALU_4, -1, midgard_condition_always);
}

/* For the blend epilogue, we need to convert the blended fragment vec4 (stored
 * in r0) to a RGBA8888 value by scaling and type converting. We then output it
 * with the int8 analogue to the fragment epilogue */

static void
emit_blend_epilogue(compiler_context *ctx)
{
        /* vmul.fmul.none.fulllow hr48, r0, #255 */

        midgard_instruction scale = {
                .type = TAG_ALU_4,
                .unit = UNIT_VMUL,
                .inline_constant = _mesa_float_to_half(255.0),
                .ssa_args = {
                        .src0 = SSA_FIXED_REGISTER(0),
                        .src1 = SSA_UNUSED_0,
                        .dest = SSA_FIXED_REGISTER(24),
                        .inline_constant = true
                },
                .alu = {
                        .op = midgard_alu_op_fmul,
                        .reg_mode = midgard_reg_mode_full,
                        .dest_override = midgard_dest_override_lower,
                        .mask = 0xFF,
                        .src1 = vector_alu_srco_unsigned(blank_alu_src),
                        .src2 = vector_alu_srco_unsigned(blank_alu_src),
                }
        };

        emit_mir_instruction(ctx, scale);

        /* vadd.f2u8.pos.low hr0, hr48, #0 */

        midgard_vector_alu_src alu_src = blank_alu_src;
        alu_src.half = true;

        midgard_instruction f2u8 = {
                .type = TAG_ALU_4,
                .ssa_args = {
                        .src0 = SSA_FIXED_REGISTER(24),
                        .src1 = SSA_UNUSED_0,
                        .dest = SSA_FIXED_REGISTER(0),
                        .inline_constant = true
                },
                .alu = {
                        .op = midgard_alu_op_f2u8,
                        .reg_mode = midgard_reg_mode_half,
                        .dest_override = midgard_dest_override_lower,
                        .outmod = midgard_outmod_pos,
                        .mask = 0xF,
                        .src1 = vector_alu_srco_unsigned(alu_src),
                        .src2 = vector_alu_srco_unsigned(blank_alu_src),
                }
        };

        emit_mir_instruction(ctx, f2u8);

        /* vmul.imov.quarter r0, r0, r0 */

        midgard_instruction imov_8 = {
                .type = TAG_ALU_4,
                .ssa_args = {
                        .src0 = SSA_UNUSED_1,
                        .src1 = SSA_FIXED_REGISTER(0),
                        .dest = SSA_FIXED_REGISTER(0),
                },
                .alu = {
                        .op = midgard_alu_op_imov,
                        .reg_mode = midgard_reg_mode_quarter,
                        .dest_override = midgard_dest_override_none,
                        .mask = 0xFF,
                        .src1 = vector_alu_srco_unsigned(blank_alu_src),
                        .src2 = vector_alu_srco_unsigned(blank_alu_src),
                }
        };

        /* Emit branch epilogue with the 8-bit move as the source */

        emit_mir_instruction(ctx, imov_8);
        EMIT(alu_br_compact_cond, midgard_jmp_writeout_op_writeout, TAG_ALU_4, 0, midgard_condition_always);

        emit_mir_instruction(ctx, imov_8);
        EMIT(alu_br_compact_cond, midgard_jmp_writeout_op_writeout, TAG_ALU_4, -1, midgard_condition_always);
}

static midgard_block *
emit_block(compiler_context *ctx, nir_block *block)
{
        midgard_block *this_block = malloc(sizeof(midgard_block));
        list_addtail(&this_block->link, &ctx->blocks);

        this_block->is_scheduled = false;
        ++ctx->block_count;

        ctx->texture_index[0] = -1;
        ctx->texture_index[1] = -1;

        /* Set up current block */
        list_inithead(&this_block->instructions);
        ctx->current_block = this_block;

        nir_foreach_instr(instr, block) {
                emit_instr(ctx, instr);
                ++ctx->instruction_count;
        }

        inline_alu_constants(ctx);
        embedded_to_inline_constant(ctx);

        /* Perform heavylifting for aliasing */
        actualise_ssa_to_alias(ctx);

        midgard_emit_store(ctx, this_block);
        midgard_eliminate_orphan_moves(ctx, this_block);
        midgard_pair_load_store(ctx, this_block);

        /* Append fragment shader epilogue (value writeout) */
        if (ctx->stage == MESA_SHADER_FRAGMENT) {
                if (block == nir_impl_last_block(ctx->func->impl)) {
                        if (ctx->is_blend)
                                emit_blend_epilogue(ctx);
                        else
                                emit_fragment_epilogue(ctx);
                }
        }

        /* Fallthrough save */
        this_block->next_fallthrough = ctx->previous_source_block;

        if (block == nir_start_block(ctx->func->impl))
                ctx->initial_block = this_block;

        if (block == nir_impl_last_block(ctx->func->impl))
                ctx->final_block = this_block;

        /* Allow the next control flow to access us retroactively, for
         * branching etc */
        ctx->current_block = this_block;

        /* Document the fallthrough chain */
        ctx->previous_source_block = this_block;

#ifdef MIR_DEBUG
        print_mir_block(this_block);
#endif

        return this_block;
}

static midgard_block *emit_cf_list(struct compiler_context *ctx, struct exec_list *list);

static void
emit_if(struct compiler_context *ctx, nir_if *nif)
{
        /* Conditional branches expect the condition in r31.w; emit a move for
         * that in the _previous_ block (which is the current block). */
        emit_condition(ctx, &nif->condition, true);

        /* Speculatively emit the branch, but we can't fill it in until later */
        EMIT(branch, true, true);
        midgard_instruction *then_branch = mir_last_in_block(ctx->current_block);

        /* Emit the two subblocks */
        midgard_block *then_block = emit_cf_list(ctx, &nif->then_list);

        /* Emit a jump from the end of the then block to the end of the else */
        EMIT(branch, false, false);
        midgard_instruction *then_exit = mir_last_in_block(ctx->current_block);

        /* Emit second block, and check if it's empty */

        int else_idx = ctx->block_count;
        int count_in = ctx->instruction_count;
        midgard_block *else_block = emit_cf_list(ctx, &nif->else_list);

        /* Now that we have the subblocks emitted, fix up the branches */

        assert(then_block);
        assert(else_block);


        if (ctx->instruction_count == count_in) {
                /* The else block is empty, so don't emit an exit jump */
                mir_remove_instruction(then_exit);
                then_branch->branch.target_block = else_idx + 1;
        } else {
                then_branch->branch.target_block = else_idx;
                then_exit->branch.target_block = else_idx + 1;
        }
}

static void
emit_loop(struct compiler_context *ctx, nir_loop *nloop)
{
        /* Remember where we are */
        midgard_block *start_block = ctx->current_block;

        /* Allocate a loop number for this. TODO: Nested loops. Instead of a
         * single current_loop variable, maybe we need a stack */

        int loop_idx = ++ctx->current_loop;

        /* Get index from before the body so we can loop back later */
        int start_idx = ctx->block_count;

        /* Emit the body itself */
        emit_cf_list(ctx, &nloop->body);

        /* Branch back to loop back */
        struct midgard_instruction br_back = v_branch(false, false);
        br_back.branch.target_block = start_idx;
        emit_mir_instruction(ctx, br_back);

        /* Find the index of the block about to follow us (note: we don't add
         * one; blocks are 0-indexed so we get a fencepost problem) */
        int break_block_idx = ctx->block_count;

        /* Fix up the break statements we emitted to point to the right place,
         * now that we can allocate a block number for them */

        list_for_each_entry_from(struct midgard_block, block, start_block, &ctx->blocks, link) {
                print_mir_block(block);
                mir_foreach_instr_in_block(block, ins) {
                        if (ins->type != TAG_ALU_4) continue;
                        if (!ins->compact_branch) continue;
                        if (ins->prepacked_branch) continue;

                        /* We found a branch -- check the type to see if we need to do anything */
                        if (ins->branch.target_type != TARGET_BREAK) continue;

                        /* It's a break! Check if it's our break */
                        if (ins->branch.target_break != loop_idx) continue;

                        /* Okay, cool, we're breaking out of this loop.
                         * Rewrite from a break to a goto */

                        ins->branch.target_type = TARGET_GOTO;
                        ins->branch.target_block = break_block_idx;
                }
        }
}

static midgard_block *
emit_cf_list(struct compiler_context *ctx, struct exec_list *list)
{
        midgard_block *start_block = NULL;

        foreach_list_typed(nir_cf_node, node, node, list) {
                switch (node->type) {
                case nir_cf_node_block: {
                        midgard_block *block = emit_block(ctx, nir_cf_node_as_block(node));

                        if (!start_block)
                                start_block = block;

                        break;
                }

                case nir_cf_node_if:
                        emit_if(ctx, nir_cf_node_as_if(node));
                        break;

                case nir_cf_node_loop:
                        emit_loop(ctx, nir_cf_node_as_loop(node));
                        break;

                case nir_cf_node_function:
                        assert(0);
                        break;
                }
        }

        return start_block;
}

int
midgard_compile_shader_nir(nir_shader *nir, midgard_program *program, bool is_blend)
{
        struct util_dynarray *compiled = &program->compiled;

        compiler_context ictx = {
                .nir = nir,
                .stage = nir->info.stage,

                .is_blend = is_blend,
                .blend_constant_offset = -1,
        };

        compiler_context *ctx = &ictx;

        /* TODO: Decide this at runtime */
        ctx->uniform_cutoff = 8;

        switch (ctx->stage) {
        case MESA_SHADER_VERTEX:
                ctx->special_uniforms = 1;
                break;

        default:
                ctx->special_uniforms = 0;
                break;
        }

        /* Append epilogue uniforms if necessary. The cmdstream depends on
         * these being at the -end-; see assign_var_locations. */

        if (ctx->stage == MESA_SHADER_VERTEX) {
                nir_variable_create(nir, nir_var_uniform, glsl_vec4_type(), "viewport");
        }

        /* Assign var locations early, so the epilogue can use them if necessary */

        nir_assign_var_locations(&nir->outputs, &nir->num_outputs, glsl_type_size);
        nir_assign_var_locations(&nir->inputs, &nir->num_inputs, glsl_type_size);
        nir_assign_var_locations(&nir->uniforms, &nir->num_uniforms, glsl_type_size);

        /* Initialize at a global (not block) level hash tables */

        ctx->ssa_constants = _mesa_hash_table_u64_create(NULL);
        ctx->ssa_varyings = _mesa_hash_table_u64_create(NULL);
        ctx->ssa_to_alias = _mesa_hash_table_u64_create(NULL);
        ctx->ssa_to_register = _mesa_hash_table_u64_create(NULL);
        ctx->hash_to_temp = _mesa_hash_table_u64_create(NULL);
        ctx->leftover_ssa_to_alias = _mesa_set_create(NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);

        /* Assign actual uniform location, skipping over samplers */

        ctx->uniform_nir_to_mdg = _mesa_hash_table_u64_create(NULL);

        nir_foreach_variable(var, &nir->uniforms) {
                if (glsl_get_base_type(var->type) == GLSL_TYPE_SAMPLER) continue;

                unsigned length = glsl_get_length(var->type);

                if (!length) {
                        length = glsl_get_matrix_columns(var->type);
                }

                for (int col = 0; col < length; ++col) {
                        int id = ctx->uniform_count++;
                        _mesa_hash_table_u64_insert(ctx->uniform_nir_to_mdg, var->data.driver_location + col + 1, (void *) ((uintptr_t) (id + 1)));
                }
        }

        if (ctx->stage == MESA_SHADER_VERTEX) {
                ctx->varying_nir_to_mdg = _mesa_hash_table_u64_create(NULL);
                ctx->varying_count = 1; /* Offset away from gl_Position */

                nir_foreach_variable(var, &nir->outputs) {
                        if (var->data.location < VARYING_SLOT_VAR0) {
                                if (var->data.location == VARYING_SLOT_POS) {
                                        _mesa_hash_table_u64_insert(ctx->varying_nir_to_mdg, var->data.driver_location + 1, (void *) ((uintptr_t) (1)));
                                        continue;
                                }
                        }

                        for (int col = 0; col < glsl_get_matrix_columns(var->type); ++col) {
                                int id = ctx->varying_count++;
                                _mesa_hash_table_u64_insert(ctx->varying_nir_to_mdg, var->data.driver_location + col + 1, (void *) ((uintptr_t) (id + 1)));
                        }
                }
        }



        /* Lower vars -- not I/O -- before epilogue */

        NIR_PASS_V(nir, nir_lower_var_copies);
        NIR_PASS_V(nir, nir_lower_vars_to_ssa);
        NIR_PASS_V(nir, nir_split_var_copies);
        NIR_PASS_V(nir, nir_lower_var_copies);
        NIR_PASS_V(nir, nir_lower_global_vars_to_local);
        NIR_PASS_V(nir, nir_lower_var_copies);
        NIR_PASS_V(nir, nir_lower_vars_to_ssa);
        NIR_PASS_V(nir, nir_lower_io, nir_var_all, glsl_type_size, 0);

        /* Append vertex epilogue before optimisation, so the epilogue itself
         * is optimised */

        if (ctx->stage == MESA_SHADER_VERTEX)
                transform_position_writes(nir);

        /* Optimisation passes */

        optimise_nir(nir);

#ifdef NIR_DEBUG
        nir_print_shader(nir, stdout);
#endif

        /* Assign counts, now that we're sure (post-optimisation) */
        program->uniform_count = nir->num_uniforms;

        program->attribute_count = (ctx->stage == MESA_SHADER_VERTEX) ? nir->num_inputs : 0;
        program->varying_count = (ctx->stage == MESA_SHADER_VERTEX) ? nir->num_outputs : ((ctx->stage == MESA_SHADER_FRAGMENT) ? nir->num_inputs : 0);


        nir_foreach_function(func, nir) {
                if (!func->impl)
                        continue;

                list_inithead(&ctx->blocks);
                ctx->block_count = 0;
                ctx->func = func;

                emit_cf_list(ctx, &func->impl->body);
                emit_block(ctx, func->impl->end_block);

                break; /* TODO: Multi-function shaders */
        }

        util_dynarray_init(compiled, NULL);

        /* Schedule! */
        schedule_program(ctx);

        /* Now that all the bundles are scheduled and we can calculate block
         * sizes, emit actual branch instructions rather than placeholders */

        int br_block_idx = 0;

        mir_foreach_block(ctx, block) {
                util_dynarray_foreach(&block->bundles, midgard_bundle, bundle) {
                        for (int c = 0; c < bundle->instruction_count; ++c) {
                                midgard_instruction *ins = &bundle->instructions[c];

                                if (ins->unit != ALU_ENAB_BR_COMPACT) continue;

                                if (ins->prepacked_branch) continue;

                                uint16_t compact;

                                /* Determine the block we're jumping to */
                                int target_number = ins->branch.target_block;

                                midgard_block *target = mir_get_block(ctx, target_number);
                                assert(target);

                                /* Determine the destination tag */
                                midgard_bundle *first = util_dynarray_element(&target->bundles, midgard_bundle, 0);
                                assert(first);

                                int dest_tag = first->tag;

                                /* Count up the number of quadwords we're jumping over. That is, the number of quadwords in each of the blocks between (br_block_idx, target_number) */
                                int quadword_offset = 0;

                                if (target_number > br_block_idx) {
                                        /* Jump forward */

                                        for (int idx = br_block_idx + 1; idx < target_number; ++idx) {
                                                midgard_block *blk = mir_get_block(ctx, idx);
                                                assert(blk);

                                                quadword_offset += blk->quadword_count;
                                        }
                                } else {
                                        /* Jump backwards */

                                        for (int idx = br_block_idx; idx >= target_number; --idx) {
                                                midgard_block *blk = mir_get_block(ctx, idx);
                                                assert(blk);

                                                quadword_offset -= blk->quadword_count;
                                        }
                                }

                                if (ins->branch.conditional) {
                                        midgard_branch_cond branch = {
                                                .op = midgard_jmp_writeout_op_branch_cond,
                                                .dest_tag = dest_tag,
                                                .offset = quadword_offset,
                                                .cond = ins->branch.invert_conditional ? midgard_condition_false : midgard_condition_true
                                        };

                                        memcpy(&compact, &branch, sizeof(branch));
                                } else {
                                        midgard_branch_uncond branch = {
                                                .op = midgard_jmp_writeout_op_branch_uncond,
                                                .dest_tag = dest_tag,
                                                .offset = quadword_offset,
                                                .unknown = 1
                                        };

                                        memcpy(&compact, &branch, sizeof(branch));
                                }

                                /* Swap in the generic branch for our actual branch */
                                ins->unit = ALU_ENAB_BR_COMPACT;
                                ins->br_compact = compact;
                        }

                }

                ++br_block_idx;
        }

        /* Emit flat binary from the instruction arrays. Iterate each block in
         * sequence. Save instruction boundaries such that lookahead tags can
         * be assigned easily */

        /* Cache _all_ bundles in source order for lookahead across failed branches */

        int bundle_count = 0;
        mir_foreach_block(ctx, block) {
                bundle_count += block->bundles.size / sizeof(midgard_bundle);
        }
        midgard_bundle **source_order_bundles = malloc(sizeof(midgard_bundle *) * bundle_count);
        int bundle_idx = 0;
        mir_foreach_block(ctx, block) {
                util_dynarray_foreach(&block->bundles, midgard_bundle, bundle) {
                        source_order_bundles[bundle_idx++] = bundle;
                }
        }

        int current_bundle = 0;

        mir_foreach_block(ctx, block) {
                util_dynarray_foreach(&block->bundles, midgard_bundle, bundle) {
                        int lookahead = 1;

                        if (current_bundle + 1 < bundle_count) {
                                uint8_t next = source_order_bundles[current_bundle + 1]->tag;

                                if (!(current_bundle + 2 < bundle_count) && IS_ALU(next)) {
                                        lookahead = 1;
                                } else {
                                        lookahead = next;
                                }
                        }

                        emit_binary_bundle(ctx, bundle, compiled, lookahead);
                        ++current_bundle;
                }

                /* TODO: Free deeper */
                //util_dynarray_fini(&block->instructions);
        }

        free(source_order_bundles);

        /* Due to lookahead, we need to report in the command stream the first
         * tag executed. An initial block might be empty, so iterate until we
         * find one that 'works' */

        midgard_block *initial_block = list_first_entry(&ctx->blocks, midgard_block, link);

        program->first_tag = 0;

        do {
                midgard_bundle *initial_bundle = util_dynarray_element(&initial_block->bundles, midgard_bundle, 0);

                if (initial_bundle) {
                        program->first_tag = initial_bundle->tag;
                        break;
                }

                /* Initial block is empty, try the next block */
                initial_block = list_first_entry(&(initial_block->link), midgard_block, link);
        } while(initial_block != NULL);

        /* Make sure we actually set the tag */
        assert(program->first_tag);

        /* Deal with off-by-one related to the fencepost problem */
        program->work_register_count = ctx->work_registers + 1;

        program->can_discard = ctx->can_discard;
        program->uniform_cutoff = ctx->uniform_cutoff;

        program->blend_patch_offset = ctx->blend_constant_offset;

#ifdef MDG_DEBUG
        disassemble_midgard(program->compiled.data, program->compiled.size);
#endif

        return 0;
}
