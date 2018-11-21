//************************************************************************************************
// bl
//
// File:   mir.c
// Author: Martin Dorazil
// Date:   3/15/18
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

#include "mir.h"
#include "unit.h"
#include "common.h"
#include "builder.h"
#include "assembly.h"
#include "mir_printer.h"

#define ARENA_CHUNK_COUNT 512

/* Pipeline:
 *   - generate first pass from AST with unknown types
 *   - generate implicit type resolvers
 *   - analyze and execute type resolvers
 *   - analyze rest of code and also execute compile time executables
 */

/* TODO: this is temporary solution, we need some kind of fast allocator for different instructions
 * with different size (we allocate pool where every element has size of biggest instruction -> we
 * are wastig memory) */
union _MirInstr
{
  MirInstrDeclVar      var;
  MirInstrConst        cnst;
  MirInstrLoad         load;
  MirInstrStore        store;
  MirInstrRet          ret;
  MirInstrBinop        binop;
  MirInstrValidateType validate_type;
  MirInstrFnProto      fn_proto;
  MirInstrCall         call;
};

typedef enum
{
  BUILDIN_TYPE_NONE = -1,
  BUILDIN_TYPE_S32,
  BUILDIN_TYPE_U32,
  _BUILDIN_TYPE_COUNT,
} BuildinType;

typedef enum
{
  PASS_GENERATE,
  PASS_ANALYZE
} Pass;

typedef struct
{
  Builder *  builder;
  Assembly * assembly;
  MirArenas *arenas;
  BArray *   globals;

  struct
  {
    BHashTable *table;

    MirType *entry_type;
    MirType *entry_s32;
    MirType *entry_u32;
    MirType *entry_void;
    MirType *entry_resolve_type_fn;
  } buildin_types;

  struct
  {
    MirBlock *block;
    unsigned  id_counter;
  } cursor;

  LLVMContextRef    llvm_cnt;
  LLVMModuleRef     llvm_module;
  LLVMTargetDataRef llvm_td;

  bool verbose;
  bool executing;
  Pass pass;
} Context;

static const char *buildin_type_names[_BUILDIN_TYPE_COUNT] = {"s32", "u32"};

static void
block_dtor(MirBlock *block)
{
  bo_unref(block->instructions);
}

static void
exec_dtor(MirExec *exec)
{
  bo_unref(exec->blocks);
}

static void
instr_dtor(MirInstr *instr)
{
  if (instr->kind == MIR_INSTR_FN_PROTO) bo_unref(((MirInstrFnProto *)instr)->arg_types);
}

static void
type_dtor(MirType *type)
{
  switch (type->kind) {
  case MIR_TYPE_FN:
    bo_unref(type->data.fn.arg_types);
    break;
  default:
    break;
  }
}

static inline BuildinType
is_buildin_type(Context *cnt, const uint64_t hash)
{
  if (!bo_htbl_has_key(cnt->buildin_types.table, hash)) return BUILDIN_TYPE_NONE;
  return bo_htbl_at(cnt->buildin_types.table, hash, BuildinType);
}

static inline MirType *
get_buildin(Context *cnt, BuildinType id)
{
  switch (id) {
  case BUILDIN_TYPE_S32:
    return cnt->buildin_types.entry_s32;
  case BUILDIN_TYPE_U32:
    return cnt->buildin_types.entry_u32;
  default:
    bl_abort("invalid buildin type");
  }
}

static inline void
set_cursor_block(Context *cnt, MirBlock *block)
{
  assert(block);
  cnt->cursor.id_counter = bo_array_size(block->instructions);
  cnt->cursor.block      = block;
}

static inline MirBlock *
get_cursor_block(Context *cnt)
{
  return cnt->cursor.block;
}

/* FW decls */
static bool
type_cmp(MirType *first, MirType *second);

static const char *
instr_name(MirInstr *instr);

static MirInstr *
ast(Context *cnt, Ast *node);

static void
ast_ublock(Context *cnt, Ast *ublock);

static void
ast_block(Context *cnt, Ast *block);

static void
ast_stmt_return(Context *cnt, Ast *ret);

static MirInstr *
ast_decl_entity(Context *cnt, Ast *entity);

