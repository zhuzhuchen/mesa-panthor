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

#include "compiler/glsl/glsl_to_nir.h"
#include "compiler/nir_types.h"
#include "main/imports.h"
#include "compiler/nir/nir_builder.h"
#include "util/half_float.h"
#include "util/register_allocate.h"
#include "util/u_dynarray.h"
#include "util/list.h"
#include "main/mtypes.h"

#include "bifrost.h"
#include "bifrost_compile.h"
#include "ir_defines.h"
#include "ir_printer.h"

static int
glsl_type_size(const struct glsl_type *type)
{
        return glsl_count_attribute_slots(type, false);
}

static void
optimize_nir(nir_shader *nir)
{
        bool progress;

        NIR_PASS(progress, nir, nir_lower_regs_to_ssa);

        do {
                progress = false;

                NIR_PASS(progress, nir, nir_lower_io, nir_var_all, glsl_type_size, 0);
                // XXX: We want to remove this in the future since Bifrost can load these in a single instruction
                NIR_PASS_V(nir, nir_lower_io_to_scalar, nir_var_all);
                NIR_PASS(progress, nir, nir_lower_var_copies);
                NIR_PASS(progress, nir, nir_lower_vars_to_ssa);

                NIR_PASS(progress, nir, nir_copy_prop);
                NIR_PASS(progress, nir, nir_opt_dce);
                NIR_PASS(progress, nir, nir_lower_alu_to_scalar);
                NIR_PASS(progress, nir, nir_lower_load_const_to_scalar);
                NIR_PASS(progress, nir, nir_lower_phis_to_scalar);
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


static struct bifrost_instruction *
mir_alloc_ins(struct bifrost_instruction ins)
{
        struct bifrost_instruction *heap_ins = malloc(sizeof(ins));
        memcpy(heap_ins, &ins, sizeof(ins));
        return heap_ins;
}

static struct bifrost_instruction*
mir_next_op(struct bifrost_instruction *ins)
{
        return list_first_entry(&(ins->link), struct bifrost_instruction, link);
}

static void
emit_mir_instruction(struct compiler_context *ctx, struct bifrost_instruction ins)
{
        list_addtail(&(mir_alloc_ins(ins))->link, &ctx->current_block->instructions);
}

static void
bifrost_pin_output(struct compiler_context *ctx, int index, int reg)
{
        _mesa_hash_table_u64_insert(ctx->ssa_to_register, index + 1, (void *) ((uintptr_t) reg + 1));
}

#define M_LOAD_UBO(_tag, name, rname, uname) \
	static struct bifrost_instruction m_##name(unsigned ssa, unsigned location, unsigned binding) { \
		struct bifrost_instruction i = { \
			.type = TAG_LOAD_STORE_UBO_##_tag, \
			.args = { \
				.rname = ssa, \
				.uname = location, \
                                .src1 = binding, \
                                .src2 = ~0U \
			}, \
			.add = { \
				.op = bifrost_add_op_##name, \
			} \
		}; \
		\
		return i; \
	}

#define M_LOAD_ATTR(name, rname, uname) \
	static struct bifrost_instruction m_##name(unsigned ssa, unsigned location) { \
		struct bifrost_instruction i = { \
			.type = TAG_LOAD_STORE_UBO_4, \
			.args = { \
				.rname = ssa, \
				.uname = ~0U, \
                                .src1 = ~0U, \
                                .src2 = ~0U \
			}, \
			.add = { \
				.op = bifrost_add_op_##name | ((location & 0xf) << 3), \
			} \
		}; \
		\
		return i; \
	}

#define M_STORE_VARY(name, rname, uname) \
	static struct bifrost_instruction m_##name(unsigned ssa, unsigned location) { \
		struct bifrost_instruction i = { \
			.type = TAG_LOAD_STORE_UBO_4, \
			.args = { \
				.rname = ssa, \
				.uname = location, \
                                .src2 = ~0U, \
                                .dest = ~0U, \
			}, \
			.add = { \
				.op = bifrost_add_op_##name, \
			} \
		}; \
		\
		return i; \
	}

