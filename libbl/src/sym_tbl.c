//*****************************************************************************
// bl
//
// File:   sym_tbl.c
// Author: Martin Dorazil
// Date:   13/02/2018
//
// Copyright 2017 Martin Dorazil
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

#include <bobject/containers/htbl.h>
#include <bobject/containers/array.h>
#include "bl/sym_tbl.h"
#include "bl/bldebug.h"

#define EXPECTED_SYM_COUNT 512

/* class SymTbl */
/* class SymTbl object members */
bo_decl_members_begin(SymTbl, BObject)
  /* members */
  BHashTable *syms;
  BArray *unsatisfied;
bo_end();

bo_impl_type(SymTbl, BObject);

void
SymTblKlass_init(SymTblKlass *klass)
{
}

void
SymTbl_ctor(SymTbl *self,
            SymTblParams *p)
{
  /* constructor */
  self->syms = bo_htbl_new_bo(bo_typeof(NodeDecl), false, EXPECTED_SYM_COUNT);
  self->unsatisfied = bo_array_new_bo(bo_typeof(NodeDecl), false);
}

void
SymTbl_dtor(SymTbl *self)
{
  bo_unref(self->syms);
  bo_unref(self->unsatisfied);
}

bo_copy_result
SymTbl_copy(SymTbl *self,
            SymTbl *other)
{
  return BO_NO_COPY;
}

/* class SymTbl end */

SymTbl *
bl_sym_tbl_new(void)
{
  return bo_new(SymTbl, NULL);
}

bool
bl_sym_tbl_register(SymTbl *self,
                    NodeDecl *node)
{
  bl_assert(node, "invalid node");
  uint32_t hash = bl_ident_get_hash(bl_node_decl_get_ident(node));

  if (bo_htbl_has_key(self->syms, hash))
    return false;

  bo_htbl_insert(self->syms, hash, node);
  return true;
}

NodeDecl *
bl_sym_tbl_get_sym_of_type(SymTbl *self,
                           Ident *ident,
                           bl_node_e type)
{
  bl_assert(ident, "invalid identifier");
  bo_iterator_t iter = bo_htbl_find(self->syms, bl_ident_get_hash(ident));
  bo_iterator_t end = bo_htbl_end(self->syms);
  if (bo_iterator_equal(&iter, &end))
    return NULL;

  NodeDecl *ret = bo_htbl_iter_peek_value(self->syms, &iter, NodeDecl *);
  if (bl_node_get_type((Node *) ret) != type)
    return NULL;

  return ret;
}

void
bl_sym_tbl_add_unsatisfied_expr(SymTbl *self,
                                NodeCall *expr)
{
  bl_assert(expr, "invalid expression");
  bo_array_push_back(self->unsatisfied, expr);
}

bool
bl_sym_tbl_try_satisfy_all(SymTbl *self)
{
  size_t c = bo_array_size(self->unsatisfied);
  size_t i = 0;
  bool ret = true;
  bool erase = false;
  NodeExpr *expr = NULL;
  NodeDecl *decl = NULL;
  Ident *ident = NULL;

  while (i < c) {
    expr = bo_array_at(self->unsatisfied, i, NodeExpr *);

    switch (bl_node_get_type((Node *) expr)){
      case BL_NODE_CALL:
        ident = bl_node_call_get_ident((NodeCall *) expr);
        decl = bl_sym_tbl_get_sym_of_type(self, ident, BL_NODE_FUNC_DECL);
        if (decl) {
          bl_node_call_get_set_callee((NodeCall *) expr, (NodeFuncDecl *) decl);
          erase = true;
        }
        break;
      default:
        bl_abort("cannot satisfy this type");
    }

    /*
     * Delete when call was satisfied.
     */
    if (erase) {
      bo_array_erase(self->unsatisfied, i);
      --c;
      erase = false;
    } else {
      i++;
      ret = false;
    }
  }

  return ret;
}