static MirInstr *
ast_type_ref(Context *cnt, Ast *type_ref);

static MirInstr *
ast_expr_ref(Context *cnt, Ast *ref);

static MirInstr *
ast_expr_lit_int(Context *cnt, Ast *expr);

static MirInstr *
ast_expr_lit_fn(Context *cnt, Ast *lit_fn);

static MirInstr *
ast_expr_binop(Context *cnt, Ast *binop);

static LLVMTypeRef
to_llvm_type(Context *cnt, MirType *type, size_t *out_size);

/* analyze */
static MirInstr *
analyze_instr(Context *cnt, MirInstr *instr, bool execute);

static MirInstr *
analyze_instr_ret(Context *cnt, MirInstrRet *ret, bool execute);

static MirInstr *
analyze_instr_store(Context *cnt, MirInstrStore *store, bool execute);

static MirInstr *
analyze_instr_fn_proto(Context *cnt, MirInstrFnProto *fn_proto, bool execute);

static MirInstr *
analyze_instr_decl_var(Context *cnt, MirInstrDeclVar *var, bool execute);

static MirInstr *
analyze_instr_const(Context *cnt, MirInstrConst *cnst, bool execute);

static MirInstr *
analyze_instr_validate_type(Context *cnt, MirInstrValidateType *validate, bool execute);

static MirInstr *
analyze_instr_call(Context *cnt, MirInstrCall *call, bool execute);

static void
analyze(Context *cnt);

/* impl */
static MirType *
create_type_type(Context *cnt)
{
  MirType *tmp = arena_alloc(&cnt->arenas->type_arena);
  tmp->kind    = MIR_TYPE_TYPE;
  tmp->name    = "type";
  return tmp;
}

static MirType *
create_type_void(Context *cnt)
{
  MirType *tmp = arena_alloc(&cnt->arenas->type_arena);
  tmp->kind    = MIR_TYPE_VOID;
  tmp->name    = "void";
  return tmp;
}

static MirType *
create_type_int(Context *cnt, const char *name, int bitcount, bool is_signed)
{
  assert(bitcount > 0);
  MirType *tmp                = arena_alloc(&cnt->arenas->type_arena);
  tmp->kind                   = MIR_TYPE_INT;
  tmp->name                   = name;
  tmp->data.integer.bitcount  = bitcount;
  tmp->data.integer.is_signed = is_signed;

  return tmp;
}

static MirType *
create_type_ptr(Context *cnt, MirType *src_type)
{
  MirType *tmp       = arena_alloc(&cnt->arenas->type_arena);
  tmp->kind          = MIR_TYPE_PTR;
  tmp->data.ptr.next = src_type;

  return tmp;
}

static MirType *
create_type_fn(Context *cnt, MirType *ret_type, BArray *arg_types)
{
  assert(arg_types && ret_type);
  MirType *tmp           = arena_alloc(&cnt->arenas->type_arena);
  tmp->kind              = MIR_TYPE_FN;
  tmp->data.fn.arg_types = arg_types;
  tmp->data.fn.ret_type  = ret_type;

  return tmp;
}

static MirVar *
create_var(Context *cnt, Ast *name)
{
  assert(name);
  MirVar *tmp = arena_alloc(&cnt->arenas->var_arena);
  tmp->name   = name;
  return tmp;
}

static MirExec *
create_exec(Context *cnt, MirFn *owner_fn)
{
  MirExec *tmp  = arena_alloc(&cnt->arenas->exec_arena);
  tmp->blocks   = bo_array_new(sizeof(MirBlock *));
  tmp->owner_fn = owner_fn;
  return tmp;
}

static MirFn *
create_fn(Context *cnt, Ast *name)
{
  MirFn *tmp         = arena_alloc(&cnt->arenas->fn_arena);
  tmp->name          = name;
  tmp->exec          = create_exec(cnt, tmp);
  tmp->exec_analyzed = create_exec(cnt, tmp);
  return tmp;
}

