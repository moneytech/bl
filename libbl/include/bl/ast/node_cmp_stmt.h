//*****************************************************************************
// blc
//
// File:   node_cmp_stmt.h
// Author: Martin Dorazil
// Date:   8.2.18
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

#ifndef BL_NODE_CMP_STMT_H
#define BL_NODE_CMP_STMT_H

#include "bl/ast/node.h"

BO_BEGIN_DECLS
/* class declaration */
bo_decl_type_begin(NodeCmpStmt, Node)
  /* virtuals */
bo_end();

extern BO_EXPORT bool
bl_node_stmt_add_child(NodeCmpStmt *self,
                       Node *node);

extern BO_EXPORT int
bl_node_stmt_child_get_count(NodeCmpStmt *self);

extern BO_EXPORT Node *
bl_node_stmt_get_child(NodeCmpStmt *self,
                       int i);

BO_END_DECLS

#endif //BL_NODE_STMT_H