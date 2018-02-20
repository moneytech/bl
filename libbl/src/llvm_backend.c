//*****************************************************************************
// blc
//
// File:   llvm_backend.c
// Author: Martin Dorazil
// Date:   6.2.18
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

#include <setjmp.h>
#include <string.h>
#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Support.h>
#include <bobject/containers/htbl.h>
#include <bobject/containers/hash.h>

#include "bl/llvm_backend.h"
#include "bl/pipeline/stage.h"
#include "bl/bldebug.h"
#include "bl/bllimits.h"
#include "bl/unit.h"
#include "ast/ast_impl.h"
#include "llvm_bl_cnt_impl.h"

/* class LlvmBackend */

#define VERIFY 1
#define gen_error(self, format, ...) \
  { \
    bl_actor_error((Actor *)(self)->unit, (format), ##__VA_ARGS__); \
    longjmp((self)->jmp_error, 1); \
  }

static void
reset(LlvmBackend *self,
      Unit *unit);

static bool
run(LlvmBackend *self,
    Unit *unit);

static void
init_self(LlvmBackend *self,
          Unit *unit,
          LLVMModuleRef module,
          jmp_buf jmp_error);

static void
destroy_self(LlvmBackend *self);

static LLVMTypeRef
to_llvm_type(Type *t);

static LLVMValueRef
get_or_create_const_string(LlvmBackend *self,
                           const char *str);

static LLVMValueRef
gen_default(LlvmBackend *self,
            Type *t);

static int
gen_func_params(LlvmBackend *self,
                NodeFuncDecl *node,
                LLVMTypeRef *out,
                bool forward);

static LLVMValueRef
gen_func(LlvmBackend *self,
         NodeFuncDecl *fnode,
         bool forward);

static LLVMValueRef
gen_expr(LlvmBackend *self,
         NodeExpr *expr);

static LLVMValueRef
gen_binop(LlvmBackend *self,
          NodeBinop *binop);

static void
gen_cmp_stmt(LlvmBackend *self,
             NodeStmt *stmt,
             NodeFuncDecl *fnode);

static void
gen_if_stmt(LlvmBackend *self,
            NodeIfStmt *ifstmt);

static void
gen_ret(LlvmBackend *self,
        NodeReturnStmt *node);

static LLVMTypeRef
get_ret_type(LlvmBackend *self,
             NodeFuncDecl *func);

static void
gen_var_decl(LlvmBackend *self,
             NodeVarDecl *vdcl);

static LLVMValueRef
gen_call(LlvmBackend *self,
         NodeCall *call);

static int
gen_call_args(LlvmBackend *self,
              NodeCall *call,
              LLVMValueRef *out);

static void
gen_gstmt(LlvmBackend *self,
          NodeGlobalStmt *gstmt);


/* class LlvmBackend constructor params */
bo_decl_params_with_base_begin(LlvmBackend, Stage)
  /* constructor params */
bo_end();

/* class LlvmBackend object members */
bo_decl_members_begin(LlvmBackend, Stage)
  /* members */
  Unit *unit;
  LLVMModuleRef mod;
  LLVMBuilderRef builder;
  jmp_buf jmp_error;

  /* tmps */
  LlvmBlockContext *block_context;
  BHashTable *const_strings;
  LLVMValueRef ret_value;
  LLVMBasicBlockRef ret_block;
bo_end();

bo_impl_type(LlvmBackend, Stage);

void
LlvmBackendKlass_init(LlvmBackendKlass *klass)
{
  bo_vtbl_cl(klass, Stage)->run =
    (bool (*)(Stage *,
              Actor *)) run;
}

void
LlvmBackend_ctor(LlvmBackend *self,
                 LlvmBackendParams *p)
{
  /* constructor */
  /* initialize parent */
  bo_parent_ctor(Stage, p);
}

void
LlvmBackend_dtor(LlvmBackend *self)
{
  LLVMDisposeBuilder(self->builder);
  bo_unref(self->const_strings);
  bo_unref(self->block_context);
}

bo_copy_result
LlvmBackend_copy(LlvmBackend *self,
                 LlvmBackend *other)
{
  return BO_NO_COPY;
}

/* class LlvmBackend end */

void
init_self(LlvmBackend *self,
          Unit *unit,
          LLVMModuleRef module,
          jmp_buf jmp_error)
{
}

void
destroy_self(LlvmBackend *self)
{
  bo_unref(self->const_strings);
  bo_unref(self->block_context);
  LLVMDisposeBuilder(self->builder);
}

void
reset(LlvmBackend *self,
      Unit *unit)
{
  LLVMDisposeBuilder(self->builder);
  bo_unref(self->const_strings);
  bo_unref(self->block_context);

  self->builder = LLVMCreateBuilder();
  self->unit = unit;
  self->mod = LLVMModuleCreateWithName(bl_unit_get_name(unit));
  self->block_context = bl_llvm_block_context_new();
  self->const_strings = bo_htbl_new(sizeof(LLVMValueRef), 256);
}

bool
run(LlvmBackend *self,
    Unit *unit)
{
  if (setjmp(self->jmp_error)) {
    return false;
  }

  reset(self, unit);

  Node *root = bl_ast_get_root(bl_unit_get_ast(unit));
  switch (root->type) {
    case BL_NODE_GLOBAL_STMT:
      gen_gstmt(self, (NodeGlobalStmt *) root);
      break;
    default: gen_error(self, "invalid node on llvm generator input");
  }

#if VERIFY
  char *error = NULL;
  if (LLVMVerifyModule(self->mod, LLVMReturnStatusAction, &error)) {
    gen_error(self, "not verified with error %s", error);
  }
#endif

  bl_unit_set_llvm_module(unit, self->mod);
  return true;
}

/*
 * Convert known type to LLVM type representation
 * TODO: dont use strings here!!!
 */
LLVMTypeRef
to_llvm_type(Type *t)
{
  switch (bl_type_get(t)) {
    case BL_TYPE_VOID:
      return LLVMVoidType();
    case BL_TYPE_I32:
      return LLVMInt32Type();
    case BL_TYPE_I64:
      return LLVMInt64Type();
    case BL_TYPE_STRING:
      return LLVMPointerType(LLVMInt8Type(), 0);
    case BL_TYPE_CHAR:
      return LLVMInt8Type();
    case BL_TYPE_BOOL:
      return LLVMInt1Type();
    default:
      return NULL;
  }
}

LLVMValueRef
get_or_create_const_string(LlvmBackend *self,
                           const char *str)
{
  uint32_t hash = bo_hash_from_str(str);
  if (bo_htbl_has_key(self->const_strings, hash))
    return bo_htbl_at(self->const_strings, hash, LLVMValueRef);

  LLVMValueRef s = LLVMBuildGlobalString(
    self->builder, str, "str");

  s = LLVMConstPointerCast(s, LLVMPointerType(LLVMInt8Type(), 0));
  bo_htbl_insert(self->const_strings, hash, s);
  return s;
}

/*
 * Generate default value for known type.
 * For string we create global string array with line terminator.
 */
static LLVMValueRef
gen_default(LlvmBackend *self,
            Type *t)
{
  switch (bl_type_get(t)) {
    case BL_TYPE_CHAR:
      return LLVMConstInt(LLVMInt8Type(), 0, true);
    case BL_TYPE_I32:
      return LLVMConstInt(LLVMInt32Type(), 0, true);
    case BL_TYPE_I64:
      return LLVMConstInt(LLVMInt64Type(), 0, true);
    case BL_TYPE_BOOL:
      return LLVMConstInt(LLVMInt1Type(), 0, false);
    case BL_TYPE_STRING: {
      return get_or_create_const_string(self, "\0");
    }
    default:
      return NULL;
  }
}

/*
 * Fill array for parameters of function and return count.
 * When forward is false we expect function body after function declaration
 * and all parameters are cached for future use in function body.
 */
static int
gen_func_params(LlvmBackend *self,
                NodeFuncDecl *node,
                LLVMTypeRef *out,
                bool forward)
{
  int out_i = 0;
  const int c = bl_node_func_decl_get_param_count(node);

  /* no params */
  if (c == 0) {
    return 0;
  }

  NodeParamVarDecl *param = NULL;
  for (int i = 0; i < c; i++) {
    param = bl_node_func_decl_get_param(node, i);
    *out = to_llvm_type(bl_node_decl_get_type((NodeDecl *) param));

    out++;
    out_i++;
  }

  return out_i;
}

LLVMValueRef
gen_expr(LlvmBackend *self,
         NodeExpr *expr)
{
  bl_node_e nt = bl_node_get_type((Node *) expr);
  LLVMValueRef val;
  switch (nt) {
    case BL_NODE_CONST:
      switch (bl_node_const_get_type((NodeConst *) expr)) {
        case BL_CONST_INT:
          val = LLVMConstInt(
            LLVMInt32Type(),
            (unsigned long long int) bl_node_const_get_int((NodeConst *) expr),
            true);
          break;
        case BL_CONST_BOOL:
          val = LLVMConstInt(
            LLVMInt1Type(),
            (unsigned long long int) bl_node_const_get_bool((NodeConst *) expr),
            false);
          break;
        case BL_CONST_STRING:
          val = get_or_create_const_string(
            self, bl_node_const_get_str((NodeConst *) expr));
          break;
        case BL_CONST_CHAR:
          val = LLVMConstInt(
            LLVMInt8Type(),
            (unsigned long long int) bl_node_const_get_char((NodeConst *) expr),
            false);
          break;
        default: bl_abort("invalid constant type");
      }
      break;
    case BL_NODE_DECL_REF: {
      val = bl_llvm_block_context_get(
        self->block_context, bl_node_decl_ref_get_ident((NodeDeclRef *) expr));
      bl_assert(val, "unknown symbol");
      break;
    }
    case BL_NODE_CALL:
      val = gen_call(self, (NodeCall *) expr);
      break;
    case BL_NODE_BINOP:
      val = gen_binop(self, (NodeBinop *) expr);
      break;
    default: bl_abort("unknown expression type");
  }

  return val;
}

LLVMValueRef
gen_binop(LlvmBackend *self,
          NodeBinop *binop)
{
  LLVMValueRef lvalue = gen_expr(self, bl_node_binop_get_lvalue(binop));
  LLVMValueRef rvalue = gen_expr(self, bl_node_binop_get_rvalue(binop));

  if (LLVMIsAAllocaInst(rvalue))
    rvalue = LLVMBuildLoad(self->builder, rvalue, "tmp");

  switch (bl_node_binop_get_op(binop)) {
    case BL_SYM_ASIGN:
      LLVMBuildStore(self->builder, rvalue, lvalue);
      return NULL;
    case BL_SYM_PLUS:
      if (LLVMIsAAllocaInst(lvalue))
        lvalue = LLVMBuildLoad(self->builder, lvalue, "tmp");
      return LLVMBuildAdd(self->builder, lvalue, rvalue, "tmp");
    case BL_SYM_MINUS:
      if (LLVMIsAAllocaInst(lvalue))
        lvalue = LLVMBuildLoad(self->builder, lvalue, "tmp");
      return LLVMBuildSub(self->builder, lvalue, rvalue, "tmp");
    case BL_SYM_ASTERISK:
      if (LLVMIsAAllocaInst(lvalue))
        lvalue = LLVMBuildLoad(self->builder, lvalue, "tmp");
      return LLVMBuildMul(self->builder, lvalue, rvalue, "tmp");
    case BL_SYM_SLASH:
      if (LLVMIsAAllocaInst(lvalue))
        lvalue = LLVMBuildLoad(self->builder, lvalue, "tmp");
      return LLVMBuildFDiv(self->builder, lvalue, rvalue, "tmp");
    case BL_SYM_EQ:
      if (LLVMIsAAllocaInst(lvalue))
        lvalue = LLVMBuildLoad(self->builder, lvalue, "tmp");
      return LLVMBuildICmp(self->builder, LLVMIntEQ, lvalue, rvalue, "tmp");
    case BL_SYM_NEQ:
      if (LLVMIsAAllocaInst(lvalue))
        lvalue = LLVMBuildLoad(self->builder, lvalue, "tmp");
      return LLVMBuildICmp(self->builder, LLVMIntNE, lvalue, rvalue, "tmp");
    case BL_SYM_GREATER:
      if (LLVMIsAAllocaInst(lvalue))
        lvalue = LLVMBuildLoad(self->builder, lvalue, "tmp");
      return LLVMBuildICmp(self->builder, LLVMIntSGT, lvalue, rvalue, "tmp");
    case BL_SYM_LESS:
      if (LLVMIsAAllocaInst(lvalue))
        lvalue = LLVMBuildLoad(self->builder, lvalue, "tmp");
      return LLVMBuildICmp(self->builder, LLVMIntSLT, lvalue, rvalue, "tmp");
    case BL_SYM_GREATER_EQ:
      if (LLVMIsAAllocaInst(lvalue))
        lvalue = LLVMBuildLoad(self->builder, lvalue, "tmp");
      return LLVMBuildICmp(self->builder, LLVMIntSGE, lvalue, rvalue, "tmp");
    case BL_SYM_LESS_EQ:
      if (LLVMIsAAllocaInst(lvalue))
        lvalue = LLVMBuildLoad(self->builder, lvalue, "tmp");
      return LLVMBuildICmp(self->builder, LLVMIntSLE, lvalue, rvalue, "tmp");
    default: bl_abort("unknown binop");
  }

  return NULL;
}

void
gen_ret(LlvmBackend *self,
        NodeReturnStmt *node)
{
  NodeExpr *expr = bl_node_return_stmt_get_expr(node);
  if (!expr) {
    return;
  }

  LLVMValueRef val = gen_expr(self, expr);

  if (LLVMIsAAllocaInst(val))
    val = LLVMBuildLoad(self->builder, val, "tmp");

  LLVMBuildStore(self->builder, val, self->ret_value);
  LLVMBuildBr(self->builder, self->ret_block);
}

LLVMTypeRef
get_ret_type(LlvmBackend *self,
             NodeFuncDecl *func)
{
  Type *t = bl_node_decl_get_type((NodeDecl *) func);
  if (t == NULL) {
    return LLVMVoidType();
  }

  return to_llvm_type(t);
}

void
gen_var_decl(LlvmBackend *self,
             NodeVarDecl *vdcl)
{
  LLVMTypeRef t = to_llvm_type(bl_node_decl_get_type((NodeDecl *) vdcl));

  Ident *id = bl_node_decl_get_ident((NodeDecl *) vdcl);
  LLVMValueRef var = LLVMBuildAlloca(self->builder, t, bl_ident_get_name(id));

  /*
   * Generate expression if there is one or use default value instead.
   */
  LLVMValueRef def = NULL;
  NodeExpr *expr = bl_node_var_decl_get_expr(vdcl);
  if (expr) {
    def = gen_expr(self, expr);
  } else {
    def = gen_default(self, bl_node_decl_get_type((NodeDecl *) vdcl));
  }

  if (LLVMIsAAllocaInst(def)) {
    def = LLVMBuildLoad(self->builder, def, "tmp");
  }

  LLVMBuildStore(self->builder, def, var);
  bl_llvm_block_context_add(self->block_context, var, id);
}

int
gen_call_args(LlvmBackend *self,
              NodeCall *call,
              LLVMValueRef *out)
{
  int out_i = 0;
  const int c = bl_node_call_get_arg_count(call);

  /* no args */
  if (c == 0) {
    return 0;
  }

  NodeExpr *expr = NULL;
  for (int i = 0; i < c; i++) {
    expr = bl_node_call_get_arg(call, i);
    LLVMValueRef val = gen_expr(self, expr);

    if (LLVMIsAAllocaInst(val))
      *out = LLVMBuildLoad(self->builder, val, "tmp");
    else
      *out = val;

    out++;
    out_i++;
  }

  return out_i;
}

LLVMValueRef
gen_call(LlvmBackend *self,
         NodeCall *call)
{
  NodeFuncDecl *callee = bl_node_call_get_callee(call);

  LLVMValueRef
    fn =
    LLVMGetNamedFunction(self->mod, bl_ident_get_name(bl_node_decl_get_ident((NodeDecl *) callee)));

  /*
   * Create forward declaration when function has not been created yet.
   */
  if (fn == NULL) {
    fn = gen_func(self, callee, true);
  }

  /* args */
  LLVMValueRef args[BL_MAX_FUNC_PARAM_COUNT] = {0};
  int argc = gen_call_args(self, call, args);

  /* TODO: return value passed from build method */

  LLVMValueRef ret = LLVMBuildCall(self->builder, fn, args, argc, "");
  return ret;
}

LLVMValueRef
gen_func(LlvmBackend *self,
         NodeFuncDecl *fnode,
         bool forward)
{
  /* params */
  LLVMTypeRef param_types[BL_MAX_FUNC_PARAM_COUNT] = {0};

  int pc = gen_func_params(self, fnode, param_types, forward);
  Ident *id = bl_node_decl_get_ident((NodeDecl *) fnode);

  LLVMValueRef func = LLVMGetNamedFunction(self->mod, bl_ident_get_name(id));
  if (func == NULL) {
    LLVMTypeRef ret = to_llvm_type(bl_node_decl_get_type((NodeDecl *) fnode));
    LLVMTypeRef ret_type = LLVMFunctionType(ret, param_types, (unsigned int) pc, false);
    func = LLVMAddFunction(self->mod, bl_ident_get_name(id), ret_type);
  }

  if (!forward) {
    NodeStmt *stmt = bl_node_func_decl_get_stmt(fnode);
    if (stmt) {
      LLVMBasicBlockRef block = LLVMAppendBasicBlock(func, "entry");
      LLVMPositionBuilderAtEnd(self->builder, block);
      gen_cmp_stmt(self, stmt, fnode);
    }
  }

  return func;
}

void
gen_if_stmt(LlvmBackend *self,
            NodeIfStmt *ifstmt)
{
  LLVMBasicBlockRef insert_block = LLVMGetInsertBlock(self->builder);
  LLVMValueRef parent = LLVMGetBasicBlockParent(insert_block);

  LLVMBasicBlockRef ifthen = LLVMAppendBasicBlock(parent, "then");
  LLVMBasicBlockRef ifelse = LLVMAppendBasicBlock(parent, "else");
  LLVMBasicBlockRef ifcont = LLVMAppendBasicBlock(parent, "cont");
  LLVMValueRef expr = gen_expr(self, bl_node_if_stmt_get_cond(ifstmt));

  if (LLVMIsAAllocaInst(expr))
    expr = LLVMBuildLoad(self->builder, expr, "tmp");
  expr = LLVMBuildIntCast(self->builder, expr, LLVMInt1Type(), "tmp");

  /*
   * If condition break generation.
   */
  LLVMBuildCondBr(self->builder, expr, ifthen, ifelse);

  LLVMPositionBuilderAtEnd(self->builder, ifthen);
  gen_cmp_stmt(self, bl_node_if_stmt_get_then_stmt(ifstmt), NULL);
//  LLVMBuildBr(self->builder, ifcont);

  LLVMPositionBuilderAtEnd(self->builder, ifelse);
  if (bl_node_if_stmt_get_else_stmt(ifstmt))
    gen_cmp_stmt(self, bl_node_if_stmt_get_else_stmt(ifstmt), NULL);
  else
    LLVMBuildBr(self->builder, ifcont);

  LLVMPositionBuilderAtEnd(self->builder, ifcont);
}

void
gen_cmp_stmt(LlvmBackend *self,
             NodeStmt *stmt,
             NodeFuncDecl *fnode)
{
  bl_llvm_block_context_push_block(self->block_context);
  LLVMBasicBlockRef insert_block = LLVMGetInsertBlock(self->builder);
  LLVMPositionBuilderAtEnd(self->builder, insert_block);

  /*
   * Create named references to function parameters so they
   * can be called by name in function body. This is valid only
   * when this compound statement is function body.
   */
  if (fnode) {
    LLVMValueRef parent = LLVMGetBasicBlockParent(insert_block);
    bl_assert(LLVMIsAFunction(parent), "invalid parent, must be function");
    const int pc = bl_node_func_decl_get_param_count(fnode);
    for (int i = 0; i < pc; i++) {
      NodeParamVarDecl *param = bl_node_func_decl_get_param(fnode, i);
      Ident *ident = bl_node_decl_get_ident((NodeDecl *) param);

      LLVMValueRef p = LLVMGetParam(parent, i);
      LLVMValueRef p_tmp = LLVMBuildAlloca(self->builder, LLVMTypeOf(p), bl_ident_get_name(ident));
      LLVMBuildStore(self->builder, p, p_tmp);
      bl_llvm_block_context_add(self->block_context, p_tmp, ident);
    }

    /*
     * Prepare return value.
     */
    LLVMTypeRef ret_type = get_ret_type(self, fnode);
    if (ret_type != LLVMVoidType()) {
      self->ret_value = LLVMBuildAlloca(self->builder, ret_type, "ret");
    } else {
      self->ret_value = NULL;
    }

    self->ret_block = LLVMAppendBasicBlock(parent, "exit");
  }

  Node *child = NULL;
  const int c = bl_node_stmt_child_get_count(stmt);
  for (int i = 0; i < c; i++) {
    child = bl_node_stmt_get_child(stmt, i);
    switch (child->type) {
      case BL_NODE_RETURN_STMT:
        gen_ret(self, (NodeReturnStmt *) child);
        goto done;
      case BL_NODE_VAR_DECL:
        gen_var_decl(self, (NodeVarDecl *) child);
        break;
      case BL_NODE_BINOP:
        gen_binop(self, (NodeBinop *) child);
        break;
      case BL_NODE_CALL:
        gen_expr(self, (NodeExpr *) child);
        break;
      case BL_NODE_IF_STMT:
        gen_if_stmt(self, (NodeIfStmt *) child);
        break;
      case BL_NODE_DECL_REF:
        /* only decl reference without any expression, this will be ignored for now */
        break;
      default: bl_warning("unimplemented statement in function scope");
    }
  }

  LLVMBuildBr(self->builder, self->ret_block);

done:
  if (fnode) {
    LLVMPositionBuilderAtEnd(self->builder, self->ret_block);

    if (self->ret_value) {
      self->ret_value = LLVMBuildLoad(self->builder, self->ret_value, "tmp");
      LLVMBuildRet(self->builder, self->ret_value);
    } else {
      LLVMBuildRetVoid(self->builder);
    }
  }

  bl_llvm_block_context_pop_block(self->block_context);
}

void
gen_gstmt(LlvmBackend *self,
          NodeGlobalStmt *gstmt)
{
  Node *child = NULL;
  const int c = bl_node_global_stmt_get_child_count(gstmt);
  for (size_t i = 0; i < c; i++) {
    child = bl_node_global_stmt_get_child(gstmt, i);
    switch (child->type) {
      case BL_NODE_FUNC_DECL:
        gen_func(self, (NodeFuncDecl *) child, false);
        break;
      default: gen_error(self, "invalid node in global scope");
    }
  }
}

LlvmBackend *
bl_llvm_backend_new(bl_compile_group_e group)
{
  LlvmBackendParams p = {.base.group = group};

  return bo_new(LlvmBackend, &p);
}