static MirBlock *
append_block(Context *cnt, MirInstr *fn, const char *name)
{
  assert(fn && name);
  MirBlock *tmp     = arena_alloc(&cnt->arenas->block_arena);
  tmp->name         = name;
  tmp->instructions = bo_array_new(sizeof(MirInstr *));

  assert(fn->kind == MIR_INSTR_FN_PROTO);
  MirFn *v_fn = fn->value.data.v_fn;
  if (!v_fn) fn->value.data.v_fn = v_fn = create_fn(cnt, fn->node);

  tmp->owner_exec = cnt->pass == PASS_GENERATE ? v_fn->exec : v_fn->exec_analyzed;
  if (!tmp->owner_exec->entry_block) tmp->owner_exec->entry_block = tmp;
  bo_array_push_back(tmp->owner_exec->blocks, tmp);
  return tmp;
}

/* instructions */
static void
push_into_curr_block(Context *cnt, MirInstr *instr)
{
  assert(instr);
  assert(cnt->cursor.block);
  bo_array_push_back(cnt->cursor.block->instructions, instr);
  instr->owner_block = cnt->cursor.block;
}

#define create_instr(_cnt, _kind, _id, _node, _t)                                                  \
  ((_t)_create_instr((_cnt), (_kind), (_id), (_node)))

static MirInstr *
_create_instr(Context *cnt, MirInstrKind kind, unsigned id, Ast *node)
{
  MirInstr *tmp = arena_alloc(&cnt->arenas->instr_arena);
  tmp->kind     = kind;
  tmp->node     = node;
  tmp->id       = id;
  return tmp;
}

static MirInstr *
create_instr_call_type_resolve(Context *cnt, MirInstr *resolver_fn)
{
  assert(resolver_fn && resolver_fn->kind == MIR_INSTR_FN_PROTO);
  MirInstrCall *tmp = create_instr(cnt, MIR_INSTR_CALL, 0, NULL, MirInstrCall *);
  tmp->callee       = resolver_fn;
  tmp->comptime     = true;
  ++resolver_fn->ref_count;
  return &tmp->base;
}

static MirInstr *
add_instr_fn_proto(Context *cnt, MirInstr *ret_type, BArray *arg_types, Ast *name)
{
  const unsigned   id  = bo_array_size(cnt->globals);
  MirInstrFnProto *tmp = create_instr(cnt, MIR_INSTR_FN_PROTO, id, name, MirInstrFnProto *);
  tmp->base.kind       = MIR_INSTR_FN_PROTO;
  tmp->ret_type        = ret_type;
  tmp->arg_types       = arg_types;

  bo_array_push_back(cnt->globals, tmp);
  return &tmp->base;
}

static MirInstr *
add_instr_decl_ref(Context *cnt, Ast *node)
{
  MirInstrDeclRef *tmp =
      create_instr(cnt, MIR_INSTR_DECL_REF, cnt->cursor.id_counter++, node, MirInstrDeclRef *);
  push_into_curr_block(cnt, &tmp->base);
  return &tmp->base;
}

static MirInstr *
add_instr_call(Context *cnt, Ast *node, MirInstr *callee, BArray *args)
{
  assert(callee && callee->kind == MIR_INSTR_FN_PROTO);
  MirInstrCall *tmp =
      create_instr(cnt, MIR_INSTR_CALL, cnt->cursor.id_counter++, node, MirInstrCall *);
  tmp->args   = args;
  tmp->callee = callee;
  ++callee->ref_count;

  push_into_curr_block(cnt, &tmp->base);
  return &tmp->base;
}

static MirInstr *
add_instr_decl_var(Context *cnt, MirInstr *type, Ast *name)
{
  if (type) ++type->ref_count;
  MirInstrDeclVar *tmp =
      create_instr(cnt, MIR_INSTR_DECL_VAR, cnt->cursor.id_counter++, name, MirInstrDeclVar *);
  tmp->type = type;

  MirVar *var = create_var(cnt, name);
  tmp->var    = var;

  push_into_curr_block(cnt, &tmp->base);
  return &tmp->base;
}

static MirInstr *
add_instr_const_int(Context *cnt, Ast *node, uint64_t val)
{
  MirInstr *tmp   = create_instr(cnt, MIR_INSTR_CONST, cnt->cursor.id_counter++, node, MirInstr *);
  tmp->value.type = cnt->buildin_types.entry_s32;
  tmp->value.data.v_int = val;

  push_into_curr_block(cnt, tmp);
  return tmp;
}

