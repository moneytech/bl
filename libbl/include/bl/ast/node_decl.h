//*****************************************************************************
// bl
//
// File:   node_decl.h
// Author: Martin Dorazil
// Date:   11/02/2018
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

#ifndef BL_NODE_DECL_H
#define BL_NODE_DECL_H

#include <bobject/bobject.h>
#include "bl/ast/node.h"
#include "bl/type.h"
#include "bl/identifier.h"

BO_BEGIN_DECLS

/* class NodeDecl declaration */
bo_decl_type_begin(NodeDecl, Node)
  /* virtuals */
bo_end();

extern BO_EXPORT Ident *
bl_node_decl_get_ident(NodeDecl *self);

extern BO_EXPORT Type *
bl_node_decl_get_type(NodeDecl *self);

BO_END_DECLS

#endif //BL_NODE_DECL_H