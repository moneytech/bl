//************************************************************************************************
// bl
//
// File:   mir_writer.c
// Author: Martin Dorazil
// Date:   20.12.18
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

#include "stages.h"
#include "assembly.h"
#include "bldebug.h"
#include "error.h"
#include "mir_printer.h"

void
mir_writer_run(Assembly *assembly)
{
  assert(assembly->mir_module);

  char *export_file = malloc(sizeof(char) * (strlen(assembly->name) + 4));
  if (!export_file) bl_abort("bad alloc");
  strcpy(export_file, assembly->name);
  strcat(export_file, ".blm");

  FILE *f = fopen(export_file, "w");
  if (f == NULL) {
    msg_error("cannot open file %s", export_file);
    free(export_file);
    return;
  }

  mir_print_module(assembly->mir_module, f);
  
  fclose(f);
  msg_log("mir code written into " GREEN("%s"), export_file);

  free(export_file);
}
