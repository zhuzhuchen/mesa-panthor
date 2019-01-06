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

#include "compiler/glsl/standalone.h"
#include "compiler/glsl/glsl_to_nir.h"
#include "compiler/nir_types.h"
#include "bifrost_compile.h"
#include "disassemble.h"
#include "util/u_dynarray.h"
#include "main/mtypes.h"

static void
compile_shader(char **argv)
{
        struct gl_shader_program *prog;
        nir_shader *nir;

        struct standalone_options options = {
                .glsl_version = 140,
                .do_link = true,
        };

        prog = standalone_compile_shader(&options, 2, argv);
        prog->_LinkedShaders[MESA_SHADER_FRAGMENT]->Program->info.stage = MESA_SHADER_FRAGMENT;

        struct bifrost_program compiled;
        nir = glsl_to_nir(prog, MESA_SHADER_VERTEX, &bifrost_nir_options);
        bifrost_compile_shader_nir(nir, &compiled);

        nir = glsl_to_nir(prog, MESA_SHADER_FRAGMENT, &bifrost_nir_options);
        bifrost_compile_shader_nir(nir, &compiled);
}

static void
disassemble(const char *filename)
{
        FILE *fp = fopen(filename, "rb");
        assert(fp);

        fseek(fp, 0, SEEK_END);
        int filesize = ftell(fp);
        rewind(fp);

        unsigned char *code = malloc(filesize);
        int res = fread(code, 1, filesize, fp);
        if (res != filesize) {
                printf("Couldn't read full file\n");
        }
        fclose(fp);

        disassemble_bifrost(code, filesize);
        free(code);
}

int
main(int argc, char **argv)
{
        if (argc < 2) {
                printf("Pass a command\n");
                exit(1);
        }
        if (strcmp(argv[1], "compile") == 0) {
                compile_shader(&argv[2]);
        } else if (strcmp(argv[1], "disasm") == 0) {
                disassemble(argv[2]);
        }
        return 0;
}