#define M_ALU_OP(name, rname, uname) \
	static struct bifrost_instruction m_##name(unsigned ssa) { \
		struct bifrost_instruction i = { \
			.type = TAG_FMA_OP, \
			.args = { \
				.rname = ssa, \
				.uname = ~0U, \
                                .src1 = ~0U, \
                                .src2 = ~0U \
			}, \
			.fma = { \
				.op = bifrost_fma_op_##name, \
			} \
		}; \
		\
		return i; \
	}
//M_ALU_OP(fma_f32, dest, src0)
M_LOAD_UBO(1, ld_ubo_i32, dest, src0)
//M_LOAD_UBO(2, ld_ubo_v2i32, dest, src0)
//M_LOAD_UBO(3, ld_ubo_v3i32, dest, src0)
//M_LOAD_UBO(4, ld_ubo_v4i32, dest, src0)
M_LOAD_ATTR(ld_attr_i32, dest, src0)
//M_LOAD_ATTR(ld_attr_v4i32, dest, src0)
M_STORE_VARY(st_vary_v1, src0, src1)
// M_STORE_VARY(st_vary_v2, src0, src1)
// M_STORE_VARY(st_vary_v3, src0, src1)
// M_STORE_VARY(st_vary_v4, src0, src1)

static void
emit_load_const(struct compiler_context *ctx, nir_load_const_instr *instr)
{
        nir_ssa_def def = instr->def;

        float *v = ralloc_array(NULL, float, 1);
        memcpy(v, &instr->value.f32[0], sizeof(float));
        _mesa_hash_table_u64_insert(ctx->ssa_constants, def.index + 1, v);
}

static void
emit_intrinsic(struct compiler_context *ctx, nir_intrinsic_instr *instr)
{
        nir_const_value *const_offset;
        unsigned offset, reg;

        switch (instr->intrinsic) {
        case nir_intrinsic_load_uniform:
                const_offset = nir_src_as_const_value(instr->src[0]);
                assert (const_offset && "no indirect inputs");

                offset = nir_intrinsic_base(instr) + const_offset->u32[0];

                reg = nir_dest_index(&instr->dest);
                unsigned uniform_offset = 0;

                void *entry = _mesa_hash_table_u64_search(ctx->uniform_nir_to_bi, offset + 1);

                /* XXX */
                if (!entry) {
                        printf("WARNING: Unknown uniform %d\n", offset);
                        break;
                }

                uniform_offset = (uintptr_t) (entry) - 1;

                // XXX
                struct bifrost_instruction ins = m_ld_ubo_i32(reg, uniform_offset, ~0U);

                emit_mir_instruction(ctx, ins);

                break;
        case nir_intrinsic_store_output:
                const_offset = nir_src_as_const_value(instr->src[1]);
                assert(const_offset && "no indirect outputs");

                offset = nir_intrinsic_base(instr) + const_offset->u32[0];


                if (ctx->stage == MESA_SHADER_FRAGMENT) {
                        /* Source color preloaded to r0 */
                        reg = nir_src_index(&instr->src[0]);
                        bifrost_pin_output(ctx, reg, 0);
                }
                else if (ctx->stage == MESA_SHADER_VERTEX) {
                        int comp = nir_intrinsic_component(instr);
                        offset += comp;
                        void *entry = _mesa_hash_table_u64_search(ctx->varying_nir_to_bi, offset + 1);

                        if (!entry) {
                                printf("WARNING: skipping varying\n");
                                break;
                        }

                        offset = (uintptr_t) (entry) - 1;

                        reg = nir_src_index(&instr->src[0]);
                        struct bifrost_instruction ins = m_st_vary_v1(offset, reg);
                        emit_mir_instruction(ctx, ins);

                } else {
                        printf("Unknown store\n");
                        assert(0);
                }
                break;
        case nir_intrinsic_load_input: {
                const_offset = nir_src_as_const_value(instr->src[0]);
                assert (const_offset && "no indirect inputs");

                offset = nir_intrinsic_base(instr) + const_offset->u32[0];

                reg = nir_dest_index(&instr->dest);

                struct bifrost_instruction ins = m_ld_attr_i32(reg, offset);

                emit_mir_instruction(ctx, ins);
                break;
        }
        case nir_intrinsic_discard_if:
        case nir_intrinsic_discard:
                printf ("Unhandled intrinsic discard\n");
                break;
        default:
                printf ("Unhandled intrinsic\n");
                assert(0);
                break;
        }
}

