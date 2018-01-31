//*****************************************************************************
// bl
//
// File:   unit.h
// Author: Martin Dorazil
// Date:   26.1.18
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

#ifndef UNIT_H_FC53HXPA
#define UNIT_H_FC53HXPA

#include <bobject/bobject.h>
#include "errors.h"
#include "tokens.h"
#include "pnode.h"

/* class Unit declaration */
bo_decl_type_begin(Unit, BObject)
  /* virtuals */
bo_end();

/* class Unit object members */
bo_decl_members_begin(Unit, BObject)
  /* members */
  BString *filepath;
  BString *src;
  Tokens  *tokens;
  Pnode   *proot;

  /* error */
  bl_err   last_err;
bo_end();

Unit *
bl_unit_new(const char *filepath);

bool
bl_unit_compile(Unit *self);

bl_err
bl_unit_last_error(Unit *self);

#endif /* end of include guard: UNIT_H_FC53HXPA */