static MirInstr *
add_instr_const_type(Context *cnt, Ast *node, MirType *type)
{
  MirInstr *tmp   = create_instr(cnt, MIR_INSTR_CONST, cnt->cursor.id_counter++, node, MirInstr *);
  tmp->value.type = cnt->buildin_types.entry_type;
  tmp->value.data.v_type = type;

  push_into_curr_block(cnt, tmp);
  return tmp;
}

static MirInstr *
add_instr_const(Context *cnt, Ast *node, MirValue *value)
{
  MirInstr *tmp = create_instr(cnt, MIR_INSTR_CONST, cnt->cursor.id_counter++, node, MirInstr *);
  tmp->value    = *value;

  push_into_curr_block(cnt, tmp);
  return tmp;
}

static MirInstr *
add_instr_ret(Context *cnt, Ast *node, MirInstr *value)
{
  if (value) {
    ++value->ref_count;
  }

  MirInstrRet *tmp =
      create_instr(cnt, MIR_INSTR_RET, cnt->cursor.id_counter++, node, MirInstrRet *);
  tmp->value           = value;
  tmp->base.value.type = cnt->buildin_types.entry_void;

  /* add terminate current block and for return statement terminate also executable unit */
  assert(cnt->cursor.block);
  assert(cnt->cursor.block->owner_exec);

  /* when current block is already terminated we produce unrecheable code replacement later during
   * analyze */
  if (!cnt->cursor.block->terminal) cnt->cursor.block->terminal = &tmp->base;
  ++tmp->base.ref_count;

  push_into_curr_block(cnt, &tmp->base);
  return &tmp->base;
}

static MirInstr *
add_instr_store(Context *cnt, Ast *node, MirInstr *src, MirInstr *dest)
{
  assert(src && dest);
  ++src->ref_count;
  ++dest->ref_count;
  MirInstrStore *tmp =
      create_instr(cnt, MIR_INSTR_STORE, cnt->cursor.id_counter++, node, MirInstrStore *);
  tmp->src  = src;
  tmp->dest = dest;

  push_into_curr_block(cnt, &tmp->base);
  return &tmp->base;
}

static MirInstr *
add_instr_binop(Context *cnt, Ast *node, MirInstr *lhs, MirInstr *rhs, BinopKind op)
{
  assert(lhs && rhs);
  ++lhs->ref_count;
  ++rhs->ref_count;
  MirInstrBinop *tmp =
      create_instr(cnt, MIR_INSTR_BINOP, cnt->cursor.id_counter++, node, MirInstrBinop *);
  tmp->lhs = lhs;
  tmp->rhs = rhs;
  tmp->op  = op;

  push_into_curr_block(cnt, &tmp->base);
  return &tmp->base;
}

static MirInstr *
add_instr_validate_type(Context *cnt, MirInstr *src)
{
  assert(src);
  ++src->ref_count;
  MirInstrValidateType *tmp = create_instr(cnt, MIR_INSTR_VALIDATE_TYPE, cnt->cursor.id_counter++,
                                           NULL, MirInstrValidateType *);
  tmp->src                  = src;
  tmp->base.value.type      = cnt->buildin_types.entry_void;

  push_into_curr_block(cnt, &tmp->base);
  return &tmp->base;
}

