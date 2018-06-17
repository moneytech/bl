//************************************************************************************************
// bl
//
// File:   visitor_impl.h
// Author: Martin Dorazil
// Date:   3/15/18
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

#ifndef VISITOR_IMPL_H_0IZSKUFY
#define VISITOR_IMPL_H_0IZSKUFY

#include "ast_impl.h"

typedef struct bl_visitor bl_visitor_t;

#define BL_SKIP_VISIT NULL

typedef enum {
  BL_VISIT_MODULE,
  BL_VISIT_FUNC,
  BL_VISIT_TYPE,
  BL_VISIT_ARG,
  BL_VISIT_STRUCT,
  BL_VISIT_STRUCT_MEMBER,
  BL_VISIT_ENUM,
  BL_VISIT_ENUM_VARIANT,
  BL_VISIT_MUT,
  BL_VISIT_CONST,
  BL_VISIT_BLOCK,
  BL_VISIT_EXPR,
  BL_VISIT_IF,
  BL_VISIT_LOOP,
  BL_VISIT_BREAK,
  BL_VISIT_CONTINUE,
  BL_VISIT_RETURN,
  BL_VISIT_SIZEOF,
  BL_VISIT_LOAD,
  BL_VISIT_LINK,
  BL_VISIT_USING,
  BL_VISIT_COUNT
} bl_visit_e;

typedef void (*bl_visit_f)(bl_visitor_t *visitor, bl_node_t **module);

struct bl_visitor
{
  bl_visit_f visitors[BL_VISIT_COUNT];
  void *context;
  int   nesting;
};

void
bl_visitor_init(bl_visitor_t *visitor, void *context);

void
bl_visitor_add(bl_visitor_t *visitor, bl_visit_f visit, bl_visit_e type);

void
bl_visitor_walk_gscope(bl_visitor_t *visitor, bl_node_t **root);

void
bl_visitor_walk_module(bl_visitor_t *visitor, bl_node_t **module);

void
bl_visitor_walk_func(bl_visitor_t *visitor, bl_node_t **func);

void
bl_visitor_walk_type(bl_visitor_t *visitor, bl_node_t **type);

void
bl_visitor_walk_arg(bl_visitor_t *visitor, bl_node_t **arg);

void
bl_visitor_walk_struct(bl_visitor_t *visitor, bl_node_t **strct);

void
bl_visitor_walk_struct_member(bl_visitor_t *visitor, bl_node_t **member);

void
bl_visitor_walk_enum(bl_visitor_t *visitor, bl_node_t **enm);

void
bl_visitor_walk_enum_variant(bl_visitor_t *visitor, bl_node_t **variant);

void
bl_visitor_walk_mut(bl_visitor_t *visitor, bl_node_t **mut);

void
bl_visitor_walk_const(bl_visitor_t *visitor, bl_node_t **cnst);

void
bl_visitor_walk_block(bl_visitor_t *visitor, bl_node_t **block);

void
bl_visitor_walk_expr(bl_visitor_t *visitor, bl_node_t **expr);

void
bl_visitor_walk_if(bl_visitor_t *visitor, bl_node_t **if_stmt);

void
bl_visitor_walk_if_true(bl_visitor_t *visitor, bl_node_t **if_stmt);

void
bl_visitor_walk_if_false(bl_visitor_t *visitor, bl_node_t **if_stmt);

void
bl_visitor_walk_loop(bl_visitor_t *visitor, bl_node_t **stmt_loop);

void
bl_visitor_walk_loop_body(bl_visitor_t *visitor, bl_node_t **stmt_loop);

void
bl_visitor_walk_break(bl_visitor_t *visitor, bl_node_t **stmt_break);

void
bl_visitor_walk_continue(bl_visitor_t *visitor, bl_node_t **stmt_continue);

void
bl_visitor_walk_return(bl_visitor_t *visitor, bl_node_t **stmt_return);

void
bl_visitor_walk_load(bl_visitor_t *visitor, bl_node_t **pre_load);

void
bl_visitor_walk_link(bl_visitor_t *visitor, bl_node_t **pre_link);

void
bl_visitor_walk_using(bl_visitor_t *visitor, bl_node_t **using);

#endif /* end of include guard: VISITOR_IMPL_H_0IZSKUFY */
