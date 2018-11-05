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
#include "stages.h"
#include "common.h"
#include "ast.h"

#define MAX_STR_BUF 256
#define int_to_void_ptr(i) (void *)((intptr_t)(i))

static inline void
print_address(Ast *node)
{
#if BL_DEBUG
  if (node)
    fprintf(stdout, YELLOW(" %d "), node->_serial);
  else
    fprintf(stdout, RED(" (null) "));
#else
  fprintf(stdout, YELLOW(" %p "), node);
#endif
}

#define print_head(_node, _pad) _print_head((Ast *)(_node), (pad))

static inline void
_print_head(Ast *node, int pad)
{
  if (node->src)
    fprintf(stdout, "\n%*s" GREEN("%s ") CYAN("<%d:%d>"), pad * 2, "", ast_get_name(node),
            node->src->line, node->src->col);
  else
    fprintf(stdout, "\n%*s" GREEN("%s ") CYAN("<IMPLICIT>"), pad * 2, "", ast_get_name(node));

  print_address(node);
}

static inline void
print_flags(int flags)
{
  if (flags)
    fprintf(stdout, " #");
  else
    return;
  if (flags & FLAG_EXTERN) fprintf(stdout, "E");
  if (flags & FLAG_MAIN) fprintf(stdout, "M");
  if (flags & FLAG_TEST) fprintf(stdout, "T");
  if (flags & FLAG_COMPILER) fprintf(stdout, "C");
}

static inline void
print_type(AstType *type)
{
  char tmp[256];
  ast_type_to_str(tmp, 256, type);
  fprintf(stdout, BLUE("{%s}"), tmp);
}

static void
print_bad(Ast *bad, int pad);

static void
print_ublock(AstUBlock *ublock, int pad);

static void
print_block(AstBlock *block, int pad);

static void
print_node(Ast *node, int pad);

static void
print_expr(AstExpr *expr, int pad);

static void
print_decl(AstDecl *decl, int pad);

static void
print_decl_entity(AstDeclEntity *entity, int pad);

static void
print_decl_arg(AstDeclArg *arg, int pad);

static void
print_expr_type(AstExpr *expr_type, int pad);

static void
print_expr_binop(AstExprBinop *binop, int pad);

static void
print_expr_ref(AstExprRef *ref, int pad);

static void
print_expr_unary(AstExprUnary *unary, int pad);

static void
print_expr_lit_fn(AstExprLitFn *fn, int pad);

static void
print_expr_lit_int(AstExprLitInt *lit, int pad);

static void
print_expr_lit_float(AstExprLitFloat *lit, int pad);

static void
print_expr_lit_char(AstExprLitChar *lit, int pad);

static void
print_expr_lit_bool(AstExprLitBool *lit, int pad);

static void
print_expr_lit_string(AstExprLitString *lit, int pad);

static void
print_expr_call(AstExprCall *call, int pad);

/* impl */
void
print_ublock(AstUBlock *ublock, int pad)
{
  print_head(ublock, pad);
  fprintf(stdout, "%s", ublock->unit->name);
  Ast *tmp = NULL;
  node_foreach(ublock->nodes, tmp) print_node(tmp, pad + 1);
}

void
print_block(AstBlock *block, int pad)
{
  print_head(block, pad);
  Ast *tmp = NULL;
  node_foreach(block->nodes, tmp) print_node(tmp, pad + 1);
}

void
print_decl_entity(AstDeclEntity *entity, int pad)
{
  print_head(entity, pad);

  switch (entity->kind) {
  case DECL_ENTITY_INVALID:
    fprintf(stdout, "[INVALID] ");
    break;
  case DECL_ENTITY_FIELD:
    fprintf(stdout, "[FIELD] ");
    break;
  case DECL_ENTITY_TYPE:
    fprintf(stdout, "[TYPE] ");
    break;
  case DECL_ENTITY_FN:
    fprintf(stdout, "[FN] ");
    break;
  case DECL_ENTITY_ENUM:
    fprintf(stdout, "[ENUM] ");
    break;
  }

  AstDecl *base = (AstDecl *)entity;
  fprintf(stdout, "'%s' '%s' used: %d ", base->name->str, entity->mutable ? "mutable" : "immutable",
          entity->used);

  print_type(base->type);
  print_flags(entity->flags);
  print_node((Ast *)entity->value, pad + 1);
}