/* LLVM */
LLVMTypeRef
to_llvm_type(Context *cnt, MirType *type, size_t *out_size)
{
  if (!type) return NULL;
  LLVMTypeRef result = NULL;

  switch (type->kind) {
  case MIR_TYPE_TYPE:

  case MIR_TYPE_VOID: {
    if (out_size) *out_size = 0;
    result = LLVMVoidTypeInContext(cnt->llvm_cnt);
    break;
  }

  case MIR_TYPE_INT: {
    result = LLVMIntTypeInContext(cnt->llvm_cnt, type->data.integer.bitcount);
    if (out_size) *out_size = LLVMSizeOfTypeInBits(cnt->llvm_td, result);
    break;
  }

  case MIR_TYPE_PTR: {
    MirType *tmp = type->data.ptr.next;
    assert(tmp);
    assert(tmp->llvm_type);
    result = LLVMPointerType(tmp->llvm_type, 0);
    if (out_size) *out_size = LLVMSizeOfTypeInBits(cnt->llvm_td, result);
    break;
  }

  case MIR_TYPE_FN: {
    MirType *tmp_ret  = type->data.fn.ret_type;
    BArray * tmp_args = type->data.fn.arg_types;
    assert(tmp_ret && tmp_ret->llvm_type && tmp_args);
    const size_t cargs = bo_array_size(tmp_args);

    LLVMTypeRef *tmp_args_llvm = bl_malloc(cargs * sizeof(LLVMTypeRef));
    if (!tmp_args_llvm) bl_abort("bad alloc");

    MirType *tmp_arg;
    barray_foreach(tmp_args, tmp_arg)
    {
      assert(tmp_arg->llvm_type);
      tmp_args_llvm[i] = tmp_arg->llvm_type;
    }

    result = LLVMFunctionType(tmp_ret->llvm_type, tmp_args_llvm, cargs, false);
    if (out_size) *out_size = 0;
    bl_free(tmp_args_llvm);
    break;
  }

  default:
    bl_abort("unimplemented");
  }

  return result;
}

bool
type_cmp(MirType *first, MirType *second)
{
  assert(first && second);
  if (first->kind != second->kind) return false;
  /* TODO */
  return true;
}

/* analyze */
MirInstr *
analyze_instr_fn_proto(Context *cnt, MirInstrFnProto *fn_proto, bool execute)
{
  MirFn *fn = fn_proto->base.value.data.v_fn;
  if (!fn || fn->analyzed) return &fn_proto->base;

  MirBlock *prev_block  = get_cursor_block(cnt);
  MirBlock *entry_block = append_block(cnt, &fn_proto->base, "entry");
  set_cursor_block(cnt, entry_block);

  MirExec *exec = fn_proto->base.value.data.v_fn->exec;
  assert(exec);
  /* iterate over entry block of executable */
  MirInstr *tmp;
  barray_foreach(exec->entry_block->instructions, tmp)
  {
    analyze_instr(cnt, tmp, execute);
  }

  fn->analyzed = true;
  set_cursor_block(cnt, prev_block);
  return entry_block->owner_exec->comptime_execute_result;
}

MirInstr *
analyze_instr_const(Context *cnt, MirInstrConst *cnst, bool execute)
{
  assert(cnst->base.value.type);
  push_into_curr_block(cnt, &cnst->base);
  return &cnst->base;
}

MirInstr *
analyze_instr_validate_type(Context *cnt, MirInstrValidateType *validate, bool execute)
{
  assert(validate->src);
  if (!type_cmp(validate->src->value.type, cnt->buildin_types.entry_type)) {
    builder_msg(cnt->builder, BUILDER_MSG_ERROR, ERR_INVALID_TYPE, validate->src->node->src,
                BUILDER_CUR_WORD, "expected type");
  }

  assert(validate->src->value.data.v_type);
  return &validate->base;
}

MirInstr *
analyze_instr_ret(Context *cnt, MirInstrRet *ret, bool execute)
{
  /* compare return value with current function type */
  push_into_curr_block(cnt, &ret->base);

  if (!cnt->cursor.block->terminal) cnt->cursor.block->terminal = &ret->base;

  if (execute && ret->value) {
    assert(ret->value->value.type);
    ret->base.owner_block->owner_exec->comptime_execute_result = ret->value;
  }

  return &ret->base;
}

MirInstr *
analyze_instr_decl_var(Context *cnt, MirInstrDeclVar *var, bool execute)
{
  if (var->type) {
    /* resolve time in compile time */
    MirInstr *result = analyze_instr(cnt, var->type, execute);
    assert(result && result->value.type);
    var->base.value.type = result->value.data.v_type;
  }

  push_into_curr_block(cnt, &var->base);
  return &var->base;
}

MirInstr *
analyze_instr_call(Context *cnt, MirInstrCall *call, bool execute)
{
  assert(call->callee && call->callee->kind == MIR_INSTR_FN_PROTO);
  MirInstrFnProto *callee = (MirInstrFnProto *)call->callee;

  MirInstr *result = analyze_instr_fn_proto(cnt, callee, call->comptime);
  if (call->comptime) {
    assert(result);
    return result;
  }

  push_into_curr_block(cnt, &call->base);
  return &call->base;
}

