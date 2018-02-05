//*****************************************************************************
// bl 
//
// File:   node_global_stmt.c
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

#include <bobject/containers/array.h>
#include "node_global_stmt.h"

static BString *
to_string(NodeGlobalStmt *self);

/* NodeGlobalStmt members */
bo_decl_members_begin(NodeGlobalStmt, Node)
bo_end();

/* NodeGlobalStmt constructor parameters */
bo_decl_params_with_base_begin(NodeGlobalStmt, Node)
bo_end();

bo_impl_type(NodeGlobalStmt, Node);

/* NodeGlobalStmt class init */
void
NodeGlobalStmtKlass_init(NodeGlobalStmtKlass *klass)
{
  bo_vtbl_cl(klass, Node)->to_string 
    = (BString *(*)(Node*)) to_string;
}

/* NodeGlobalStmt constructor */
void
NodeGlobalStmt_ctor(NodeGlobalStmt *self, NodeGlobalStmtParams *p)
{
  bo_parent_ctor(Node, p);
}

/* NodeGlobalStmt destructor */
void
NodeGlobalStmt_dtor(NodeGlobalStmt *self)
{
}

/* NodeGlobalStmt copy constructor */
bo_copy_result
NodeGlobalStmt_copy(NodeGlobalStmt *self, NodeGlobalStmt *other)
{
  return BO_NO_COPY;
}

BString *
to_string(NodeGlobalStmt *self)
{
  BString *ret = bo_string_new(128);
  bo_string_append(ret, "<");
  bo_string_append(ret, bl_node_strings[bo_members(self, Node)->type]);
  bo_string_append(ret, ">");
  return ret;
}

/* public */

NodeGlobalStmt *
bl_node_global_stmt_new(const char *generated_from,
                        int         line,
                        int         col)
{
  NodeGlobalStmtParams p = {
    .base.type = BL_NODE_GLOBAL_STMT,
    .base.generated_from = generated_from, 
    .base.line = line, 
    .base.col = col,
  };
  
  return bo_new(NodeGlobalStmt, &p);
}

