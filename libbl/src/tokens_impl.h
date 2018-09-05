//************************************************************************************************
// blc
//
// File:   tokens.h
// Author: Martin Dorazil
// Date:   29.1.18
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

#ifndef TOKENS_H_Z3NM7BJC
#define TOKENS_H_Z3NM7BJC

#include <bobject/bobject.h>
#include <bobject/containers/string.h>
#include <bobject/containers/array.h>
#include "token_impl.h"

typedef struct bl_tokens
{
  BArray *buf;
  BArray *string_cache;
  size_t iter;
  size_t marker;
} tokens_t;

void
bl_tokens_init(tokens_t *tokens);

void
bl_tokens_terminate(tokens_t *tokens);

BString *
bl_tokens_create_cached_str(tokens_t *tokens);

int
bl_tokens_count(tokens_t *tokens);

void
bl_tokens_push(tokens_t *tokens, token_t *t);

token_t *
bl_tokens_peek(tokens_t *tokens);

token_t *
bl_tokens_peek_last(tokens_t *tokens);

token_t *
bl_tokens_peek_2nd(tokens_t *tokens);

token_t *
bl_tokens_peek_nth(tokens_t *tokens, size_t n);

token_t *
bl_tokens_peek_prev(tokens_t *tokens);

token_t *
bl_tokens_consume(tokens_t *tokens);

token_t *
bl_tokens_consume_if(tokens_t *tokens, sym_e sym);

bool
bl_tokens_current_is(tokens_t *tokens, sym_e sym);

bool
bl_tokens_previous_is(tokens_t *tokens, sym_e sym);

bool
bl_tokens_next_is(tokens_t *tokens, sym_e sym);

bool
bl_tokens_current_is_not(tokens_t *tokens, sym_e sym);

bool
bl_tokens_next_is_not(tokens_t *tokens, sym_e sym);

bool
bl_tokens_is_seq(tokens_t *tokens, int cnt, ...);

void
bl_tokens_reset_iter(tokens_t *tokens);

void
bl_tokens_set_marker(tokens_t *tokens);

void
bl_tokens_back_to_marker(tokens_t *tokens);

void
bl_tokens_consume_till(tokens_t *tokens, sym_e sym);

BArray *
bl_tokens_get_all(tokens_t *tokens);

#endif /* end of include guard: TOKENS_H_Z3NM7BJC */
