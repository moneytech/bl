//************************************************************************************************
// blc
//
// File:   builder.c
// Author: Martin Dorazil
// Date:   14.2.18
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

#include <stdarg.h>
#include <time.h>
#include <bobject/containers/string.h>

#include "common.h"
#include "builder.h"
#include "assembly.h"
#include "unit.h"
#include "stages.h"
#include "token.h"

#define MAX_MSG_LEN 1024
#define MAX_ERROR_REPORTED 10

static int
compile_unit(Builder *builder, Unit *unit, Assembly *assembly, uint32_t flags);

static int
compile_assembly(Builder *builder, Assembly *assembly, uint32_t flags);

static void
default_error_handler(const char *msg, void *context)
{
#if BL_ABORT_ON_CMP_ERROR
  bl_abort(false, "%s", msg);
#else
  msg_error("%s", msg);
#endif
}

static void
default_warning_handler(const char *msg, void *context)
{
  msg_warning("%s", msg);
}

static bool llvm_initialized = false;

static void
llvm_init(void)
{
  if (llvm_initialized) return;

  LLVMInitializeAllTargetInfos();
  LLVMInitializeAllTargets();
  LLVMInitializeAllTargetMCs();
  LLVMInitializeAllAsmParsers();
  LLVMInitializeAllAsmPrinters();
  LLVMLinkInMCJIT();
  llvm_initialized = true;
}

#define interrupt_on_error(_builder)                                                               \
  if ((_builder)->errorc) return COMPILE_FAIL;

int
compile_unit(Builder *builder, Unit *unit, Assembly *assembly, uint32_t flags)
{
  // bl_msg_log("processing unit: %s", unit->name);

  if (flags & BUILDER_LOAD_FROM_FILE) {
    file_loader_run(builder, unit);
    interrupt_on_error(builder);
  }

  lexer_run(builder, unit);
  interrupt_on_error(builder);

  if (flags & BUILDER_PRINT_TOKENS) {
    token_printer_run(unit);
    interrupt_on_error(builder);
  }

  parser_run(builder, assembly, unit);

  return COMPILE_OK;
}

int
compile_assembly(Builder *builder, Assembly *assembly, uint32_t flags)
{
  if (!builder->errorc) checker_run(builder, assembly);
  if (!builder->errorc) post_run(builder, assembly);

  if (flags & BUILDER_PRINT_AST) {
    ast_printer_run(assembly);
  }
  interrupt_on_error(builder);

  if (!(flags & BUILDER_SYNTAX_ONLY)) {
    ir_run(builder, assembly);

    if (flags & BUILDER_EMIT_LLVM) {
      bc_writer_run(builder, assembly);
      interrupt_on_error(builder);
    }

    if (flags & BUILDER_RUN) jit_exec_run(builder, assembly);

    if (!(flags & BUILDER_NO_BIN)) {
      linker_run(builder, assembly);
      interrupt_on_error(builder);
      native_bin_run(builder, assembly);
      interrupt_on_error(builder);
    }
  }

  return COMPILE_OK;
}

/* public */
Builder *
builder_new(void)
{
  Builder *builder = bl_calloc(1, sizeof(Builder));
  if (!builder) bl_abort("bad alloc");

  builder->on_error   = default_error_handler;
  builder->on_warning = default_warning_handler;
  builder->no_warn    = false;
  builder->errorc     = 0;

  llvm_init();

  return builder;
}

void
builder_delete(Builder *builder)
{
  bl_free(builder);
}

int
builder_compile(Builder *builder, Assembly *assembly, uint32_t flags)
{
  clock_t begin = clock();
  Unit *unit;
  int     state = COMPILE_OK;

  builder->no_warn = flags & BUILDER_NO_WARN;
  msg_log("compile assembly: %s", assembly->name);

  barray_foreach(assembly->units, unit)
  {
    /* IDEA: can run in separate thread */
    if ((state = compile_unit(builder, unit, assembly, flags)) != COMPILE_OK) {
      break;
    }
  }

  if (state == COMPILE_OK) state = compile_assembly(builder, assembly, flags);

  clock_t end        = clock();
  double  time_spent = (double)(end - begin) / CLOCKS_PER_SEC;

  if (state == COMPILE_OK) {
    msg_log("compiled %i lines in %f seconds", builder->total_lines, time_spent);
  } else {
    msg_log("there were errors, sorry...");
  }

  return state;
}