void
print_decl_arg(AstDeclArg *arg, int pad)
{
  print_head(arg, pad);
}

void
print_bad(Ast *bad, int pad)
{
  print_head(bad, pad);
}

void
print_expr_unary(AstExprUnary *unary, int pad)
{
  print_head(unary, pad);

  const char *op = NULL;
  switch (unary->kind) {
  case UNOP_INVALID:
    op = "invalid";
    break;
  case UNOP_NEG:
    op = "-";
    break;
  case UNOP_POS:
    op = "+";
    break;
  case UNOP_NOT:
    op = "!";
    break;
  case UNOP_ADR:
    op = "&";
    break;
  case UNOP_DEREF:
    op = "*";
    break;
  }

  fprintf(stdout, "'%s' ", op);
  print_node((Ast *)unary->next, pad + 1);
}

void
print_expr_binop(AstExprBinop *binop, int pad)
{
  print_head(binop, pad);

  const char *op = NULL;
  switch (binop->kind) {
  case BINOP_ASSIGN:
    op = "=";
    break;
  case BINOP_ADD_ASSIGN:
    op = "+=";
    break;
  case BINOP_SUB_ASSIGN:
    op = "-=";
    break;
  case BINOP_MUL_ASSIGN:
    op = "*=";
    break;
  case BINOP_DIV_ASSIGN:
    op = "/=";
    break;
  case BINOP_MOD_ASSIGN:
    op = "%=";
    break;
  case BINOP_ADD:
    op = "+";
    break;
  case BINOP_SUB:
    op = "-";
    break;
  case BINOP_MUL:
    op = "*";
    break;
  case BINOP_DIV:
    op = "/";
    break;
  case BINOP_MOD:
    op = "%";
    break;
  case BINOP_EQ:
    op = "==";
    break;
  case BINOP_NEQ:
    op = "!=";
    break;
  case BINOP_GREATER:
    op = ">";
    break;
  case BINOP_LESS:
    op = "<";
    break;
  case BINOP_GREATER_EQ:
    op = ">=";
    break;
  case BINOP_LESS_EQ:
    op = "<=";
    break;
  case BINOP_LOGIC_AND:
    op = "&&";
    break;
  case BINOP_LOGIC_OR:
    op = "||";
    break;
  default:
    op = "invalid";
  }

  fprintf(stdout, "'%s' ", op);
  print_type(((AstExpr *)binop)->type);
  print_node((Ast *)binop->lhs, pad + 1);
  print_node((Ast *)binop->rhs, pad + 1);
}

void
print_expr_type(AstExpr *expr_type, int pad)
{
  print_head(expr_type, pad);
  print_type(expr_type->type);
}

void
print_expr_ref(AstExprRef *ref, int pad)
{
  print_head(ref, pad);
  fprintf(stdout, "'%s' ", ref->ident->str);
}

void
print_expr_lit_int(AstExprLitInt *lit, int pad)
{
  print_head(lit, pad);
  fprintf(stdout, "%llu ", (long long unsigned)lit->i);
}

void
print_expr_lit_float(AstExprLitFloat *lit, int pad)
{
  print_head(lit, pad);
  fprintf(stdout, "%f ", lit->f);
}

void
print_expr_lit_char(AstExprLitChar *lit, int pad)
{
  print_head(lit, pad);
  fprintf(stdout, "%c ", lit->c);
}

void
print_expr_lit_bool(AstExprLitBool *lit, int pad)
{
  print_head(lit, pad);
  fprintf(stdout, "%s ", lit->b ? "true" : "false");
}