MirInstr *
analyze_instr_store(Context *cnt, MirInstrStore *store, bool execute)
{
  push_into_curr_block(cnt, &store->base);

  store->base.value.type = store->dest->value.type;

  return &store->base;
}

MirInstr *
analyze_instr(Context *cnt, MirInstr *instr, bool execute)
{
  if (!instr) return NULL;

  switch (instr->kind) {
  case MIR_INSTR_FN_PROTO:
    return analyze_instr_fn_proto(cnt, (MirInstrFnProto *)instr, execute);
  case MIR_INSTR_DECL_VAR:
    return analyze_instr_decl_var(cnt, (MirInstrDeclVar *)instr, execute);
  case MIR_INSTR_CALL:
    return analyze_instr_call(cnt, (MirInstrCall *)instr, execute);
  case MIR_INSTR_CONST:
    return analyze_instr_const(cnt, (MirInstrConst *)instr, execute);
  case MIR_INSTR_VALIDATE_TYPE:
    return analyze_instr_validate_type(cnt, (MirInstrValidateType *)instr, execute);
  case MIR_INSTR_RET:
    return analyze_instr_ret(cnt, (MirInstrRet *)instr, execute);
  case MIR_INSTR_STORE:
    return analyze_instr_store(cnt, (MirInstrStore *)instr, execute);
  default:
    msg_warning("missing analyze for %s", instr_name(instr));
  }

  return NULL;
}

void
analyze(Context *cnt)
{
  MirInstr *tmp;
  barray_foreach(cnt->globals, tmp)
  {
    assert(tmp->kind == MIR_INSTR_FN_PROTO);
    analyze_instr(cnt, tmp, false);

    mir_print_instr(tmp);
  }
}

/* MIR building */
void
ast_ublock(Context *cnt, Ast *ublock)
{
  Ast *tmp;
  barray_foreach(ublock->data.ublock.nodes, tmp) ast(cnt, tmp);
}

void
ast_block(Context *cnt, Ast *block)
{
  Ast *tmp;
  barray_foreach(block->data.block.nodes, tmp) ast(cnt, tmp);
}

void
ast_stmt_return(Context *cnt, Ast *ret)
{
  MirInstr *value = ast(cnt, ret->data.stmt_return.expr);
  add_instr_ret(cnt, ret, value);
}

MirInstr *
ast_expr_lit_int(Context *cnt, Ast *expr)
{
  return add_instr_const_int(cnt, expr, expr->data.expr_integer.val);
}

MirInstr *
ast_expr_ref(Context *cnt, Ast *ref)
{
  Ast *ident = ref->data.expr_ref.ident;
  assert(ident);
  return add_instr_decl_ref(cnt, ident);
}

MirInstr *
ast_expr_lit_fn(Context *cnt, Ast *lit_fn)
{
  Ast *block = lit_fn->data.expr_fn.block;
  assert(block);
  ast(cnt, block);
  return NULL;
}

MirInstr *
ast_expr_binop(Context *cnt, Ast *binop)
{
  Ast *ast_lhs = binop->data.expr_binop.lhs;
  Ast *ast_rhs = binop->data.expr_binop.rhs;
  assert(ast_lhs && ast_rhs);

  MirInstr *lhs = ast(cnt, ast_lhs);
  MirInstr *rhs = ast(cnt, ast_rhs);
  assert(lhs && rhs);

  return add_instr_binop(cnt, binop, lhs, rhs, binop->data.expr_binop.kind);
}

MirInstr *
ast_decl_entity(Context *cnt, Ast *entity)
{
  Ast *ast_name  = entity->data.decl.name;
  Ast *ast_type  = entity->data.decl.type;
  Ast *ast_value = entity->data.decl_entity.value;

  assert(ast_value);
  MirInstr *type = NULL;
  if (ast_type) {
    MirInstr *type_resolve_fn = ast(cnt, ast_type);
    assert(type_resolve_fn && type_resolve_fn->kind == MIR_INSTR_FN_PROTO);
    type = create_instr_call_type_resolve(cnt, type_resolve_fn);
  }

  if (ast_value->kind == AST_EXPR_LIT_FN) {
    MirInstr *fn          = add_instr_fn_proto(cnt, NULL, NULL, ast_name);
    MirBlock *entry_block = append_block(cnt, fn, "entry");
    set_cursor_block(cnt, entry_block);
    ast(cnt, ast_value);
  } else {
    MirInstr *decl = add_instr_decl_var(cnt, type, ast_name);
    MirInstr *init = ast(cnt, ast_value);
    return add_instr_store(cnt, ast_value, init, decl);
  }

  return NULL;
}

