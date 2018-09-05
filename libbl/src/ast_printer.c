//************************************************************************************************
// blc
//
// File:   ast_printer.c
// Author: Martin Dorazil
// Date:   04/02/2018
//
// Copyright 2018 Martin Dorazil
//
// Permissicopy
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

#include <stdio.h>
#include "stages_impl.h"
#include "common_impl.h"
#include "ast_impl.h"

#define MAX_STR_BUF 256

static inline void
print_address(bl_node_t *node)
{
#if BL_DEBUG
  if (node)
    fprintf(stdout, BL_YELLOW(" %d "), node->_serial);
  else
    fprintf(stdout, BL_RED(" (null) "));
#else
  fprintf(stdout, BL_YELLOW(" %p "), node);
#endif
}

static inline void
print_head(const char *name, bl_src_t *src, bl_node_t *ptr, int pad)
{
  if (src)
    fprintf(stdout, "\n%*s" BL_GREEN("%s ") BL_CYAN("<%d:%d>"), pad * 2, "", name, src->line,
            src->col);
  else
    fprintf(stdout, "\n%*s" BL_GREEN("%s ") BL_CYAN("<IMPLICIT>"), pad * 2, "", name);

  print_address(ptr);
}

static inline void
print_type(bl_node_t *type)
{
  if (!type) {
    fprintf(stdout, BL_RED("{?}"));
    return;
  }
  char tmp[MAX_STR_BUF];
  bl_ast_type_to_string(tmp, MAX_STR_BUF, type);
  fprintf(stdout, BL_CYAN("{%s}"), tmp);
}

static inline void
print_flags(int flags)
{
  if (flags)
    fprintf(stdout, " #");
  else
    return;
  if (flags & BL_FLAG_EXTERN) fprintf(stdout, "E");
  if (flags & BL_FLAG_MAIN) fprintf(stdout, "M");
}

