//************************************************************************************************
// bl
//
// File:   obj_writer.c
// Author: Martin Dorazil
// Date:   28/02/2018
//
// Copyright 2018 Martin Dorazil
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//************************************************************************************************

#include "llvm_api.h"
#include "common.h"
#include "error.h"
#include "stages.h"

#ifdef BL_PLATFORM_WIN
#define OBJ_EXT ".obj"
#else
#define OBJ_EXT ".o"
#endif

/* Emit assembly object file. */
void
obj_writer_run(Assembly *assembly)
{
	char *filename = bl_malloc(sizeof(char) * (strlen(assembly->name) + strlen(OBJ_EXT) + 1));
	if (!filename) BL_ABORT("bad alloc");
	strcpy(filename, assembly->name);
	strcat(filename, OBJ_EXT);

	char *error_msg = NULL;
	remove(filename);
	if (LLVMTargetMachineEmitToFile(
	        assembly->llvm.TM, assembly->llvm.module, filename, LLVMObjectFile, &error_msg)) {
		msg_error("Cannot emit object file: %s with error: %s", filename, error_msg);

		LLVMDisposeMessage(error_msg);
		bl_free(filename);
		return;
	}

	bl_free(filename);
}
