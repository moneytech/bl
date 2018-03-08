//*****************************************************************************
// bl
//
// File:   linker.c
// Author: Martin Dorazil
// Date:   3/7/18
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
//*****************************************************************************

#include <setjmp.h>
#include "stages_impl.h"
#include "common_impl.h"

#define PRINT_GLOBALS 1

#define link_error(cnt, code, loc, format, ...) \
  { \
    bl_builder_error((cnt)->builder, "%s %d:%d " format, loc->file, loc->line, loc->col, ##__VA_ARGS__); \
    longjmp((cnt)->jmp_error, (code)); \
  }

typedef struct
{
  bl_builder_t  *builder;
  bl_assembly_t *assembly;
  jmp_buf       jmp_error;
} context_t;

static void
link(context_t *cnt,
     bl_unit_t *unit);

static void
link_unsatisfied(context_t *cnt,
                 bl_unit_t *unit);

static void
link_call_expr(context_t *cnt,
               bl_node_t *unsatisfied,
               bl_node_t *found);

static void
link_member_expr(context_t *cnt,
                 bl_node_t *unsatisfied,
                 bl_node_t *found);

void
link(context_t *cnt,
     bl_unit_t *unit)
{
  /* copy global declarations and solve collisions */
  BHashTable *dest = bl_scope_get_all(&cnt->assembly->scope);
  BHashTable *src  = bl_scope_get_all(&unit->scope);

  bo_iterator_t src_iter = bo_htbl_begin(src);
  bo_iterator_t src_end  = bo_htbl_end(src);

  bl_node_t *decl;
  bl_node_t *coliding;
  while (!bo_iterator_equal(&src_iter, &src_end)) {
    decl = bo_htbl_iter_peek_value(src, &src_iter, bl_node_t *);
    if (bo_htbl_has_key(dest, decl->value.decl.ident.hash)) {
      coliding = bo_htbl_at(dest, decl->value.decl.ident.hash, bl_node_t *);

      link_error(cnt,
                 BL_ERR_DUPLICATE_SYMBOL,
                 decl,
                 "redeclaration of "
                   BL_YELLOW("'%s'")
                   " previous declaration found here: %s %d:%d",
                 decl->value.decl.ident.name,
                 coliding->file,
                 coliding->line,
                 coliding->col);
    } else {
      bo_htbl_insert(dest, decl->value.decl.ident.hash, decl);
    }
    bo_htbl_iter_next(src, &src_iter);
  }

  bl_scope_clear(&unit->scope);
}

void
link_unsatisfied(context_t *cnt,
                 bl_unit_t *unit)
{
  const size_t c = bo_array_size(unit->unsatisfied);
  bl_node_t    *unsatisfied;
  bl_node_t    *found;

  for (size_t i = 0; i < c; i++) {
    unsatisfied = bo_array_at(unit->unsatisfied, i, bl_node_t *);

    switch (unsatisfied->type) {
      case BL_NODE_CALL_EXPR:
        found = bl_scope_get(
          &cnt->assembly->scope, &unsatisfied->value.call_expr.ident);
        link_call_expr(cnt, unsatisfied, found);
        break;
      case BL_NODE_MEMBER_EXPR:
        found = bl_scope_get(
          &cnt->assembly->scope, &unsatisfied->value.member_expr.ident);
        link_member_expr(cnt, unsatisfied, found);
        break;
      default: bl_abort("expression of type %i cannot be satisfied", unsatisfied->type);
    }
  }

  bo_array_clear(unit->unsatisfied);
}

void
link_call_expr(context_t *cnt,
               bl_node_t *unsatisfied,
               bl_node_t *found)
{
  if (found == NULL) {
    link_error(cnt, BL_ERR_UNKNOWN_SYMBOL, unsatisfied, "unknown function "
      BL_YELLOW("'%s'"), unsatisfied->value.call_expr.ident.name);
  }

  if (found->type != BL_NODE_FUNC_DECL) {
    link_error(cnt,
               BL_ERR_DIFF_KIND_OF_SYMBOL,
               unsatisfied,
               "expected function, but "
                 BL_YELLOW("'%s'")
                 " is %s and it's declared here: %s %d:%d",
               unsatisfied->value.call_expr.ident.name,
               bl_node_to_str(found),
               found->file,
               found->line,
               found->col);

  }

  const int param_count = bl_node_call_expr_get_arg_count(unsatisfied);
  if (param_count != bl_node_func_decl_get_param_count(found)) {
    link_error(cnt,
               BL_ERR_INVALID_PARAM_COUNT,
               unsatisfied,
               "function call "
                 BL_YELLOW("'%s'")
                 " has invalid argument count %d (expected is %d), declared here: %s %d:%d",
               unsatisfied->value.call_expr.ident.name,
               param_count,
               bl_node_func_decl_get_param_count(found),
               found->file,
               found->line,
               found->col);
  }

  /* TODO: args type check
  bl_node_t *param_uns;
  bl_node_t *param_found;
  for (size_t i = 0; i < param_count; i++) {
    param_found = bo_array_at(found->value.func_decl.params, i, bl_node_t *);
    param_uns = bo_array_at(unsatisfied->value.call_expr.args, i, bl_node_t *);

    if (param_uns->value.decl.type.hash != param_found->value.decl.type.hash) {
      link_error(cnt,
                 BL_ERR_INVALID_PARAM_COUNT,
                 param_uns,
                 "invalid type of argument "
                   BL_YELLOW("'%s'")
                   " (expected is %s), declared here: %s %d:%d",
                 unsatisfied->value.call_expr.ident.name,
                 param_found->value.decl.type.name,
                 found->file,
                 found->line,
                 found->col);
    }
  }
  */

  unsatisfied->value.call_expr.callee = found;
}

void
link_member_expr(context_t *cnt,
                 bl_node_t *unsatisfied,
                 bl_node_t *found)
{
//  bl_log("found %s", found->value.decl.ident.name);
}

/* public */
bl_error_e
bl_linker_run(bl_builder_t *builder,
              bl_assembly_t *assembly)
{
  /*
   * Solve unsatisfied expressions and declaration collisions
   * between units.
   */

  context_t cnt = {
    .builder = builder, .assembly = assembly
  };

  int error = 0;
  if ((error = setjmp(cnt.jmp_error))) {
    return (bl_error_e) error;
  }

  const int c     = bl_assembly_get_unit_count(assembly);
  bl_unit_t *unit = NULL;

  for (int i = 0; i < c; i++) {
    unit = bl_assembly_get_unit(assembly, i);
    link(&cnt, unit);
  }

  for (int i = 0; i < c; i++) {
    unit = bl_assembly_get_unit(assembly, i);
    link_unsatisfied(&cnt, unit);
  }

  /* TEST */
#if PRINT_GLOBALS
  BHashTable    *src     = bl_scope_get_all(&assembly->scope);
  bo_iterator_t src_iter = bo_htbl_begin(src);
  bo_iterator_t src_end  = bo_htbl_end(src);

  bl_node_t *decl;
  while (!bo_iterator_equal(&src_iter, &src_end)) {
    decl = bo_htbl_iter_peek_value(src, &src_iter, bl_node_t *);
    bl_log("decl: %s", decl->value.decl.ident.name);
    bo_htbl_iter_next(src, &src_iter);
  }
#endif

  return BL_NO_ERR;
}