MirInstr *
ast_type_ref(Context *cnt, Ast *type_ref)
{
  MirInstr *result  = NULL;
  Ast *     ast_ref = type_ref->data.type_ref.ident;
  assert(ast_ref);

  /* create type resolver function */
  MirBlock *prev_block    = get_cursor_block(cnt);
  MirInstr *resolver_fn   = add_instr_fn_proto(cnt, NULL, NULL, NULL);
  resolver_fn->value.type = cnt->buildin_types.entry_resolve_type_fn;

  MirBlock *block = append_block(cnt, resolver_fn, "entry");
  set_cursor_block(cnt, block);

  BuildinType id = is_buildin_type(cnt, ast_ref->data.ident.hash);
  if (id != BUILDIN_TYPE_NONE) {
    /* buildin primitive !!! */
    result = add_instr_const_type(cnt, ast_ref, get_buildin(cnt, id));
  }

  add_instr_validate_type(cnt, result);
  add_instr_ret(cnt, NULL, result);
  set_cursor_block(cnt, prev_block);

  return resolver_fn;
}

MirInstr *
ast(Context *cnt, Ast *node)
{
  if (!node) return NULL;
  switch (node->kind) {
  case AST_UBLOCK:
    ast_ublock(cnt, node);
    break;
  case AST_BLOCK:
    ast_block(cnt, node);
    break;
  case AST_STMT_RETURN:
    ast_stmt_return(cnt, node);
    break;
  case AST_DECL_ENTITY:
    return ast_decl_entity(cnt, node);
  case AST_TYPE_REF:
    return ast_type_ref(cnt, node);
  case AST_EXPR_LIT_INT:
    return ast_expr_lit_int(cnt, node);
  case AST_EXPR_LIT_FN:
    return ast_expr_lit_fn(cnt, node);
  case AST_EXPR_BINOP:
    return ast_expr_binop(cnt, node);
  case AST_EXPR_REF:
    return ast_expr_ref(cnt, node);
  default:
    bl_abort("invalid node %s", ast_get_name(node));
  }

  return NULL;
}

const char *
instr_name(MirInstr *instr)
{
  assert(instr);
  switch (instr->kind) {
  case MIR_INSTR_INVALID:
    return "InstrInvalid";
  case MIR_INSTR_DECL_VAR:
    return "InstrDeclVar";
  case MIR_INSTR_CONST:
    return "InstrConst";
  case MIR_INSTR_LOAD:
    return "InstrLoad";
  case MIR_INSTR_STORE:
    return "InstrStore";
  case MIR_INSTR_BINOP:
    return "InstrBinop";
  case MIR_INSTR_RET:
    return "InstrRet";
  case MIR_INSTR_VALIDATE_TYPE:
    return "InstrValidateType";
  case MIR_INSTR_FN_PROTO:
    return "InstrFnProto";
  case MIR_INSTR_CALL:
    return "InstrCall";
  case MIR_INSTR_DECL_REF:
    return "InstrDeclRef";
  }

  return "UNKNOWN";
}

/* public */
void
mir_arenas_init(MirArenas *arenas)
{
  arena_init(&arenas->block_arena, sizeof(MirBlock), ARENA_CHUNK_COUNT, (ArenaElemDtor)block_dtor);
  arena_init(&arenas->instr_arena, sizeof(union _MirInstr), ARENA_CHUNK_COUNT,
             (ArenaElemDtor)instr_dtor);
  arena_init(&arenas->type_arena, sizeof(MirType), ARENA_CHUNK_COUNT, (ArenaElemDtor)type_dtor);
  arena_init(&arenas->exec_arena, sizeof(MirExec), ARENA_CHUNK_COUNT, (ArenaElemDtor)exec_dtor);
  arena_init(&arenas->var_arena, sizeof(MirVar), ARENA_CHUNK_COUNT, NULL);
  arena_init(&arenas->fn_arena, sizeof(MirFn), ARENA_CHUNK_COUNT, NULL);
}