#define ALU_FMA_CASE(_arguments, nir, name) \
	case nir_op_##nir: \
                arguments = _arguments; \
		op = bifrost_fma_op_##name; \
		break;

static void
emit_alu(struct compiler_context *ctx, nir_alu_instr *instr)
{
        unsigned dest = nir_dest_index(&instr->dest.dest);

        unsigned op = ~0U, arguments;
        switch (instr->op) {
        ALU_FMA_CASE(2, fmul, fma_f32)
        default:
                printf("Unhandled ALU op %s\n", nir_op_infos[instr->op].name);
                assert(0);
                return;
        }

        if (op == ~0U) return;


        unsigned src0 = nir_alu_src_index(&instr->src[0]);
        unsigned src1 = arguments == 2 ? nir_alu_src_index(&instr->src[1]) : ~0;

        struct bifrost_instruction ins = {
                .type = TAG_FMA_OP,
                .args = {
                        .src0 = src0,
                        .src1 = src1,
                        .src2 = ~0U,
                        .dest = dest,
                }
        };

        emit_mir_instruction(ctx, ins);
}

static void
emit_instr(struct compiler_context *ctx, struct nir_instr *instr)
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
                printf("Unhandled tex type\n");
                break;
        case nir_instr_type_jump:
                printf("Unhandled jump type\n");
                break;
        case nir_instr_type_ssa_undef:
                printf("Unhandled undef type\n");
                break;
        default:
                printf("Unhandled instruction type\n");
                break;
        }
}


