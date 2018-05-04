//************************************************************************************************
// Biscuit Engine
//
// File:   assembly_impl.h
// Author: Martin Dorazil
// Date:   02/03/2018
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

#ifndef BISCUIT_ASSEMBLY_IMPL_H
#define BISCUIT_ASSEMBLY_IMPL_H

#include <bobject/containers/array.h>
#include <bobject/containers/htbl.h>
#include <llvm-c/Core.h>
#include "bl/assembly.h"
#include "scope_impl.h"

typedef struct bl_assembly
{
  BArray *          units;
  BHashTable *      unique_cache;
  char *            name;
  bl_scope_t *      scope;
  bl_scope_cache_t *scope_cache;
  LLVMModuleRef     llvm_module;
  LLVMContextRef    llvm_cnt;
} bl_assembly_t;

#endif /* end of include guard: BISCUIT_ASSEMBLY_IMPL_H */
