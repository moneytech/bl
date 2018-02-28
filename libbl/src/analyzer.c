//*****************************************************************************
// blc
//
// File:   analyzer.c
// Author: Martin Dorazil
// Date:   09/02/2018
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
#include "bl/analyzer.h"
#include "bl/bldebug.h"
#include "bl/bllimits.h"
#include "bl/unit.h"
#include "ast/ast_impl.h"

#define analyze_error(self, format, ...) \
  { \
    bl_actor_error((Actor *)(self)->unit, (format), ##__VA_ARGS__); \
    longjmp((self)->jmp_error, 1); \
  }

static bool
run(Analyzer *self,
    Unit *unit);

static void
analyze_func(Analyzer *self,
             NodeFuncDecl *func);

static void
analyze_var_decl(Analyzer *self,
                 NodeVarDecl *vdcl);

static void
analyze_stmt(Analyzer *self,
             NodeCmpStmt *stmt);

static void
analyze_gstmt(Analyzer *self,
              NodeGlobalStmt *node);

static void
reset(Analyzer *self,
      Unit *unit);

/* Analyzer members */
bo_decl_members_begin(Analyzer, Stage)
  Unit *unit;
  jmp_buf jmp_error;

  /* tmps */
  NodeFuncDecl *current_func_tmp;
bo_end();

/* Analyzer constructor parameters */
bo_decl_params_with_base_begin(Analyzer, Stage)
bo_end();

bo_impl_type(Analyzer, Stage);

/* Analyzer class init */
void
AnalyzerKlass_init(AnalyzerKlass *klass)
{
  bo_vtbl_cl(klass, Stage)->run =
    (bool (*)(Stage *,
              Actor *)) run;
}

/* Analyzer constructor */
void
Analyzer_ctor(Analyzer *self,
              AnalyzerParams *p)
{
  bo_parent_ctor(Stage, p);
}

/* Analyzer destructor */
void
Analyzer_dtor(Analyzer *self)
{
}

/* Analyzer copy constructor */
bo_copy_result
Analyzer_copy(Analyzer *self,
              Analyzer *other)
{
  return BO_NO_COPY;
}

void
analyze_func(Analyzer *self,
             NodeFuncDecl *func)
{
  self->current_func_tmp = func;
  /*
   * Check return type existence.
   */
  Type *type_tmp = bl_node_decl_get_type((NodeDecl *) func);
  if (bl_type_is_user_defined(type_tmp)) {
    analyze_error(self,
                  "%s %d:%d unknown return type '%s' for function '%s'",
                  bl_unit_get_src_file(self->unit),
                  bo_members(func, Node)->line,
                  bo_members(func, Node)->col,
                  bl_type_get_name(type_tmp),
                  bl_node_decl_get_ident((NodeDecl *) func));
  }

  /*
   * Check params.
   */
  const int c = bl_node_func_decl_get_param_count(func);

  /*
   * Reached maximum count of parameters.
   */
  if (c > BL_MAX_FUNC_PARAM_COUNT) {
    analyze_error(self,
                  "%s %d:%d reached maximum count of function parameters, function has: %d and maximum is: %d",
                  bl_unit_get_src_file(self->unit),
                  bo_members(func, Node)->line,
                  bo_members(func, Node)->col,
                  c,
                  BL_MAX_FUNC_PARAM_COUNT);
  }

  NodeParamVarDecl *param = NULL;
  for (int i = 0; i < c; i++) {
    param = bl_node_func_decl_get_param(func, i);

    type_tmp = bl_node_decl_get_type((NodeDecl *) param);
    if (bl_type_is_user_defined(type_tmp)) {
      analyze_error(self,
                    "%s %d:%d unknown type '%s' for function parameter",
                    bl_unit_get_src_file(self->unit),
                    bo_members(param, Node)->line,
                    bo_members(param, Node)->col,
                    bl_type_get_name(type_tmp));
    }
  }

  /*
   * Validate scope statement if there is one.
   */
  NodeCmpStmt *stmt = bl_node_func_decl_get_stmt(func);
  if (stmt) {
    analyze_stmt(self, stmt);
  }
}

void
analyze_var_decl(Analyzer *self,
                 NodeVarDecl *vdcl)
{
  /*
   * Check type of declaration.
   */
  Type *type_tmp = bl_node_decl_get_type((NodeDecl *) vdcl);
  if (bl_type_is_user_defined(type_tmp)) {
    analyze_error(self,
                  "%s %d:%d unknown type '%s'",
                  bl_unit_get_src_file(self->unit),
                  bo_members(vdcl, Node)->line,
                  bo_members(vdcl, Node)->col,
                  bl_type_get_name(type_tmp));
  } else if (bl_type_is(type_tmp, BL_TYPE_VOID)) {
    analyze_error(self,
                  "%s %d:%d 'void' is not allowed here",
                  bl_unit_get_src_file(self->unit),
                  bo_members(vdcl, Node)->line,
                  bo_members(vdcl, Node)->col);
  }
}

static void
analyze_stmt(Analyzer *self,
             NodeCmpStmt *stmt)
{
  bool return_presented = false;
  const int c = bl_node_stmt_child_get_count(stmt);
  Node *node = NULL;
  for (int i = 0; i < c; i++) {
    node = bl_node_stmt_get_child(stmt, i);
    switch (node->type) {
      case BL_NODE_RETURN_STMT:
        return_presented = true;
        if (i != c - 1) {
          bl_warning("(analyzer) %s %d:%d unrecheable code after 'return' statement",
                     bl_unit_get_src_file(self->unit),
                     node->line,
                     node->col);
        }
        break;
      case BL_NODE_VAR_DECL:
        analyze_var_decl(self, (NodeVarDecl *) node);
        break;
      default:
        break;
    }
  }

  Type *exp_ret = bl_node_decl_get_type((NodeDecl *) self->current_func_tmp);
  Ident *ident = bl_node_decl_get_ident((NodeDecl *) self->current_func_tmp);
  if (!return_presented && bl_type_is_not(exp_ret, BL_TYPE_VOID)) {
    analyze_error(self,
                  "%s %d:%d unterminated function '%s'",
                  bl_unit_get_src_file(self->unit),
                  bo_members(self->current_func_tmp, Node)->line,
                  bo_members(self->current_func_tmp, Node)->col,
                  bl_ident_get_name(ident));
  }
}

void
analyze_gstmt(Analyzer *self,
              NodeGlobalStmt *node)
{
  Node *child = NULL;
  const int c = bl_node_global_stmt_get_child_count(node);
  for (int i = 0; i < c; i++) {
    child = bl_node_global_stmt_get_child(node, i);
    switch (child->type) {
      case BL_NODE_FUNC_DECL:
        analyze_func(self, (NodeFuncDecl *) child);
        break;
      default:
        break;
    }
  }
}

void
reset(Analyzer *self,
      Unit *unit)
{
  self->unit = unit;
}

bool
run(Analyzer *self,
    Unit *unit)
{
  if (setjmp(self->jmp_error))
    return false;

  reset(self, unit);

  Node *root = bl_ast_get_root(bl_unit_get_ast(unit));

  switch (root->type) {
    case BL_NODE_GLOBAL_STMT:
      analyze_gstmt(self, (NodeGlobalStmt *) root);
      break;
    default:
      break;
  }
  return true;
}

/* public */
Analyzer *
bl_analyzer_new(bl_compile_group_e group)
{
  AnalyzerParams p = {.base.group = group};

  return bo_new(Analyzer, &p);
}

