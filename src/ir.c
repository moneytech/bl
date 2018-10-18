//************************************************************************************************
// bl
//
// File:   ir.c
// Author: Martin Dorazil
// Date:   12/7/18
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

#include "ir.h"
#include "ast.h"
#include "arena.h"

#define ARENA_CHUNK_COUNT 512
#define VERBOSE 0

typedef struct
{
  Builder * builder;
  Assembly *assembly;
} Context;

/* public */
void
ir_arena_init(Arena *arena)
{
  arena_init(arena, sizeof(Ir), ARENA_CHUNK_COUNT, NULL);
}

Ir *
ir_create(Arena *arena, IrCode c)
{
  Ir *ir = arena_alloc(arena);
  ir->code = c;
  return ir;
}

void
ir_run(Builder *builder, Assembly *assembly)
{
  Context cnt = {.builder = builder, .assembly = assembly};

  bl_log("ir run");
}
