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

#include "parser.h"
#include "domains.h"
#include "unit.h"
#include "bldebug.h"
#include "ast.h"

static Node *
parse_global_stmt(Parser *self, 
                  Unit *unit);
static Node *
parse_func_decl(Parser *self, 
                Unit *unit);

static bool
run(Parser *self,
    Unit   *unit);

static int
domain(Parser *self);

/* Parser members */
bo_decl_members_begin(Parser, Stage)
bo_end();

/* Parser constructor parameters */
bo_decl_params_begin(Parser)
bo_end();

bo_impl_type(Parser, Stage);

/* Parser class init */
void
ParserKlass_init(ParserKlass *klass)
{
  bo_vtbl_cl(klass, Stage)->run 
    = (bool (*)(Stage*, Actor *)) run;
  bo_vtbl_cl(klass, Stage)->domain
    = (int (*)(Stage*)) domain;
}

/* Parser constructor */
void
Parser_ctor(Parser *self, ParserParams *p)
{
}

/* Parser destructor */
void
Parser_dtor(Parser *self)
{
}

/* Parser copy constructor */
bo_copy_result
Parser_copy(Parser *self, Parser *other)
{
  return BO_NO_COPY;
}

Node *
parse_global_stmt(Parser *self, 
                  Unit *unit)
{
  NodeGlobalStmt *gstmt = bl_node_global_stmt_new(bo_string_get(unit->src), 1, 0);
  return (Node *)gstmt;
}

Node *
parse_func_decl(Parser *self, 
                Unit *unit)
{ 
  return NULL;
}

bool
run(Parser *self,
    Unit   *unit)
{
  if (unit->tokens == NULL) {
    bl_actor_error((Actor *)unit, "no tokens found for unit");
    return false;
  }

  bo_unref(unit->ast);
  unit->ast = parse_global_stmt(self, unit);
  bl_log("* parsing done\n");
  return true;
}

int
domain(Parser *self)
{
  return BL_DOMAIN_UNIT;
}

/* public */
Parser *
bl_parser_new(void)
{
  return bo_new(Parser, NULL);
}

/* public */