void
mir_arenas_terminate(MirArenas *arenas)
{
  arena_terminate(&arenas->block_arena);
  arena_terminate(&arenas->instr_arena);
  arena_terminate(&arenas->type_arena);
  arena_terminate(&arenas->exec_arena);
  arena_terminate(&arenas->var_arena);
  arena_terminate(&arenas->fn_arena);
}

static void
_type_to_str(char *buf, size_t len, MirType *type)
{
#define append_buf(buf, len, str)                                                                  \
  {                                                                                                \
    const size_t filled = strlen(buf);                                                             \
    snprintf((buf) + filled, (len)-filled, "%s", str);                                             \
  }

  if (!buf) return;
  if (!type) {
    append_buf(buf, len, "?");
    return;
  }

  if (type->name) {
    append_buf(buf, len, type->name);
    return;
  }

  switch (type->kind) {
  case MIR_TYPE_INT:
    append_buf(buf, len, "integer");
    break;

  case MIR_TYPE_FN: {
    append_buf(buf, len, "fn(");

    MirType *tmp;
    BArray * args = type->data.fn.arg_types;
    assert(args);
    barray_foreach(args, tmp)
    {
      _type_to_str(buf, len, tmp);
      if (i < bo_array_size(args)) append_buf(buf, len, ", ");
    }

    append_buf(buf, len, ") ");

    _type_to_str(buf, len, type->data.fn.ret_type);
    break;
  }

  case MIR_TYPE_PTR: {
    append_buf(buf, len, "*");
    _type_to_str(buf, len, type->data.ptr.next);
    break;
  }

  default:
    bl_abort("unimplemented");
  }
}

void
mir_type_to_str(char *buf, int len, MirType *type)
{
  if (!buf || !len) return;
  buf[0] = '\0';
  _type_to_str(buf, len, type);
}

void
mir_run(Builder *builder, Assembly *assembly)
{
  Context cnt;
  memset(&cnt, 0, sizeof(Context));
  cnt.builder     = builder;
  cnt.assembly    = assembly;
  cnt.arenas      = &builder->mir_arenas;
  cnt.verbose     = builder->flags & BUILDER_VERBOSE;
  cnt.globals     = bo_array_new(sizeof(MirInstr *));
  cnt.llvm_cnt    = LLVMContextCreate();
  cnt.llvm_module = LLVMModuleCreateWithNameInContext(assembly->name, cnt.llvm_cnt);
  cnt.llvm_td     = LLVMGetModuleDataLayout(cnt.llvm_module);
  cnt.pass        = PASS_GENERATE;

  /* INIT BUILDINS */
  uint64_t tmp;
  cnt.buildin_types.table = bo_htbl_new(sizeof(BuildinType), _BUILDIN_TYPE_COUNT);
  for (int i = 0; i < _BUILDIN_TYPE_COUNT; ++i) {
    tmp = bo_hash_from_str(buildin_type_names[i]);
    bo_htbl_insert(cnt.buildin_types.table, tmp, i);
  }

  cnt.buildin_types.entry_type = create_type_type(&cnt);
  cnt.buildin_types.entry_void = create_type_void(&cnt);
  cnt.buildin_types.entry_s32 =
      create_type_int(&cnt, buildin_type_names[BUILDIN_TYPE_S32], 32, true);
  cnt.buildin_types.entry_u32 =
      create_type_int(&cnt, buildin_type_names[BUILDIN_TYPE_U32], 32, false);

  BArray *args = bo_array_new(sizeof(MirType *));
  cnt.buildin_types.entry_resolve_type_fn =
      create_type_fn(&cnt, cnt.buildin_types.entry_type, args);
  /* INIT BUILDINS */

  Unit *unit;
  barray_foreach(assembly->units, unit)
  {
    ast(&cnt, unit->ast);
  }

  cnt.pass = PASS_ANALYZE;
  analyze(&cnt);

  bo_unref(cnt.buildin_types.table);
  bo_unref(cnt.globals);
  /* TODO: pass context to the next stages */
  LLVMContextDispose(cnt.llvm_cnt);
}