void
print_expr_lit_string(AstExprLitString *lit, int pad)
{
  print_head(lit, pad);

  char *tmp = strdup(lit->s);
  fprintf(stdout, "%s ", strtok(tmp, "\n"));
  char *next = strtok(NULL, "\n");
  if (next && strlen(next)) fprintf(stdout, "... ");
  free(tmp);
}

void
print_expr_lit_fn(AstExprLitFn *fn, int pad)
{
  print_head(fn, pad);
  print_type(((AstExpr *)fn)->type);
  print_node(fn->block, pad + 1);
}

void
print_expr_call(AstExprCall *call, int pad)
{
  print_head(call, pad);
  print_type(((AstExpr *)call)->type);

  print_node((Ast *)call->ref, pad + 1);

  Ast *arg;
  node_foreach(call->args, arg) print_node(arg, pad + 1);
}

void
print_expr(AstExpr *expr, int pad)
{
  switch (expr->kind) {
  case AST_EXPR_BAD:
    print_bad((Ast *)expr, pad);
    break;
  case AST_EXPR_TYPE:
    print_expr_type(expr, pad);
    break;
  case AST_EXPR_REF:
    print_expr_ref(&expr->ref, pad);
    break;
  case AST_EXPR_CAST:
    break;
  case AST_EXPR_BINOP:
    print_expr_binop(&expr->binop, pad);
    break;
  case AST_EXPR_CALL:
    print_expr_call(&expr->call, pad);
    break;
  case AST_EXPR_MEMBER:
    break;
  case AST_EXPR_ELEM:
    break;
  case AST_EXPR_SIZEOF:
    break;
  case AST_EXPR_TYPEOF:
    break;
  case AST_EXPR_UNARY:
    print_expr_unary(&expr->unary, pad);
    break;
  case AST_EXPR_NULL:
    break;
  case AST_EXPR_LIT_FN:
    print_expr_lit_fn(&expr->fn, pad);
    break;
  case AST_EXPR_LIT_INT:
    print_expr_lit_int(&expr->integer, pad);
    break;
  case AST_EXPR_LIT_FLOAT:
    print_expr_lit_float(&expr->real, pad);
    break;
  case AST_EXPR_LIT_CHAR:
    print_expr_lit_char(&expr->character, pad);
    break;
  case AST_EXPR_LIT_STRING:
    print_expr_lit_string(&expr->string, pad);
    break;
  case AST_EXPR_LIT_BOOL:
    print_expr_lit_bool(&expr->boolean, pad);
    break;
  case AST_EXPR_LIT_CMP:
    break;
  }
}

void
print_decl(AstDecl *decl, int pad)
{
  switch (decl->kind) {
  case AST_DECL_BAD:
    print_bad((Ast *)decl, pad);
    break;
  case AST_DECL_ENTITY:
    print_decl_entity(&decl->entity, pad);
    break;
  case AST_DECL_ARG:
    print_decl_arg(&decl->arg, pad);
    break;
  case AST_DECL_MEMBER:
  case AST_DECL_VARIANT:
    break;
  }
}

void
print_node(Ast *node, int pad)
{
  if (!node) return;
  switch (node->kind) {
  case AST_BAD:
    print_bad(node, pad);
    break;

  case AST_LOAD:
    break;
  case AST_LINK:
    break;
  case AST_IDENT:
    break;
  case AST_UBLOCK:
    print_ublock(&node->ublock, pad);
    break;
  case AST_BLOCK:
    print_block(&node->block, pad);
    break;
  case AST_STMT_RETURN:
    break;
  case AST_STMT_IF:
    break;
  case AST_STMT_LOOP:
    break;
  case AST_STMT_BREAK:
    break;
  case AST_STMT_CONTINUE:
    break;
  case AST_DECL:
    print_decl(&node->decl, pad);
    break;
  case AST_EXPR:
    print_expr(&node->expr, pad);
    break;
  case AST_TYPE:
  case AST_COUNT:
    break;
  }
}

void
ast_printer_run(Assembly *assembly)
{
  Unit *unit;
  barray_foreach(assembly->units, unit)
  {
    print_node((Ast *)unit->ast, 0);
  }
  fprintf(stdout, "\n\n");
}