static struct bifrost_block *
emit_block(struct compiler_context *ctx, nir_block *block)
{
        struct bifrost_block *this_block = malloc(sizeof(struct bifrost_block));
        list_addtail(&this_block->link, &ctx->blocks);

        ++ctx->block_count;

        /* Set up current block */
        list_inithead(&this_block->instructions);
        ctx->current_block = this_block;

        nir_foreach_instr(instr, block) {
                emit_instr(ctx, instr);
                ++ctx->instruction_count;
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

        return this_block;
}


static struct bifrost_block *
emit_cf_list(struct compiler_context *ctx, struct exec_list *list)
{
        struct bifrost_block *start_block = NULL;

        foreach_list_typed(nir_cf_node, node, node, list) {
                switch (node->type) {
                case nir_cf_node_block: {
                        struct bifrost_block *block = emit_block(ctx, nir_cf_node_as_block(node));

                        if (!start_block)
                                start_block = block;

                        break;
                }
                default:
                case nir_cf_node_if:
                case nir_cf_node_loop:
                case nir_cf_node_function:
                        assert(0);
                        break;
                }
        }

        return start_block;
}

static unsigned
find_or_allocate_temp(struct compiler_context *ctx, unsigned hash)
{
        if ((hash >= SSA_FIXED_MINIMUM))
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

static void
allocate_registers(struct compiler_context *ctx)
{
        /* First, initialize the RA */
        struct ra_regs *regs = ra_alloc_reg_set(NULL, 64, true);

        /* Create a primary (general purpose) class, as well as special purpose
         * pipeline register classes */

        int primary_class = ra_alloc_reg_class(regs);

        /* Add the full set of work registers */
        for (int i = 0; i < 64; ++i)
                ra_class_add_reg(regs, primary_class, i);

        /* We're done setting up */
        ra_set_finalize(regs, NULL);

        /* Transform the MIR into squeezed index form */
        mir_foreach_block(ctx, block) {
                mir_foreach_instr_in_block(block, ins) {

                        ins->args.src0 = find_or_allocate_temp(ctx, ins->args.src0);
                        ins->args.src1 = find_or_allocate_temp(ctx, ins->args.src1);
                        ins->args.src2 = find_or_allocate_temp(ctx, ins->args.src2);
                        ins->args.dest = find_or_allocate_temp(ctx, ins->args.dest);
                }

                print_mir_block(block);
        }

}

/* Schedules, but does not emit, a single basic block. After scheduling, the
 * final tag and size of the block are known, which are necessary for branching
 * */

static struct bifrost_clause
schedule_clause(struct compiler_context *ctx, struct bifrost_block *block, struct bifrost_instruction *ins, int *skip) {
        int instructions_emitted = 0, instructions_consumed = -1;
        struct bifrost_clause clause = {};

        /* Copy the instructions into the clause */
        clause.instruction_count = instructions_emitted + 1;

        struct bifrost_instruction *uins = ins;
        util_dynarray_init(&clause.instructions, NULL);
        for (int i = 0; i < clause.instruction_count; ++i) {
                util_dynarray_append(&clause.instructions, struct bifrost_instruction, *uins);
                uins = mir_next_op(uins);
        }

        *skip = (instructions_consumed == -1) ? instructions_emitted : instructions_consumed;

        // XXX: Fill out the header with register information

        return clause;
}

static void
schedule_block(struct compiler_context *ctx, struct bifrost_block *block)
{
        util_dynarray_init(&block->clauses, NULL);

        mir_foreach_instr_in_block(block, ins) {
                int skip;
                struct bifrost_clause clause = schedule_clause(ctx, block, ins, &skip);
                util_dynarray_append(&block->clauses, struct bifrost_clause, clause);

                while(skip--)
                        ins = mir_next_op(ins);
        }
}

static void
schedule_program(struct compiler_context *ctx)
{
        allocate_registers(ctx);

        mir_foreach_block(ctx, block) {
                schedule_block(ctx, block);
        }
}


int
bifrost_compile_shader_nir(nir_shader *nir, struct bifrost_program *program) {
        struct compiler_context ictx = {
                .nir = nir,
                .stage = nir->info.stage,
        };

        struct compiler_context *ctx = &ictx;

        /* Assign var locations early, so the epilogue can use them if necessary */

        nir_assign_var_locations(&nir->outputs, &nir->num_outputs, glsl_type_size);
        nir_assign_var_locations(&nir->inputs, &nir->num_inputs, glsl_type_size);
        nir_assign_var_locations(&nir->uniforms, &nir->num_uniforms, glsl_type_size);

        /* Initialize at a global (not block) level hash tables */
        ctx->ssa_constants = _mesa_hash_table_u64_create(NULL);
        ctx->ssa_to_register = _mesa_hash_table_u64_create(NULL);
        ctx->hash_to_temp = _mesa_hash_table_u64_create(NULL);

        /* Assign actual uniform location, skipping over samplers */
        ctx->uniform_nir_to_bi  = _mesa_hash_table_u64_create(NULL);

        nir_foreach_variable(var, &nir->uniforms) {
                if (glsl_get_base_type(var->type) == GLSL_TYPE_SAMPLER) continue;

                for (int col = 0; col < glsl_get_matrix_columns(var->type); ++col) {
                        int id = ctx->uniform_count++;
                        _mesa_hash_table_u64_insert(ctx->uniform_nir_to_bi, var->data.driver_location + col + 1, (void *) ((uintptr_t) (id + 1)));
                }
        }

        if (ctx->stage == MESA_SHADER_VERTEX) {
                ctx->varying_nir_to_bi = _mesa_hash_table_u64_create(NULL);

                nir_foreach_variable(var, &nir->outputs) {
                        if (var->data.location < VARYING_SLOT_VAR0) {
                                if (var->data.location == VARYING_SLOT_POS)
                                        _mesa_hash_table_u64_insert(ctx->varying_nir_to_bi, var->data.driver_location + 1, (void *) ((uintptr_t) (1)));

                                continue;
                        }

                        for (int col = 0; col < glsl_get_matrix_columns(var->type); ++col) {
                                for (int comp = 0; comp < 4; ++comp) {
                                        int id = comp + ctx->varying_count++;
                                        _mesa_hash_table_u64_insert(ctx->varying_nir_to_bi, var->data.driver_location + col + comp + 1, (void *) ((uintptr_t) (id + 1)));
                                }
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

        /* Optimisation passes */
        optimize_nir(nir);

        nir_print_shader(nir, stdout);

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

        /* Schedule! */
        schedule_program(ctx);

        return 0;
}