static void
print_load(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_sizeof(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_member(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_elem(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_unary(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_break(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_continue(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_ublock(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_type_struct(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_decl(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_block(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_bad(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_binop(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_call(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_lit(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_lit_fn(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_lit_struct(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_lit_enum(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_ident(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_return(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_if(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_loop(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_cast(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

static void
print_null(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad);

// impl
void
print_sizeof(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("sizeof", node->src, node, (intptr_t)pad);
  bl_node_expr_sizeof_t *_sizeof = bl_peek_expr_sizeof(node);
  print_type(_sizeof->in);
}

void
print_member(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("member", node->src, node, (intptr_t)pad);
  bl_node_expr_member_t *_member = bl_peek_expr_member(node);
  print_type(_member->type);
  fprintf(stdout, " (%s)", _member->ptr_ref ? "->" : ".");
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_elem(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("elem", node->src, node, (intptr_t)pad);
  bl_node_expr_elem_t *_elem = bl_peek_expr_elem(node);
  print_type(_elem->type);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_load(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("load", node->src, node, (intptr_t)pad);
  bl_node_load_t *_load = bl_peek_load(node);
  fprintf(stdout, "'%s'", _load->filepath);
}

void
print_lit_struct(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("struct", node->src, node, (intptr_t)pad);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_lit_enum(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("enum", node->src, node, (intptr_t)pad);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_break(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("break", node->src, node, (intptr_t)pad);
}

void
print_continue(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("continue", node->src, node, (intptr_t)pad);
}

void
print_cast(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("cast", node->src, node, (intptr_t)pad);
  bl_node_expr_cast_t *_cast = bl_peek_expr_cast(node);
  print_type(_cast->type);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_null(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("null", node->src, node, (intptr_t)pad);
  bl_node_expr_null_t *_null = bl_peek_expr_null(node);
  print_type(_null->type);
}

void
print_unary(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("unary", node->src, node, (intptr_t)pad);
  bl_node_expr_unary_t *_unary = bl_peek_expr_unary(node);
  fprintf(stdout, "%s ", bl_sym_strings[_unary->op]);
  print_type(_unary->type);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_if(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("if", node->src, node, (intptr_t)pad);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_loop(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("loop", node->src, node, (intptr_t)pad);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_decl(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("declaration", node->src, node, (intptr_t)pad);
  bl_node_decl_t *_decl = bl_peek_decl(node);
  fprintf(stdout, "[%d] ", _decl->kind);
  fprintf(stdout, "%s (%s) used: %d ", bl_peek_ident(_decl->name)->str,
          _decl->mutable ? "mutable" : "immutable", _decl->used);

  print_type(_decl->type);
  print_flags(_decl->flags);
  bl_ast_visit(visitor, _decl->value, pad + 1);
}

void
print_type_struct(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("struct", node->src, node, (intptr_t)pad);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_block(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("block", node->src, node, (intptr_t)pad);
  bl_ast_walk(visitor, node, pad);
}

void
print_ident(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("ident", node->src, node, (intptr_t)pad);
  bl_node_ident_t *_ident = bl_peek_ident(node);
  fprintf(stdout, "%s ->", _ident->str);
  print_address(_ident->ref);
}

void
print_return(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("return", node->src, node, (intptr_t)pad);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_ublock(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("unit", node->src, node, (intptr_t)pad);
  bl_node_ublock_t *_ublock = bl_peek_ublock(node);
  fprintf(stdout, "%s", _ublock->unit->name);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_bad(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("INVALID", node->src, node, (intptr_t)pad);
}

void
print_binop(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("binop", node->src, node, (intptr_t)pad);
  bl_node_expr_binop_t *_binop = bl_peek_expr_binop(node);
  fprintf(stdout, "%s ", bl_sym_strings[_binop->op]);
  print_type(_binop->type);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_lit(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("literal", node->src, node, (intptr_t)pad);
  bl_node_lit_t *_lit = bl_peek_lit(node);
  assert(_lit->type);

  bl_node_type_fund_t *_type = bl_peek_type_fund(bl_ast_get_type(_lit->type));
  switch (_type->code) {
  case BL_FTYPE_S8:
  case BL_FTYPE_S16:
  case BL_FTYPE_S32:
  case BL_FTYPE_S64:
  case BL_FTYPE_U8:
  case BL_FTYPE_U16:
  case BL_FTYPE_U32:
  case BL_FTYPE_U64:
  case BL_FTYPE_SIZE: fprintf(stdout, "%llu ", _lit->value.u); break;
  case BL_FTYPE_F32:
  case BL_FTYPE_F64: fprintf(stdout, "%f ", _lit->value.d); break;
  case BL_FTYPE_CHAR: fprintf(stdout, "%c ", _lit->value.c); break;
  case BL_FTYPE_STRING: {
    char *tmp = strdup(_lit->value.str);
    fprintf(stdout, "%s ", strtok(tmp, "\n"));
    char *next = strtok(NULL, "\n");
    if (next && strlen(next)) fprintf(stdout, "... ");
    free(tmp);
    break;
  }
  case BL_FTYPE_BOOL: fprintf(stdout, "%s ", _lit->value.u ? "true" : "false"); break;
  default: break;
  }

  print_type(_lit->type);
}

void
print_lit_fn(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("function", node->src, node, (intptr_t)pad);
  bl_node_lit_fn_t *_fn = bl_peek_lit_fn(node);

  print_type(_fn->type);
  bl_ast_walk(visitor, node, pad + 1);
}

void
print_call(bl_ast_visitor_t *visitor, bl_node_t *node, void *pad)
{
  print_head("call", node->src, node, (intptr_t)pad);
  bl_node_expr_call_t *_call = bl_peek_expr_call(node);
  assert(_call->ref);
  if (bl_node_is(_call->ref, BL_NODE_IDENT)) {
    bl_node_ident_t *_ident = bl_peek_ident(_call->ref);
    fprintf(stdout, "%s ->", _ident->str);
    print_address(_ident->ref);
  }
  print_type(_call->type);
  if (_call->run) fprintf(stdout, " #run");

  bl_ast_walk(visitor, node, pad + 1);

  if (bl_node_is(_call->ref, BL_NODE_EXPR_MEMBER)) {
    bl_ast_visit(visitor, _call->ref, pad + 2);
  }
}

void
bl_ast_printer_run(bl_assembly_t *assembly)
{
  bl_ast_visitor_t visitor;
  bl_ast_visitor_init(&visitor);

  bl_ast_visitor_add(&visitor, print_bad, BL_NODE_BAD);
  bl_ast_visitor_add(&visitor, print_ublock, BL_NODE_UBLOCK);
  bl_ast_visitor_add(&visitor, print_block, BL_NODE_BLOCK);
  bl_ast_visitor_add(&visitor, print_ident, BL_NODE_IDENT);
  bl_ast_visitor_add(&visitor, print_decl, BL_NODE_DECL);
  bl_ast_visitor_add(&visitor, print_load, BL_NODE_LOAD);
  bl_ast_visitor_add(&visitor, print_lit_fn, BL_NODE_LIT_FN);
  bl_ast_visitor_add(&visitor, print_lit_struct, BL_NODE_LIT_STRUCT);
  bl_ast_visitor_add(&visitor, print_lit_enum, BL_NODE_LIT_ENUM);
  bl_ast_visitor_add(&visitor, print_lit, BL_NODE_LIT);
  bl_ast_visitor_add(&visitor, print_return, BL_NODE_STMT_RETURN);
  bl_ast_visitor_add(&visitor, print_if, BL_NODE_STMT_IF);
  bl_ast_visitor_add(&visitor, print_loop, BL_NODE_STMT_LOOP);
  bl_ast_visitor_add(&visitor, print_break, BL_NODE_STMT_BREAK);
  bl_ast_visitor_add(&visitor, print_continue, BL_NODE_STMT_CONTINUE);
  bl_ast_visitor_add(&visitor, print_call, BL_NODE_EXPR_CALL);
  bl_ast_visitor_add(&visitor, print_binop, BL_NODE_EXPR_BINOP);
  bl_ast_visitor_add(&visitor, print_null, BL_NODE_EXPR_NULL);
  bl_ast_visitor_add(&visitor, print_sizeof, BL_NODE_EXPR_SIZEOF);
  bl_ast_visitor_add(&visitor, print_cast, BL_NODE_EXPR_CAST);
  bl_ast_visitor_add(&visitor, print_member, BL_NODE_EXPR_MEMBER);
  bl_ast_visitor_add(&visitor, print_elem, BL_NODE_EXPR_ELEM);
  bl_ast_visitor_add(&visitor, print_unary, BL_NODE_EXPR_UNARY);
  bl_ast_visitor_add(&visitor, print_type_struct, BL_NODE_TYPE_STRUCT);

  bl_unit_t *unit;
  bl_barray_foreach(assembly->units, unit)
  {
    bl_ast_visit(&visitor, unit->ast.root, 0);
  }
  fprintf(stdout, "\n\n");
}