void
builder_set_error_diag_handler(Builder *builder, diag_handler_f handler, void *context)
{
  builder->on_error     = handler;
  builder->on_error_cnt = context;
}

void
builder_set_warning_diag_handler(Builder *builder, diag_handler_f handler, void *context)
{
  builder->on_warning     = handler;
  builder->on_warning_cnt = context;
}

void
bl_diag_delete_msg(char *msg)
{
  free(msg);
}

void
builder_error(Builder *builder, const char *format, ...)
{
  if (builder->errorc > MAX_ERROR_REPORTED) return;
  char error[MAX_MSG_LEN] = {0};

  va_list args;
  va_start(args, format);
  vsnprintf(error, MAX_MSG_LEN, format, args);
  va_end(args);

  builder->on_error(&error[0], builder->on_error_cnt);
  builder->errorc++;
}

void
builder_warning(Builder *builder, const char *format, ...)
{
  char warning[MAX_MSG_LEN] = {0};

  va_list args;
  va_start(args, format);
  vsnprintf(warning, MAX_MSG_LEN, format, args);
  va_end(args);

  builder->on_warning(&warning[0], builder->on_error_cnt);
}

void
builder_msg(Builder *builder, BuilderMsgType type, int code, Src *src,
            BuilderCurPos pos, const char *format, ...)
{
  if (type == BUILDER_MSG_ERROR && builder->errorc > MAX_ERROR_REPORTED) return;
  if (builder->no_warn && type == BUILDER_MSG_WARNING) return;

  assert(src);
  assert(src->unit);
  BString *tmp              = bo_string_new(MAX_MSG_LEN);
  char     msg[MAX_MSG_LEN] = {0};

  int line = src->line;
  int col  = src->col;
  int len  = src->len;

  switch (pos) {
  case BUILDER_CUR_AFTER:
    col += len;
    len = 1;
    break;

  case BUILDER_CUR_WORD:
    break;

  case BUILDER_CUR_BEFORE:
    col -= col < 1 ? 0 : 1;
    len = 1;
    break;
  }

  snprintf(msg, MAX_MSG_LEN, "[%s%04d] %s:%d:%d ", type == BUILDER_MSG_ERROR ? "E" : "W", code,
           src->unit->filepath, line, col);
  va_list args;
  va_start(args, format);
  vsnprintf(msg + strlen(msg), MAX_MSG_LEN - strlen(msg), format, args);
  va_end(args);
  bo_string_append(tmp, &msg[0]);

  int         pad      = sprintf(msg, "%d", src->line) + 2;
  long        line_len = 0;
  const char *line_str = unit_get_src_ln(src->unit, src->line - 1, &line_len);
  if (line_str && line_len) {
    sprintf(msg, "\n%*d", pad, src->line - 1);
    bo_string_append(tmp, &msg[0]);
    bo_string_append(tmp, " | ");
    bo_string_appendn(tmp, line_str, line_len);
  }

  line_str = unit_get_src_ln(src->unit, src->line, &line_len);
  if (line_str && line_len) {
    sprintf(msg, CYAN("\n%*d"), pad, src->line);
    bo_string_append(tmp, &msg[0]);
    bo_string_append(tmp, " | ");
    bo_string_appendn(tmp, line_str, line_len);
    sprintf(msg, "\n%*s", pad, "");
    bo_string_append(tmp, &msg[0]);
    bo_string_append(tmp, " | ");
  }

  for (int i = 0; i < col + len - 1; ++i) {
    if (i < col - 1)
      bo_string_append(tmp, " ");
    else
      bo_string_append(tmp, type == BUILDER_MSG_ERROR ? RED("^") : YELLOW("^"));
  }

  line_str = unit_get_src_ln(src->unit, src->line + 1, &line_len);
  if (line_str && line_len) {
    sprintf(msg, "\n%*d", pad, src->line + 1);
    bo_string_append(tmp, &msg[0]);
    bo_string_append(tmp, " | ");
    bo_string_appendn(tmp, line_str, line_len);
  }

  if (type == BUILDER_MSG_ERROR) {
    builder->errorc++;
    builder->on_error(bo_string_get(tmp), builder->on_error_cnt);
  } else
    builder->on_warning(bo_string_get(tmp), builder->on_error_cnt);

  bo_unref(tmp);

#if ASSERT_ON_CMP_ERROR
  if (type == BUILDER_MSG_ERROR) assert(false);
#endif
}
