#include "ir_defines.h"
#include "ir_printer.h"

void
print_mir_instruction(struct bifrost_instruction *ins)
{
        // XXX: prettify this
        printf("\t%d ", ins->add.op);

        printf("<%d %d %d %d>",
                ins->args.dest,
                ins->args.src0,
                ins->args.src1,
                ins->args.src2);

        printf("\n");
}

void
print_mir_block(struct bifrost_block *block)
{
        printf("{\n");

        mir_foreach_instr_in_block(block, instr) {
                print_mir_instruction(instr);
        }

        printf("}\n");
}


