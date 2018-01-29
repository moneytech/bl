//*****************************************************************************
// bl
//
// File:   pnode.h
// Author: Martin Dorazil
// Date:   26/01/2018
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

#ifndef BL_PNODE_H
#define BL_PNODE_H

#include <bobject/bobject.h>
#include <bobject/containers/array.h>
#include "token.h"

typedef enum _bl_ptype {
  BL_PT_GSCOPE,
  BL_PT_SCOPE,
  BL_PT_METHOD,
  BL_PT_CALL,
  BL_PT_DECL,
  BL_PT_ATTRIBUTE
} bl_ptype_e;

typedef struct _bl_pt_decl {
  bl_token_t *type;
  bl_token_t *name;
} bl_pt_decl;

typedef struct _bl_pt_attribute {
  bl_token_t *header;
  bl_token_t *entry_point;
} bl_pt_attribute;

typedef struct _bl_pt_method {
  bl_token_t *ret;
  bl_token_t *name;
  bl_token_t *modif;
} bl_pt_method;

typedef struct _bl_pt_call {
  bl_token_t *name;
  bl_token_t *param1; // TODO: implement more params
} bl_pt_call;


/* class Pnode declaration */
bo_decl_type_begin(Pnode, BObject)
  /* virtuals */
bo_end();

/* class Pnode object members */
bo_decl_members_begin(Pnode, BObject)
  /* members */
  BArray    *nodes;
  bl_ptype_e type;
  union _content {
    bl_pt_method    as_method;
    bl_pt_call      as_call;
    bl_pt_decl      as_decl;
    bl_pt_attribute as_attribute;
  } content;
bo_end();

Pnode *
bl_pnode_new(bl_ptype_e type);

#endif //BL_PNODE_H
