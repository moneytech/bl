//*****************************************************************************
// Biscuit Engine
//
// File:   parser.c
// Author: Martin Dorazil
// Date:   03/02/2018
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
#include <string.h>
#include <bobject/containers/array.h>
#include "stages_impl.h"
#include "common_impl.h"

#define parse_error(cnt, code, format, ...) \
  { \
    bl_builder_error((cnt)->builder, (format), ##__VA_ARGS__); \
    longjmp((cnt)->jmp_error, (code)); \
  }

#define new_node(cnt, type, tok) \
  bl_ast_new_node(&(cnt)->unit->ast, (type), (cnt)->unit->filepath, (tok)->line, (tok)->col);

typedef struct context
{
  bl_builder_t *builder;
  bl_unit_t    *unit;
  bl_tokens_t  *tokens;

  jmp_buf    jmp_error;

  /* tmp */
  bool is_loop;
  bl_token_t *modif;
} context_t;

static void
parse_semicolon_erq(context_t *cnt);

static void
parse_modif(context_t *cnt);

static bl_node_t *
parse_global_stmt(context_t *cnt);

static bl_node_t *
parse_cmp_stmt(context_t *cnt);

static bl_node_t *
parse_if_stmt(context_t *cnt);

static bl_node_t *
parse_expr(context_t *cnt);

static bl_node_t *
parse_atom_expr(context_t *cnt);

static bl_node_t *
parse_expr_1(context_t *cnt,
             bl_node_t *lhs,
             int min_precedence);

static bl_node_t *
parse_call_expr(context_t *cnt);

static bl_node_t *
parse_dec_ref_expr(context_t *cnt);

static bl_node_t *
parse_var_decl(context_t *cnt);

static bl_node_t *
parse_func_decl(context_t *cnt);

static bl_node_t *
parse_enum_decl(context_t *cnt);

static bl_node_t *
parse_struct_decl(context_t *cnt);

static bl_node_t *
parse_param_var_decl(context_t *cnt);

static bl_node_t *
parse_return_stmt(context_t *cnt);

static bl_node_t *
parse_loop_stmt(context_t *cnt);

static bl_node_t *
parse_break_stmt(context_t *cnt);

static bl_node_t *
parse_continue_stmt(context_t *cnt);

void
parse_semicolon_erq(context_t *cnt)
{
  bl_token_t *tok = bl_tokens_consume(cnt->tokens);
  if (tok->sym != BL_SYM_SEMICOLON) {
    parse_error(cnt, BL_ERR_MISSING_SEMICOLON, "%s %d:%d missing semicolon "
      BL_YELLOW("';'")
      " at the end of expression", cnt->unit->filepath, tok->line, tok->col);
  }
}

void
parse_modif(context_t *cnt)
{
  bl_token_t *tok = bl_tokens_peek(cnt->tokens);
  switch (tok->sym) {
    case BL_SYM_EXTERN:
    case BL_SYM_CONST:
      bl_tokens_consume(cnt->tokens);
      cnt->modif = tok;
      break;
    default:
      break;
  }
}

bl_node_t *
parse_global_stmt(context_t *cnt)
{
  bl_node_t
    *gstmt = bl_ast_new_node(&cnt->unit->ast, BL_NODE_GLOBAL_STMT, cnt->unit->filepath, 1, 0);

stmt:
  if (bl_tokens_current_is(cnt->tokens, BL_SYM_SEMICOLON)) {
    bl_token_t *tok = bl_tokens_consume(cnt->tokens);
    bl_warning("%s %d:%d extra semicolon can be removed "
                 BL_YELLOW("';'"), cnt->unit->filepath, tok->line, tok->col);
    goto stmt;
  }

  parse_modif(cnt);

  if (bl_node_glob_stmt_add_child(gstmt, parse_struct_decl(cnt))) {
    goto stmt;
  }

  if (bl_node_glob_stmt_add_child(gstmt, parse_enum_decl(cnt))) {
    goto stmt;
  }

  if (bl_node_glob_stmt_add_child(gstmt, parse_func_decl(cnt))) {
    goto stmt;
  }

  if (bl_tokens_current_is_not(cnt->tokens, BL_SYM_EOF)) {
    bl_token_t *tok = bl_tokens_peek(cnt->tokens);
    parse_error(cnt,
                BL_ERR_UNEXPECTED_DECL,
                "%s %d:%d unexpected declaration in global scope",
                cnt->unit->filepath,
                tok->line,
                tok->col);
  }

  return gstmt;
}

/*
 * Return statement.
 */
bl_node_t *
parse_return_stmt(context_t *cnt)
{
  bl_node_t *rstmt = NULL;
  if (bl_tokens_current_is(cnt->tokens, BL_SYM_RETURN)) {
    bl_token_t *tok = bl_tokens_consume(cnt->tokens);
    rstmt           = new_node(cnt, BL_NODE_RETURN_STMT, tok);

    /*
     * Here we expect nothing (for void returning functions) or
     * some expression.
     */

    if (bl_tokens_current_is_not(cnt->tokens, BL_SYM_SEMICOLON)) {
      bl_node_t *expr = parse_expr(cnt);
      if (expr) {
        rstmt->value.return_stmt.expr = expr;
      } else {
        tok = bl_tokens_consume(cnt->tokens);
        parse_error(cnt,
                    BL_ERR_EXPECTED_EXPR,
                    "%s %d:%d expected expression or nothing after return statement",
                    cnt->unit->filepath,
                    tok->line,
                    tok->col);
      }
    }
  }
  return rstmt;
}

bl_node_t *
parse_loop_stmt(context_t *cnt)
{
  bl_node_t *loop        = NULL;
  bool      prev_is_loop = cnt->is_loop;

  bl_token_t *tok = bl_tokens_peek(cnt->tokens);
  if (tok->sym == BL_SYM_LOOP || tok->sym == BL_SYM_WHILE) {
    bl_tokens_consume(cnt->tokens);
    loop = new_node(cnt, BL_NODE_LOOP_STMT, tok);

    if (tok->sym == BL_SYM_WHILE) {
      /* while loop has expression defined in (expr) so we parse it here */

      /* eat ( */
      tok = bl_tokens_consume(cnt->tokens);
      if (tok->sym != BL_SYM_LPAREN) {
        parse_error(cnt, BL_ERR_MISSING_COMMA, "%s %d:%d missing "
          BL_YELLOW("'('")
          " after while statement", cnt->unit->filepath, tok->line, tok->col);
      }

      loop->value.loop_stmt.expr = parse_expr(cnt);
      if (!loop->value.loop_stmt.expr) {
        parse_error(cnt,
                    BL_ERR_EXPECTED_EXPR,
                    "%s %d:%d expected while statement expression ",
                    cnt->unit->filepath,
                    tok->line,
                    tok->col);
      }

      /* eat ) */
      tok = bl_tokens_consume(cnt->tokens);
      if (tok->sym != BL_SYM_RPAREN) {
        parse_error(cnt, BL_ERR_MISSING_COMMA, "%s %d:%d missing "
          BL_YELLOW("')'")
          " after while statement expression", cnt->unit->filepath, tok->line, tok->col);
      }

    } else {
      /* loop without expression defined */
      loop->value.loop_stmt.expr = NULL;
    }

    cnt->is_loop = true;
    bl_scope_push(&cnt->unit->scope);
    bl_node_t *stmt = parse_cmp_stmt(cnt);
    bl_scope_pop(&cnt->unit->scope);
    cnt->is_loop = prev_is_loop;
    if (!stmt) {
      parse_error(cnt,
                  BL_ERR_EXPECTED_STMT,
                  "%s %d:%d expected if statement body",
                  cnt->unit->filepath,
                  tok->line,
                  tok->col);
    }

    loop->value.loop_stmt.cmp_stmt = stmt;
  }

  return loop;
}

bl_node_t *
parse_break_stmt(context_t *cnt)
{
  bl_node_t *break_stmt = NULL;

  bl_token_t *tok = bl_tokens_peek(cnt->tokens);
  if (tok->sym == BL_SYM_BREAK) {
    if (!cnt->is_loop) {
      parse_error(cnt, BL_ERR_BREAK_OUTSIDE_LOOP, "%s %d:%d "
        BL_YELLOW("break")
        " statement outside of a loop or switch", cnt->unit->filepath, tok->line, tok->col);
    }

    bl_tokens_consume(cnt->tokens);
    break_stmt = new_node(cnt, BL_NODE_BREAK_STMT, tok);
  }

  return break_stmt;
}

bl_node_t *
parse_continue_stmt(context_t *cnt)
{
  bl_node_t *continue_stmt = NULL;

  bl_token_t *tok = bl_tokens_peek(cnt->tokens);
  if (tok->sym == BL_SYM_CONTINUE) {
    if (!cnt->is_loop) {
      parse_error(cnt, BL_ERR_CONTINUE_OUTSIDE_LOOP, "%s %d:%d "
        BL_YELLOW("continue")
        " statement outside of a loop or switch", cnt->unit->filepath, tok->line, tok->col);
    }

    bl_tokens_consume(cnt->tokens);
    continue_stmt = new_node(cnt, BL_NODE_CONTINUE_STMT, tok);
  }

  return continue_stmt;
}

bl_node_t *
parse_if_stmt(context_t *cnt)
{
  bl_node_t  *ifstmt    = NULL;
  bl_node_t  *expr      = NULL;
  bl_node_t  *then_stmt = NULL;
  bl_node_t  *else_stmt = NULL;
  bl_token_t *tok       = NULL;

  if (bl_tokens_current_is(cnt->tokens, BL_SYM_IF)) {
    bl_tokens_consume(cnt->tokens);

    tok = bl_tokens_consume(cnt->tokens);
    if (tok->sym != BL_SYM_LPAREN) {
      parse_error(cnt, BL_ERR_MISSING_COMMA, "%s %d:%d missing "
        BL_YELLOW("'('")
        " after if statement", cnt->unit->filepath, tok->line, tok->col);
    }

    expr = parse_expr(cnt);
    if (!expr) {
      parse_error(cnt,
                  BL_ERR_EXPECTED_EXPR,
                  "%s %d:%d expected expression ",
                  cnt->unit->filepath,
                  tok->line,
                  tok->col);
    }

    tok = bl_tokens_consume(cnt->tokens);
    if (tok->sym != BL_SYM_RPAREN) {
      parse_error(cnt, BL_ERR_MISSING_COMMA, "%s %d:%d missing "
        BL_YELLOW("')'")
        " after expression", cnt->unit->filepath, tok->line, tok->col);
    }

    /*
     * Parse then compound statement
     */
    bl_scope_push(&cnt->unit->scope);
    then_stmt = parse_cmp_stmt(cnt);
    bl_scope_pop(&cnt->unit->scope);
    if (!then_stmt) {
      parse_error(cnt,
                  BL_ERR_EXPECTED_STMT,
                  "%s %d:%d expected if statement body",
                  cnt->unit->filepath,
                  tok->line,
                  tok->col);
    }

    ifstmt = new_node(cnt, BL_NODE_IF_STMT, tok);
    ifstmt->value.if_stmt.expr      = expr;
    ifstmt->value.if_stmt.then_stmt = then_stmt;

    /*
     * Parse else statement if there is one.
     */
    if (bl_tokens_current_is(cnt->tokens, BL_SYM_ELSE)) {
      tok = bl_tokens_consume(cnt->tokens);

      /* else if */
      if (bl_tokens_current_is(cnt->tokens, BL_SYM_IF)) {
        ifstmt->value.if_stmt.else_if_stmt = parse_if_stmt(cnt);
      } else {
        bl_scope_push(&cnt->unit->scope);
        else_stmt = parse_cmp_stmt(cnt);
        bl_scope_pop(&cnt->unit->scope);

        if (!else_stmt) {
          parse_error(cnt,
                      BL_ERR_EXPECTED_STMT,
                      "%s %d:%d expected else statement body",
                      cnt->unit,
                      tok->line,
                      tok->col);
        }

        ifstmt->value.if_stmt.else_stmt = else_stmt;
      }
    }
  }

  return ifstmt;
}

bl_node_t *
parse_cmp_stmt(context_t *cnt)
{
  /* eat '{' */
  bl_token_t *tok  = bl_tokens_consume(cnt->tokens);
  if (tok->sym != BL_SYM_LBLOCK) {
    parse_error(cnt, BL_ERR_EXPECTED_BODY, "%s %d:%d expected scope body "
      BL_YELLOW("'{'"), cnt->unit->filepath, tok->line, tok->col);
  }

  bl_node_t  *stmt = new_node(cnt, BL_NODE_CMP_STMT, tok);

//  bl_scope_push(&cnt->unit->scope);
stmt:
  if (bl_tokens_current_is(cnt->tokens, BL_SYM_SEMICOLON)) {
    tok = bl_tokens_consume(cnt->tokens);
    bl_warning("%s %d:%d extra semicolon can be removed "
                 BL_YELLOW("';'"), cnt->unit->filepath, tok->line, tok->col);
    goto stmt;
  }

  parse_modif(cnt);

  /* compound sub-statement */
  if (bl_tokens_current_is(cnt->tokens, BL_SYM_LBLOCK)) {
    bl_scope_push(&cnt->unit->scope);
    bl_node_cmp_stmt_add_child(stmt, parse_cmp_stmt(cnt));
    bl_scope_pop(&cnt->unit->scope);
    goto stmt;
  }

  /* var decl */
  if (bl_node_cmp_stmt_add_child(stmt, parse_var_decl(cnt))) {
    parse_semicolon_erq(cnt);
    goto stmt;
  }

  /* expr */
  if (bl_node_cmp_stmt_add_child(stmt, parse_expr(cnt))) {
    parse_semicolon_erq(cnt);
    goto stmt;
  }

  /* if stmt*/
  if (bl_node_cmp_stmt_add_child(stmt, parse_if_stmt(cnt)))
    goto stmt;

  /* loop */
  if (bl_node_cmp_stmt_add_child(stmt, parse_loop_stmt(cnt)))
    goto stmt;

  /* return stmt */
  if (bl_node_cmp_stmt_add_child(stmt, parse_return_stmt(cnt))) {
    parse_semicolon_erq(cnt);
    goto stmt;
  }

  /* break stmt */
  if (bl_node_cmp_stmt_add_child(stmt, parse_break_stmt(cnt))) {
    parse_semicolon_erq(cnt);
    goto stmt;
  }

  /* continue stmt */
  if (bl_node_cmp_stmt_add_child(stmt, parse_continue_stmt(cnt))) {
    parse_semicolon_erq(cnt);
    goto stmt;
  }

  tok = bl_tokens_consume(cnt->tokens);

  if (tok->sym != BL_SYM_RBLOCK) {
    parse_error(cnt, BL_ERR_EXPECTED_BODY_END, "%s %d:%d expected declaration or scope end "
      BL_YELLOW("'}'"), cnt->unit->filepath, tok->line, tok->col + tok->len);
  }

  return stmt;
}

bl_node_t *
parse_func_decl(context_t *cnt)
{
  bl_node_t *func_decl = NULL;
  bl_sym_e  modif      = BL_SYM_NONE;

  /*
   * handle modificators
   */

  /* Store marker in case when current sequence of tokens is not function at all. */
  bl_tokens_set_marker(cnt->tokens);

  if (bl_tokens_is_seq(
    cnt->tokens, 3, BL_SYM_IDENT, BL_SYM_IDENT, BL_SYM_LPAREN)) {

    if (cnt->modif) {
      switch (cnt->modif->sym) {
        case BL_SYM_EXTERN:
          modif = cnt->modif->sym;
          cnt->modif = NULL;
          break;
        default: {
          parse_error(cnt,
                      BL_ERR_UNEXPECTED_MODIF,
                      "%s %d:%d function declaration cannot be "
                        BL_YELLOW("%s"),
                      cnt->unit->filepath,
                      cnt->modif->line,
                      cnt->modif->col,
                      bl_sym_strings[cnt->modif->sym]);
        }
      }
    }

    bl_token_t *tok_type  = bl_tokens_consume(cnt->tokens);
    bl_token_t *tok_ident = bl_tokens_consume(cnt->tokens);

    func_decl = new_node(cnt, BL_NODE_FUNC_DECL, tok_ident);

    bl_type_init(&func_decl->value.decl.type, tok_type->value.as_string);
    bl_ident_init(&func_decl->value.decl.ident, tok_ident->value.as_string);

    /*
     * Validate and store into scope cache.
     */
    bl_scope_t *scope      = &cnt->unit->scope;
    bl_node_t  *conflicted = bl_scope_add(scope, func_decl);
    if (conflicted != NULL) {
      parse_error(cnt,
                  BL_ERR_DUPLICATE_SYMBOL,
                  "%s %d:%d "
                    BL_YELLOW("'%s'")
                    " already declared here: %d:%d",
                  cnt->unit->filepath,
                  func_decl->line,
                  func_decl->col,
                  func_decl->value.decl.ident.name,
                  conflicted->line,
                  conflicted->col);
    }

    /* consume '(' */
    bl_tokens_consume(cnt->tokens);

    bl_scope_push(&cnt->unit->scope);

    if (bl_tokens_current_is_not(cnt->tokens, BL_SYM_RPAREN)) {
param:
      bl_node_func_decl_stmt_add_param(func_decl, parse_param_var_decl(cnt));
      if (bl_tokens_consume_if(cnt->tokens, BL_SYM_COMMA))
        goto param;
    }

    bl_token_t *tok = bl_tokens_consume(cnt->tokens);
    if (tok->sym != BL_SYM_RPAREN) {
      parse_error(cnt, BL_ERR_MISSING_BRACKET, "%s %d:%d expected "
        BL_YELLOW("')'")
        " after function parameter declaration", cnt->unit->filepath, tok->line, tok->col);
    }

    if (modif == BL_SYM_EXTERN) {
      tok = bl_tokens_consume(cnt->tokens);
      if (tok->sym != BL_SYM_SEMICOLON) {
        parse_error(cnt, BL_ERR_MISSING_SEMICOLON, "%s %d:%d missing semicolon "
          BL_YELLOW("';'")
          " at the end of extern function declaration", cnt->unit->filepath, tok->line, tok->col);
      }
    } else {
      func_decl->value.func_decl.cmp_stmt = parse_cmp_stmt(cnt);
    }

    bl_scope_pop(&cnt->unit->scope);
  } else {
    /* Roll back to marker. */
    bl_tokens_back_to_marker(cnt->tokens);
  }

  return func_decl;
}

bl_node_t *
parse_param_var_decl(context_t *cnt)
{
  bl_token_t *tok   = bl_tokens_consume(cnt->tokens);
  if (tok->sym != BL_SYM_IDENT) {
    parse_error(cnt,
                BL_ERR_EXPECTED_TYPE,
                "%s %d:%d expected parameter type",
                cnt->unit->filepath,
                tok->line,
                tok->col);
  }

  const char *type = tok->value.as_string;

  tok = bl_tokens_consume(cnt->tokens);
  if (tok->sym != BL_SYM_IDENT) {
    parse_error(cnt,
                BL_ERR_EXPECTED_NAME,
                "%s %d:%d expected parameter name",
                cnt->unit->filepath,
                tok->line,
                tok->col);
  }

  const char *ident = tok->value.as_string;

  bl_node_t  *param = new_node(cnt, BL_NODE_PARAM_VAR_DECL, tok);

  bl_type_init(&param->value.param_var_decl.base.type, type);
  bl_ident_init(&param->value.param_var_decl.base.ident, ident);

  bl_scope_t *scope      = &cnt->unit->scope;
  bl_node_t  *conflicted = bl_scope_add(scope, param);
  if (conflicted != NULL) {
    parse_error(cnt,
                BL_ERR_DUPLICATE_SYMBOL,
                "%s %d:%d "
                  BL_YELLOW("'%s'")
                  " already declared here: %d:%d",
                cnt->unit->filepath,
                param->line,
                param->col,
                param->value.decl.ident.name,
                conflicted->line,
                conflicted->col);
  }
  return param;
}

bl_node_t *
parse_enum_decl(context_t *cnt)
{
  bl_node_t  *enm = NULL;
  bl_token_t *tok;

  tok = bl_tokens_consume_if(cnt->tokens, BL_SYM_ENUM);
  if (tok) {
    if (cnt->modif) {
      parse_error(cnt,
                  BL_ERR_UNEXPECTED_MODIF,
                  "%s %d:%d enum cannot be declared as "
                    BL_YELLOW("%s"),
                  cnt->unit->filepath,
                  cnt->modif->line,
                  cnt->modif->col,
                  bl_sym_strings[cnt->modif->sym]);
    }

    tok = bl_tokens_consume(cnt->tokens);
    if (tok->sym != BL_SYM_IDENT) {
      parse_error(cnt,
                  BL_ERR_EXPECTED_NAME,
                  "%s %d:%d expected enum name",
                  cnt->unit->filepath,
                  tok->line,
                  tok->col);
    }

    enm = new_node(cnt, BL_NODE_ENUM_DECL, tok);

    /* TODO parse base type: enum my_enum : i32 {} */
    bl_type_init(&enm->value.decl.type, "i32");
    bl_ident_init(&enm->value.decl.ident, tok->value.as_string);
    enm->value.decl.modificator = BL_SYM_NONE;

    /* eat '{' */
    tok = bl_tokens_consume(cnt->tokens);
    if (tok->sym != BL_SYM_LBLOCK) {
      parse_error(cnt, BL_ERR_EXPECTED_BODY, "%s %d:%d expected enum body "
        BL_YELLOW("'{'"), cnt->unit->filepath, tok->line, tok->col);
    }

elem:
    /* eat ident */
    if ((tok = bl_tokens_consume_if(cnt->tokens, BL_SYM_IDENT))) {
      if (bl_tokens_consume_if(cnt->tokens, BL_SYM_COMMA)) {
        goto elem;
      } else if (bl_tokens_peek(cnt->tokens)->sym != BL_SYM_RBLOCK) {
        tok = bl_tokens_consume(cnt->tokens);
        parse_error(cnt, BL_ERR_MISSING_COMMA, "%s %d:%d enum elements must be separated by comma "
          BL_YELLOW("','"), cnt->unit->filepath, tok->line, tok->col + tok->len);
      }
    }


    /* eat '}' */
    tok = bl_tokens_consume(cnt->tokens);
    if (tok->sym != BL_SYM_RBLOCK) {
      parse_error(cnt, BL_ERR_EXPECTED_BODY_END, "%s %d:%d expected end of enum body "
        BL_YELLOW("'}'"), cnt->unit->filepath, tok->line, tok->col + tok->len);
    }

    /*
     * Validate and store into scope cache.
     */
    bl_scope_t *scope      = &cnt->unit->scope;
    bl_node_t  *conflicted = bl_scope_add(scope, enm);
    if (conflicted != NULL) {
      parse_error(cnt,
                  BL_ERR_DUPLICATE_SYMBOL,
                  "%s %d:%d "
                    BL_YELLOW("'%s'")
                    " already declared here: %d:%d",
                  cnt->unit->filepath,
                  enm->line,
                  enm->col,
                  enm->value.decl.ident.name,
                  conflicted->line,
                  conflicted->col);
    }
  }

  return enm;
}

bl_node_t *
parse_struct_decl(context_t *cnt)
{
  bl_node_t  *strct  = NULL;
  bl_node_t  *member = NULL;
  bl_token_t *tok;

  tok = bl_tokens_consume_if(cnt->tokens, BL_SYM_STRUCT);
  if (tok) {
    if (cnt->modif) {
      parse_error(cnt,
                  BL_ERR_UNEXPECTED_MODIF,
                  "%s %d:%d structure cannot be declared as "
                    BL_YELLOW("%s"),
                  cnt->unit->filepath,
                  cnt->modif->line,
                  cnt->modif->col,
                  bl_sym_strings[cnt->modif->sym]);
    }

    tok = bl_tokens_consume(cnt->tokens);
    if (tok->sym != BL_SYM_IDENT) {
      parse_error(cnt,
                  BL_ERR_EXPECTED_NAME,
                  "%s %d:%d expected structure name",
                  cnt->unit->filepath,
                  tok->line,
                  tok->col);
    }

    strct = new_node(cnt, BL_NODE_STRUCT_DECL, tok);

    bl_type_init(&strct->value.decl.type, tok->value.as_string);
    /* TODO: chose what will describe structure type */
    bl_ident_init(&strct->value.decl.ident, tok->value.as_string);
    strct->value.decl.modificator = BL_SYM_NONE;

    /* eat '{' */
    tok = bl_tokens_consume(cnt->tokens);
    if (tok->sym != BL_SYM_LBLOCK) {
      parse_error(cnt, BL_ERR_EXPECTED_BODY, "%s %d:%d expected struct body "
        BL_YELLOW("'{'"), cnt->unit->filepath, tok->line, tok->col);
    }

    int order = 0;
    bl_scope_push(&cnt->unit->scope);
member:
    /* eat ident */
    member = parse_var_decl(cnt);
    if (bl_node_struct_decl_add_member(strct, member)) {
      member->value.var_decl.order = order++;

      if (bl_tokens_consume_if(cnt->tokens, BL_SYM_COMMA)) {
        goto member;
      } else if (bl_tokens_peek(cnt->tokens)->sym != BL_SYM_RBLOCK) {
        tok = bl_tokens_consume(cnt->tokens);
        parse_error(cnt, BL_ERR_MISSING_COMMA, "%s %d:%d struct members must be separated by comma "
          BL_YELLOW("','"), cnt->unit->filepath, tok->line, tok->col + tok->len);
      }
    }
    bl_scope_pop(&cnt->unit->scope);

    /* eat '}' */
    tok = bl_tokens_consume(cnt->tokens);
    if (tok->sym != BL_SYM_RBLOCK) {
      parse_error(cnt, BL_ERR_EXPECTED_BODY_END, "%s %d:%d expected end of struct body "
        BL_YELLOW("'}'"), cnt->unit->filepath, tok->line, tok->col + tok->len);
    }

    /*
     * Validate and store into scope cache.
     */
    bl_scope_t *scope      = &cnt->unit->scope;
    bl_node_t  *conflicted = bl_scope_add(scope, strct);
    if (conflicted != NULL) {
      parse_error(cnt,
                  BL_ERR_DUPLICATE_SYMBOL,
                  "%s %d:%d "
                    BL_YELLOW("'%s'")
                    " already declared here: %d:%d",
                  cnt->unit->filepath,
                  strct->line,
                  strct->col,
                  strct->value.decl.ident.name,
                  conflicted->line,
                  conflicted->col);
    }
  }

  return strct;
}

bl_node_t *
parse_expr(context_t *cnt)
{
  return parse_expr_1(cnt, parse_atom_expr(cnt), 0);
}

bl_node_t *
parse_atom_expr(context_t *cnt)
{
  bl_node_t *expr = NULL;

  bl_token_t *tok = bl_tokens_peek(cnt->tokens);

  if (bl_tokens_previous_is(cnt->tokens, BL_SYM_DOT)) {
    bl_tokens_consume(cnt->tokens);
    expr = new_node(cnt, BL_NODE_MEMBER_EXPR, tok);
    bl_ident_init(&expr->value.member_expr.ident, tok->value.as_string);

    bo_array_push_back(cnt->unit->unsatisfied, expr);
    return expr;
  }

  switch (tok->sym) {
    case BL_SYM_LPAREN:
      /* parse sub-expression in (...) */

      /* eat ( */
      bl_tokens_consume(cnt->tokens);
      expr = parse_expr(cnt);
      if (expr == NULL) {
        parse_error(cnt,
                    BL_ERR_EXPECTED_EXPR,
                    "%s %d:%d expected expression.",
                    cnt->unit->filepath,
                    tok->line,
                    tok->col);
      }

      /* eat ) */
      tok = bl_tokens_consume(cnt->tokens);
      if (tok->sym != BL_SYM_RPAREN) {
        parse_error(cnt, BL_ERR_MISSING_BRACKET, "%s %d:%d unterminated sub-expression, missing "
          BL_YELLOW("')'"), cnt->unit->filepath, tok->line, tok->col);
      }

      break;
    case BL_SYM_IDENT:
      expr = parse_call_expr(cnt);
      if (expr)
        break;

      expr = parse_dec_ref_expr(cnt);
      break;
    case BL_SYM_FLOAT:
      bl_tokens_consume(cnt->tokens);

      expr = new_node(cnt, BL_NODE_CONST_EXPR, tok);
      expr->value.const_expr.value.as_float = tok->value.as_float;
      expr->value.const_expr.type           = BL_CONST_FLOAT;
      break;
    case BL_SYM_DOUBLE:
      bl_tokens_consume(cnt->tokens);

      expr = new_node(cnt, BL_NODE_CONST_EXPR, tok);
      expr->value.const_expr.value.as_double = tok->value.as_double;
      expr->value.const_expr.type            = BL_CONST_DOUBLE;
      break;
    case BL_SYM_NUM:
      bl_tokens_consume(cnt->tokens);

      expr = new_node(cnt, BL_NODE_CONST_EXPR, tok);
      expr->value.const_expr.value.as_ulong = tok->value.as_ull;
      expr->value.const_expr.type           = BL_CONST_INT;
      break;
    case BL_SYM_TRUE:
      bl_tokens_consume(cnt->tokens);

      expr = new_node(cnt, BL_NODE_CONST_EXPR, tok);
      expr->value.const_expr.value.as_bool = true;
      expr->value.const_expr.type          = BL_CONST_BOOL;
      break;
    case BL_SYM_FALSE:
      bl_tokens_consume(cnt->tokens);

      expr = new_node(cnt, BL_NODE_CONST_EXPR, tok);
      expr->value.const_expr.value.as_bool = false;
      expr->value.const_expr.type          = BL_CONST_BOOL;
      break;
    case BL_SYM_STRING:
      bl_tokens_consume(cnt->tokens);

      expr = new_node(cnt, BL_NODE_CONST_EXPR, tok);
      expr->value.const_expr.value.as_string = tok->value.as_string;
      expr->value.const_expr.type            = BL_CONST_STRING;
      break;
    case BL_SYM_CHAR:
      bl_tokens_consume(cnt->tokens);

      expr = new_node(cnt, BL_NODE_CONST_EXPR, tok);
      expr->value.const_expr.value.as_char = tok->value.as_char;
      expr->value.const_expr.type          = BL_CONST_CHAR;
      break;
    default:
      break;
  }

  return expr;
}

bl_node_t *
parse_expr_1(context_t *cnt,
             bl_node_t *lhs,
             int min_precedence)
{
  bl_node_t  *rhs       = NULL;
  bl_token_t *lookahead = bl_tokens_peek(cnt->tokens);
  bl_token_t *op        = NULL;

  while (bl_token_prec(lookahead) >= min_precedence) {
    op = lookahead;
    bl_tokens_consume(cnt->tokens);
    rhs       = parse_atom_expr(cnt);
    lookahead = bl_tokens_peek(cnt->tokens);

    while (bl_token_prec(lookahead) > bl_token_prec(op)) {
      rhs       = parse_expr_1(cnt, rhs, bl_token_prec(lookahead));
      lookahead = bl_tokens_peek(cnt->tokens);
    }

    if (op->sym == BL_SYM_DOT) {
      rhs->value.member_expr.next = lhs;
      lhs = rhs;
    } else {
      bl_node_t *tmp = lhs;
      lhs = new_node(cnt, BL_NODE_BINOP, op);
      lhs->value.binop.lhs      = tmp;
      lhs->value.binop.rhs      = rhs;
      lhs->value.binop.operator = op->sym;
    }
  }

  return lhs;
}

bl_node_t *
parse_call_expr(context_t *cnt)
{
  bl_node_t *call_expr = NULL;

  if (bl_tokens_is_seq(cnt->tokens, 2, BL_SYM_IDENT, BL_SYM_LPAREN)) {
    bl_token_t *tok_calle = bl_tokens_consume(cnt->tokens);

    /* eat '(' */
    bl_tokens_consume(cnt->tokens);

    call_expr = new_node(cnt, BL_NODE_CALL_EXPR, tok_calle);

    bl_ident_init(&call_expr->value.call_expr.ident, tok_calle->value.as_string);

    /*
     * Call expression is stored as unsatisfied here because callee may
     * not exist yet and it's declared later in current unit or in another
     * unit. The linker will handle connecting between expression and callee.
     */
    bo_array_push_back(cnt->unit->unsatisfied, call_expr);

arg:
    bl_node_call_expr_add_arg(call_expr, parse_expr(cnt));
    if (bl_tokens_consume_if(cnt->tokens, BL_SYM_COMMA))
      goto arg;

    bl_token_t *tok = bl_tokens_consume(cnt->tokens);
    if (tok->sym != BL_SYM_RPAREN) {
      parse_error(cnt, BL_ERR_MISSING_BRACKET, "%s %d:%d expected "
        BL_YELLOW("')'")
        " after function call argument list", cnt->unit->filepath, tok->line, tok->col);
    }
  }

  return call_expr;
}

bl_node_t *
parse_dec_ref_expr(context_t *cnt)
{
  bl_node_t  *expr;
  bl_token_t *tok = bl_tokens_consume(cnt->tokens);

  expr            = new_node(cnt, BL_NODE_DECL_REF_EXPR, tok);
  bl_ident_init(&expr->value.decl_ref_expr.ident, tok->value.as_string);

  /*
   * Validate and store into scope cache.
   */
  bl_scope_t *scope = &cnt->unit->scope;
  bl_node_t  *ref   = bl_scope_get(scope, &expr->value.decl_ref_expr.ident);

  if (ref == NULL) {
    parse_error(cnt,
                BL_ERR_UNKNOWN_SYMBOL,
                "%s %d:%d can't resolve variable "
                  BL_YELLOW("'%s'"),
                cnt->unit->filepath,
                expr->line,
                expr->col,
                expr->value.decl_ref_expr.ident.name);
  }

  expr->value.decl_ref_expr.ref = ref;

  return expr;
}

bl_node_t *
parse_var_decl(context_t *cnt)
{
  bl_node_t *vdcl = NULL;
  bl_sym_e  modif = BL_SYM_NONE;

  if (bl_tokens_is_seq(cnt->tokens, 2, BL_SYM_IDENT, BL_SYM_IDENT)) {
    if (cnt->modif) {
      switch (cnt->modif->sym) {
        case BL_SYM_CONST:
          modif = cnt->modif->sym;
          break;
        default: {
          parse_error(cnt,
                      BL_ERR_UNEXPECTED_MODIF,
                      "%s %d:%d variable declaration cannot be "
                        BL_YELLOW("%s"),
                      cnt->unit->filepath,
                      cnt->modif->line,
                      cnt->modif->col,
                      bl_sym_strings[cnt->modif->sym]);
        }
      }
    }

    bl_token_t *tok_type  = bl_tokens_consume(cnt->tokens);
    bl_token_t *tok_ident = bl_tokens_consume(cnt->tokens);

    vdcl = new_node(cnt, BL_NODE_VAR_DECL, tok_ident);

    bl_type_init(&vdcl->value.var_decl.base.type, tok_type->value.as_string);
    bl_ident_init(&vdcl->value.var_decl.base.ident, tok_ident->value.as_string);

    /*
     * Validate and store into scope cache.
     */
    bl_scope_t *scope      = &cnt->unit->scope;
    bl_node_t  *conflicted = bl_scope_add(scope, vdcl);
    if (conflicted != NULL) {
      parse_error(cnt,
                  BL_ERR_DUPLICATE_SYMBOL,
                  "%s %d:%d "
                    BL_YELLOW("'%s'")
                    " already declared here: %d:%d",
                  cnt->unit->filepath,
                  vdcl->line,
                  vdcl->col,
                  vdcl->value.decl.ident.name,
                  conflicted->line,
                  conflicted->col);
    }

    if (bl_tokens_consume_if(cnt->tokens, BL_SYM_ASIGN)) {
      /*
       * Variable is also asigned to some expression.
       */

      /* expected expression */
      bl_node_t *expr = parse_expr(cnt);
      if (expr) {
        vdcl->value.var_decl.expr = expr;
      } else {
        parse_error(cnt, BL_ERR_EXPECTED_EXPR, "%s %d:%d expected expression after "
          BL_YELLOW("'='"), cnt->unit->filepath, tok_ident->line, tok_ident->col + tok_ident->len);
      }
    }

    vdcl->value.decl.modificator = modif;
  }

  return vdcl;
}

bl_error_e
bl_parser_run(bl_builder_t *builder,
              bl_unit_t *unit)
{
  context_t
    cnt =
    {.builder = builder, .unit = unit, .tokens = &unit->tokens, .is_loop = false, .modif = NULL};

  int error = 0;
  if ((error = setjmp(cnt.jmp_error))) {
    return (bl_error_e) error;
  }

  unit->ast.root = parse_global_stmt(&cnt);

  return BL_NO_ERR;
}
