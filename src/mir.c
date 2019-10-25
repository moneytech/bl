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
#include "ast.h"
#include "builder.h"
#include "common.h"
#include "llvm_di.h"
#include "mir_printer.h"
#include "unit.h"

// Constants
// clang-format off
#define ARENA_CHUNK_COUNT               512
#define ANALYZE_TABLE_SIZE              8192 
#define TEST_CASE_FN_NAME               ".test"
#define RESOLVE_TYPE_FN_NAME            ".type"
#define INIT_VALUE_FN_NAME              ".init"
#define IMPL_FN_NAME                    ".impl"
#define IMPL_VARGS_TMP_ARR              ".vargs.arr"
#define IMPL_VARGS_TMP                  ".vargs"
#define IMPL_ANY_TMP                    ".any"
#define IMPL_ANY_EXPR_TMP               ".any.expr"
#define IMPL_COMPOUND_TMP               ".compound"
#define IMPL_RTTI_ENTRY                 ".rtti"
#define IMPL_RET_TMP                    ".ret"
#define NO_REF_COUNTING                 -1
#define VERBOSE_ANALYZE                 false
// clang-format on

#define ANALYZE_RESULT(_state, _waiting_for)                                                       \
	(AnalyzeResult)                                                                            \
	{                                                                                          \
		.state = ANALYZE_##_state, .waiting_for = (_waiting_for)                           \
	}

#define CREATE_INSTR(_cnt, _kind, _node, _t) ((_t)_create_instr((_cnt), (_kind), (_node)))

#define CREATE_TYPE_RESOLVER_CALL(_ast)                                                            \
	ast_create_impl_fn_call(                                                                   \
	    cnt, (_ast), RESOLVE_TYPE_FN_NAME, cnt->builtin_types.t_resolve_type_fn, false)

#define CREATE_VALUE_RESOLVER_CALL(_ast, _analyze)                                                 \
	ast_create_impl_fn_call(cnt, (_ast), INIT_VALUE_FN_NAME, NULL, (_analyze))

#define GEN_INSTR_SIZEOF
#include "mir.inc"
#undef GEN_INSTR_SIZEOF

TSMALL_ARRAY_TYPE(LLVMType, LLVMTypeRef, 8);
TSMALL_ARRAY_TYPE(LLVMMetadata, LLVMMetadataRef, 16);
TSMALL_ARRAY_TYPE(DeferStack, Ast *, 64);
TSMALL_ARRAY_TYPE(InstrPtr64, MirInstr *, 64);
TSMALL_ARRAY_TYPE(String, const char *, 64);

typedef struct {
	VM        vm;
	Assembly *assembly;
	TArray    test_cases;
	TString   tmp_sh;
	MirFn *   entry_fn;
	bool      debug_mode;

	/* AST -> MIR generation */
	struct {
		TSmallArray_DeferStack defer_stack;
		MirInstrBlock *        current_block;
		MirInstrBlock *        break_block;
		MirInstrBlock *        exit_block;
		MirInstrBlock *        continue_block;
		ID *                   current_entity_id; /* Sometimes used for named structures */
		MirInstr *             current_fwd_struct_decl;
		bool                   enable_incomplete_decl_refs;
	} ast;

	/* Analyze MIR generated from AST */
	struct {
		/* Instructions waiting for analyze. */
		TList queue;

		/* Hash table of arrays. Hash is ID of symbol and array contains queue
		 * of waiting instructions (DeclRefs). */
		THashTable waiting;
		bool       verbose_pre;
		bool       verbose_post;

		/* Unique table of pointers to types later generated into RTTI, this table contains
		 * only pointer to top level types used by typeinfo operator, not all sub-types. */
		THashTable RTTI_entry_types; // INCOMPLETE: remove

		LLVMDIBuilderRef llvm_di_builder;
	} analyze;

	/* Builtins */
	struct BuiltinTypes {
		MirType *t_type;
		MirType *t_s8;
		MirType *t_s16;
		MirType *t_s32;
		MirType *t_s64;
		MirType *t_u8;
		MirType *t_u16;
		MirType *t_u32;
		MirType *t_u64;
		MirType *t_usize;
		MirType *t_bool;
		MirType *t_f32;
		MirType *t_f64;
		MirType *t_string;
		MirType *t_void;

		MirType *t_u8_ptr;
		MirType *t_string_ptr;
		MirType *t_string_slice;
		MirType *t_resolve_type_fn;
		MirType *t_test_case_fn;

		/* TypeInfo cached types. */
		/* Any type info can be generated after all this types are found in assembly. */
		MirType *t_TypeKind;
		MirType *t_TypeInfo;
		MirType *t_TypeInfoInt;
		MirType *t_TypeInfoReal;
		MirType *t_TypeInfoPtr;
		MirType *t_TypeInfoEnum;
		MirType *t_TypeInfoEnumVariant;
		MirType *t_TypeInfoArray;
		MirType *t_TypeInfoStruct;
		MirType *t_TypeInfoStructMember;
		MirType *t_TypeInfoFn;
		MirType *t_TypeInfoFnArg;
		MirType *t_TypeInfoType;
		MirType *t_TypeInfoVoid;
		MirType *t_TypeInfoBool;
		MirType *t_TypeInfoNull;
		MirType *t_TypeInfoString;

		MirType *t_TypeInfo_ptr;
		MirType *t_TypeInfo_slice;
		MirType *t_TypeInfoStructMembers_slice;
		MirType *t_TypeInfoEnumVariants_slice;
		MirType *t_TypeInfoFnArgs_slice;
		/* OTHER END */
	} builtin_types;
} Context;

typedef enum {
	/* Analyze pass failed. */
	ANALYZE_FAILED = 0,

	/* Analyze pass passed. */
	ANALYZE_PASSED = 1,

	/* Analyze pass cannot be done because some of sub-parts has not been
	   analyzed yet and probably needs to be executed during analyze pass. In
	   such case we push analyzed instruction at the end of analyze queue. */
	ANALYZE_POSTPONE = 2,

	/* In this case AnalyzeResult will contain hash of desired symbol which be satisfied later,
	   instruction is pushed into waiting table. */
	ANALYZE_WAITING = 3,
} AnalyzeState;

typedef struct {
	AnalyzeState state;
	u64          waiting_for;
} AnalyzeResult;

typedef enum {
	ANALYZE_STAGE_BREAK,
	ANALYZE_STAGE_CONTINUE,
	ANALYZE_STAGE_FAILED,
} AnalyzeStageState;

typedef AnalyzeStageState (*AnalyzeStageFn)(Context *, MirInstr **, MirType *);

typedef struct {
	s32            count;
	AnalyzeStageFn stages[];
} AnalyzeSlotConfig;

/* Ids of builtin symbols, hash is calculated inside init_builtins function
 * later. */
static ID builtin_ids[_MIR_BUILTIN_ID_COUNT] = {
#define GEN_BUILTIN_IDS
#include "mir.inc"
#undef GEN_BUILTIN_IDS
};

/* Arena destructor for functions. */
static void
fn_dtor(MirFn **fn)
{
	dcbFreeCallback((*fn)->dyncall.extern_callback_handle);
}

/* FW decls */
/* Initialize all builtin types. */
static void
init_builtins(Context *cnt);

/* Start top-level execution of entry function using MIR-VM. (Usualy 'main' funcition) */
static void
execute_entry_fn(Context *cnt);

/* Execute all registered test cases in current assembly. */
static void
execute_test_cases(Context *cnt);

/* Register incomplete scope entry for symbol. */
static ScopeEntry *
register_symbol(Context *cnt, Ast *node, ID *id, Scope *scope, bool is_builtin, bool enable_groups);

/* Lookup builtin by builtin kind in global scope. Return NULL even if builtin is valid symbol in
 * case when it's not been analyzed yet or is incomplete struct type. In such case caller must
 * postpone analyze process. This is an error in any post-analyze processing (every type must be
 * complete when analyze pass id completed!). */
static MirType *
lookup_builtin(Context *cnt, MirBuiltinIdKind kind);

/* Initialize type ID. This function creates and set ID string and calculates integer hash from this
 * string. The type.id.str could be also used as name for unnamed types. */
static void
init_type_id(Context *cnt, MirType *type);

/* Create new type. The 'user_id' is optional. */
static MirType *
create_type(Context *cnt, MirTypeKind kind, ID *user_id);

static MirType *
create_type_type(Context *cnt);

static MirType *
create_type_null(Context *cnt, MirType *base_type);

static MirType *
create_type_void(Context *cnt);

static MirType *
create_type_bool(Context *cnt);

static MirType *
create_type_int(Context *cnt, ID *id, s32 bitcount, bool is_signed);

static MirType *
create_type_real(Context *cnt, ID *id, s32 bitcount);

static MirType *
create_type_ptr(Context *cnt, MirType *src_type);

static MirType *
create_type_fn(Context *cnt, ID *id, MirType *ret_type, TSmallArray_ArgPtr *args, bool is_vargs);

static MirType *
create_type_array(Context *cnt, MirType *elem_type, s64 len);

static MirType *
create_type_struct(Context *              cnt,
                   MirTypeKind            kind,
                   ID *                   id,
                   Scope *                scope,
                   TSmallArray_MemberPtr *members,   /* MirMember */
                   MirType *              base_type, /* optional */
                   bool                   is_packed);

/* Make incomplete type struct declaration complete. This function sets all desired information
 * about struct to the forward declaration type. */
static MirType *
complete_type_struct(Context *              cnt,
                     MirInstr *             fwd_decl,
                     Scope *                scope,
                     TSmallArray_MemberPtr *members,
                     MirType *              base_type, /* optional */
                     bool                   is_packed);

/* Create incomplete struct type placeholer to be filled later. */
static MirType *
create_type_struct_incomplete(Context *cnt, ID *user_id);

static MirType *
create_type_enum(Context *               cnt,
                 ID *                    id,
                 Scope *                 scope,
                 MirType *               base_type,
                 TSmallArray_VariantPtr *variants);

MirType *
create_type_struct_special(Context *cnt, MirTypeKind kind, ID *id, MirType *elem_ptr_type);

/* These functions will generate LLVM type representation for each BL type, this functions fulfill
 * some BL type's missing values as sizes and alignment also. */
static void
init_llvm_type_int(Context *cnt, MirType *type);

static void
init_llvm_type_real(Context *cnt, MirType *type);

static void
init_llvm_type_ptr(Context *cnt, MirType *type);

static void
init_llvm_type_null(Context *cnt, MirType *type);

static void
init_llvm_type_void(Context *cnt, MirType *type);

static void
init_llvm_type_bool(Context *cnt, MirType *type);

static void
init_llvm_type_fn(Context *cnt, MirType *type);

static void
init_llvm_type_array(Context *cnt, MirType *type);

static void
init_llvm_type_struct(Context *cnt, MirType *type);

static void
init_llvm_type_enum(Context *cnt, MirType *type);

static void
init_llvm_DI_scope(Context *cnt, Scope *scope);

static MirVar *
create_var(Context *cnt,
           Ast *    decl_node,
           Scope *  scope,
           ID *     id,
           MirType *alloc_type,
           bool     is_mutable,
           bool     is_in_gscope,
           u32      flags);

static MirVar *
create_var_impl(Context *   cnt,
                const char *name,
                MirType *   alloc_type,
                bool        is_mutable,
                bool        is_in_gscope,
                bool        comptime);

static MirFn *
create_fn(Context *        cnt,
          Ast *            node,
          ID *             id,
          const char *     linkage_name,
          u32              flags,
          MirInstrFnProto *prototype,
          bool             emit_llvm,
          bool             is_in_gscope);

static MirMember *
create_member(Context *cnt, Ast *node, ID *id, Scope *scope, s64 index, MirType *type);

static MirArg *
create_arg(Context *cnt, Ast *node, ID *id, Scope *scope, MirType *type);

static MirVariant *
create_variant(Context *cnt, ID *id, Scope *scope, MirConstValue *value);

static MirConstValue *
create_const_value(Context *cnt, MirType *type);

/* init_or_create_* functions can be used in two ways, 'v' is initialized when we pass one and
 * function returns pointer to this value. If 'v' is NULL, new value is created with desired values
 * set. */
static MirConstValue *
init_or_create_const_integer(Context *cnt, MirConstValue *v, MirType *type, u64 i);

static MirConstValue *
init_or_create_const_bool(Context *cnt, MirConstValue *v, bool b);

static MirConstValue *
init_or_create_const_null(Context *cnt, MirConstValue *v, MirType *type);

static MirConstValue *
init_or_create_const_var_ptr(Context *cnt, MirConstValue *v, MirType *type, MirVar *var);

static MirConstValue *
init_or_create_const_array(Context *                  cnt,
                           MirConstValue *            v,
                           MirType *                  elem_type,
                           TSmallArray_ConstValuePtr *elems);

static MirConstValue *
init_or_create_const_struct(Context *                  cnt,
                            MirConstValue *            v,
                            MirType *                  type,
                            TSmallArray_ConstValuePtr *members);

static MirConstValue *
init_or_create_const_string(Context *cnt, MirConstValue *v, const char *str);

static MirInstrBlock *
append_block(Context *cnt, MirFn *fn, const char *name);

/* instructions */
static void
maybe_mark_as_unrechable(MirInstrBlock *block, MirInstr *instr);

static void
append_current_block(Context *cnt, MirInstr *instr);

/* insert_* functions are used of compiler-generated instructions (not written by programmer). Such
 * instructions are marked as implicit. */
static MirInstr *
insert_instr_load(Context *cnt, MirInstr *src);

static MirInstr *
insert_instr_cast(Context *cnt, MirInstr *src, MirType *to_type);

static MirInstr *
insert_instr_addrof(Context *cnt, MirInstr *src);

static MirInstr *
insert_instr_toany(Context *cnt, MirInstr *expr);

/* Return cast operation if there is one between 'from' and 'to' type. */
static MirCastOp
get_cast_op(MirType *from, MirType *to);

static MirInstr *
_create_instr(Context *cnt, MirInstrKind kind, Ast *node);

static MirInstr *
create_instr_call_comptime(Context *cnt, Ast *node, MirInstr *fn);

/* Append functions creates desired instruction and push it into current basic block. */
static MirInstr *
append_instr_arg(Context *cnt, Ast *node, unsigned i);

static MirInstr *
append_instr_phi(Context *cnt, Ast *node);

static MirInstr *
append_instr_compound(Context *cnt, Ast *node, MirInstr *type, TSmallArray_InstrPtr *values);

static MirInstr *
append_instr_cast(Context *cnt, Ast *node, MirInstr *type, MirInstr *next);

static MirInstr *
append_instr_sizeof(Context *cnt, Ast *node, MirInstr *expr);

static MirInstr *
create_instr_type_info(Context *cnt, Ast *node, MirInstr *expr);

static MirInstr *
append_instr_type_info(Context *cnt, Ast *node, MirInstr *expr);

static MirInstr *
append_instr_alignof(Context *cnt, Ast *node, MirInstr *expr);

static MirInstr *
create_instr_elem_ptr(Context * cnt,
                      Ast *     node,
                      MirInstr *arr_ptr,
                      MirInstr *index,
                      bool      target_is_slice);

static MirInstr *
append_instr_elem_ptr(Context * cnt,
                      Ast *     node,
                      MirInstr *arr_ptr,
                      MirInstr *index,
                      bool      target_is_slice);

static MirInstr *
create_instr_member_ptr(Context *        cnt,
                        Ast *            node,
                        MirInstr *       target_ptr,
                        Ast *            member_ident,
                        ScopeEntry *     scope_entry,
                        MirBuiltinIdKind builtin_id);

static MirInstr *
append_instr_member_ptr(Context *        cnt,
                        Ast *            node,
                        MirInstr *       target_ptr,
                        Ast *            member_ident,
                        ScopeEntry *     scope_entry,
                        MirBuiltinIdKind builtin_id);

static MirInstr *
append_instr_cond_br(Context *      cnt,
                     Ast *          node,
                     MirInstr *     cond,
                     MirInstrBlock *then_block,
                     MirInstrBlock *else_block);

static MirInstr *
append_instr_br(Context *cnt, Ast *node, MirInstrBlock *then_block);

static MirInstr *
append_instr_switch(Context *               cnt,
                    Ast *                   node,
                    MirInstr *              value,
                    MirInstrBlock *         default_block,
                    bool                    user_defined_default,
                    TSmallArray_SwitchCase *cases);

static MirInstr *
append_instr_load(Context *cnt, Ast *node, MirInstr *src);

static MirInstr *
append_instr_type_fn(Context *cnt, Ast *node, MirInstr *ret_type, TSmallArray_InstrPtr *args);

static MirInstr *
append_instr_type_struct(Context *             cnt,
                         Ast *                 node,
                         ID *                  id,
                         MirInstr *            fwd_decl, /*Optional */
                         Scope *               scope,
                         TSmallArray_InstrPtr *members,
                         bool                  is_packed);

static MirInstr *
append_instr_type_enum(Context *             cnt,
                       Ast *                 node,
                       ID *                  id,
                       Scope *               scope,
                       TSmallArray_InstrPtr *variants,
                       MirInstr *            base_type);

static MirInstr *
append_instr_type_ptr(Context *cnt, Ast *node, MirInstr *type);

static MirInstr *
append_instr_type_array(Context *cnt, Ast *node, MirInstr *elem_type, MirInstr *len);

static MirInstr *
append_instr_type_slice(Context *cnt, Ast *node, MirInstr *elem_type);

static MirInstr *
append_instr_type_vargs(Context *cnt, Ast *node, MirInstr *elem_type);

MirInstr *
append_instr_type_const(Context *cnt, Ast *node, MirInstr *type);

static MirInstr *
append_instr_fn_proto(Context * cnt,
                      Ast *     node,
                      MirInstr *type,
                      MirInstr *user_type,
                      bool      schedule_analyze);

static MirInstr *
append_instr_decl_ref(Context *   cnt,
                      Ast *       node,
                      Unit *      parent_unit,
                      ID *        rid,
                      Scope *     scope,
                      ScopeEntry *scope_entry);

static MirInstr *
append_instr_decl_direct_ref(Context *cnt, MirInstr *ref);

static MirInstr *
append_instr_call(Context *cnt, Ast *node, MirInstr *callee, TSmallArray_InstrPtr *args);

static MirInstr *
append_instr_decl_var(Context * cnt,
                      Ast *     node,
                      MirInstr *type,
                      MirInstr *init,
                      bool      is_mutable,
                      bool      is_in_gscope,
                      s32       order, /* -1 of none */
                      u32       flags);

static MirInstr *
append_instr_decl_var_impl(Context *   cnt,
                           const char *name,
                           MirInstr *  type,
                           MirInstr *  init,
                           bool        is_mutable,
                           bool        is_in_gscope,
                           s32         order, /* -1 of none */
                           u32         flags);

static MirInstr *
append_instr_decl_member(Context *cnt, Ast *node, MirInstr *type);

static MirInstr *
append_instr_decl_member_impl(Context *cnt, Ast *node, ID *id, MirInstr *type);

static MirInstr *
append_instr_decl_arg(Context *cnt, Ast *node, MirInstr *type);

static MirInstr *
append_instr_decl_variant(Context *cnt, Ast *node, MirInstr *value);

static MirInstr *
create_instr_const_int(Context *cnt, Ast *node, MirType *type, u64 val);

static MirInstr *
append_instr_const_int(Context *cnt, Ast *node, MirType *type, u64 val);

static MirInstr *
append_instr_const_float(Context *cnt, Ast *node, float val);

static MirInstr *
append_instr_const_double(Context *cnt, Ast *node, double val);

static MirInstr *
append_instr_const_bool(Context *cnt, Ast *node, bool val);

static MirInstr *
append_instr_const_string(Context *cnt, Ast *node, const char *str);

static MirInstr *
append_instr_const_char(Context *cnt, Ast *node, char c);

static MirInstr *
append_instr_const_null(Context *cnt, Ast *node);

static MirInstr *
append_instr_ret(Context *cnt, Ast *node, MirInstr *value, bool infer_type);

static MirInstr *
append_instr_store(Context *cnt, Ast *node, MirInstr *src, MirInstr *dest);

static MirInstr *
append_instr_binop(Context *cnt, Ast *node, MirInstr *lhs, MirInstr *rhs, BinopKind op);

static MirInstr *
append_instr_unop(Context *cnt, Ast *node, MirInstr *instr, UnopKind op);

static MirInstr *
append_instr_unrecheable(Context *cnt, Ast *node);

static MirInstr *
create_instr_addrof(Context *cnt, Ast *node, MirInstr *src);

static MirInstr *
append_instr_addrof(Context *cnt, Ast *node, MirInstr *src);

/*
 * This will erase whole instruction tree of instruction with ref_count == 0. When force is set
 * ref_count is ignored.
 */
static void
erase_instr_tree(MirInstr *instr);

static MirInstr *
create_instr_vargs_impl(Context *cnt, MirType *type, TSmallArray_InstrPtr *values);

/* ast_* functions are used for generation of MIR from input AST. */
static MirInstr *
ast_create_impl_fn_call(Context *   cnt,
                        Ast *       node,
                        const char *fn_name,
                        MirType *   fn_type,
                        bool        schedule_analyze);

static MirInstr *
ast(Context *cnt, Ast *node);

static void
ast_ublock(Context *cnt, Ast *ublock);

static void
ast_test_case(Context *cnt, Ast *test);

static void
ast_unrecheable(Context *cnt, Ast *unr);

static void
ast_defer_block(Context *cnt, Ast *block, bool whole_tree);

static void
ast_block(Context *cnt, Ast *block);

static void
ast_stmt_if(Context *cnt, Ast *stmt_if);

static void
ast_stmt_return(Context *cnt, Ast *ret);

static void
ast_stmt_defer(Context *cnt, Ast *defer);

static void
ast_stmt_loop(Context *cnt, Ast *loop);

static void
ast_stmt_break(Context *cnt, Ast *br);

static void
ast_stmt_continue(Context *cnt, Ast *cont);

static void
ast_stmt_switch(Context *cnt, Ast *stmt_switch);

static MirInstr *
ast_decl_entity(Context *cnt, Ast *entity);

static MirInstr *
ast_decl_arg(Context *cnt, Ast *arg);

static MirInstr *
ast_decl_member(Context *cnt, Ast *arg);

static MirInstr *
ast_decl_variant(Context *cnt, Ast *variant);

static MirInstr *
ast_type_ref(Context *cnt, Ast *type_ref);

static MirInstr *
ast_type_struct(Context *cnt, Ast *type_struct);

static MirInstr *
ast_type_fn(Context *cnt, Ast *type_fn);

static MirInstr *
ast_type_arr(Context *cnt, Ast *type_arr);

static MirInstr *
ast_type_slice(Context *cnt, Ast *type_slice);

static MirInstr *
ast_type_ptr(Context *cnt, Ast *type_ptr);

static MirInstr *
ast_type_vargs(Context *cnt, Ast *type_vargs);

static MirInstr *
ast_type_enum(Context *cnt, Ast *type_enum);

static MirInstr *
ast_expr_line(Context *cnt, Ast *line);

static MirInstr *
ast_expr_file(Context *cnt, Ast *file);

static MirInstr *
ast_expr_addrof(Context *cnt, Ast *addrof);

static MirInstr *
ast_expr_cast(Context *cnt, Ast *cast);

static MirInstr *
ast_expr_sizeof(Context *cnt, Ast *szof);

static MirInstr *
ast_expr_type_info(Context *cnt, Ast *type_info);

static MirInstr *
ast_expr_alignof(Context *cnt, Ast *szof);

static MirInstr *
ast_expr_type(Context *cnt, Ast *type);

static MirInstr *
ast_expr_deref(Context *cnt, Ast *deref);

static MirInstr *
ast_expr_ref(Context *cnt, Ast *ref);

static MirInstr *
ast_expr_call(Context *cnt, Ast *call);

static MirInstr *
ast_expr_elem(Context *cnt, Ast *elem);

static MirInstr *
ast_expr_member(Context *cnt, Ast *member);

static MirInstr *
ast_expr_null(Context *cnt, Ast *nl);

static MirInstr *
ast_expr_lit_int(Context *cnt, Ast *expr);

static MirInstr *
ast_expr_lit_float(Context *cnt, Ast *expr);

static MirInstr *
ast_expr_lit_double(Context *cnt, Ast *expr);

static MirInstr *
ast_expr_lit_bool(Context *cnt, Ast *expr);

static MirInstr *
ast_expr_lit_fn(Context *cnt, Ast *lit_fn, Ast *decl_node, bool is_in_gscope, u32 flags);

static MirInstr *
ast_expr_lit_string(Context *cnt, Ast *lit_string);

static MirInstr *
ast_expr_lit_char(Context *cnt, Ast *lit_char);

static MirInstr *
ast_expr_binop(Context *cnt, Ast *binop);

static MirInstr *
ast_expr_unary(Context *cnt, Ast *unop);

static MirInstr *
ast_expr_compound(Context *cnt, Ast *cmp);

/* analyze */

/* This function has effect only on comptime instructions which can be evaluated directly after
 * they are analyzed. Reduction can use VM for execution of passed instruction and can also delete
 * the instruction from the owner block if actual value is const expression value. (Delete of an
 * instruction means only remove from basic block, all instruction's data will be keept utouched) */
static void
reduce_instr(Context *cnt, MirInstr *instr);

/* Main analyze entry function. */
static AnalyzeResult
analyze_instr(Context *cnt, MirInstr *instr);

/* This function takes configuration pipeline as 'conf', input instruction and optional slot type.
 * Behavior of this function depends on passed pipeline, it is used for implicit operations needed
 * by slot-owner instruction. */
static AnalyzeState
analyze_slot(Context *cnt, const AnalyzeSlotConfig *conf, MirInstr **input, MirType *slot_type);

/* Insert load instruction if needed. */
static AnalyzeStageState
analyze_stage_load(Context *cnt, MirInstr **input, MirType *slot_type);

/* Set null type constant if needed. */
static AnalyzeStageState
analyze_stage_set_null(Context *cnt, MirInstr **input, MirType *slot_type);

/* Set auto cast desired type if needed. */
static AnalyzeStageState
analyze_stage_set_auto(Context *cnt, MirInstr **input, MirType *slot_type);

/* Implicit conversion of input value to any. */
static AnalyzeStageState
analyze_stage_toany(Context *cnt, MirInstr **input, MirType *slot_type);

static AnalyzeStageState
analyze_stage_set_volatile_expr(Context *cnt, MirInstr **input, MirType *slot_type);

/* Do implicit cast if possible. */
static AnalyzeStageState
analyze_stage_implicit_cast(Context *cnt, MirInstr **input, MirType *slot_type);

static AnalyzeStageState
analyze_stage_report_type_mismatch(Context *cnt, MirInstr **input, MirType *slot_type);

/* Dummy slot analyze pipeline. */
static const AnalyzeSlotConfig analyze_slot_conf_reduce_only = {.count = 0};

/* Basic slot analyze pipeline generation only load instruction. */
static const AnalyzeSlotConfig analyze_slot_conf_basic = {.count  = 1,
                                                          .stages = {analyze_stage_load}};

/* Default slot analyze pipeline. */
static const AnalyzeSlotConfig analyze_slot_conf_default = {
    .count  = 6,
    .stages = {
        /* Set volatile type if needed. */
        analyze_stage_set_volatile_expr,
        /* Set type of null expression. */
        analyze_stage_set_null,
        /* Set destination type for auto cast. */
        analyze_stage_set_auto,
        /* Insert load instruction if needed. */
        analyze_stage_load,
        /* Generate implicit cast if possible. */
        analyze_stage_implicit_cast,
        /* Validate result type and expected type of en expression. */
        analyze_stage_report_type_mismatch,
    }};

/* Full slot analyze pipeline. */
static const AnalyzeSlotConfig analyze_slot_conf_full = {
    .count  = 7,
    .stages = {
        /* Set volatile type if needed. */
        analyze_stage_set_volatile_expr,
        /* Set type of null expression. */
        analyze_stage_set_null,
        /* Set destination type for auto cast. */
        analyze_stage_set_auto,
        /* Try to cast input expression to Any value. */
        analyze_stage_toany,
        /* Insert load instruction if needed. */
        analyze_stage_load,
        /* Generate implicit cast if possible. */
        analyze_stage_implicit_cast,
        /* Validate result type and expected type of en expression. */
        analyze_stage_report_type_mismatch,
    }};

/* This function produce analyze of implicit call to the type resolver function in MIR and set
 * out_type when analyze passed without problems. When analyze does not pass postpone is returned
 * and out_type stay unchanged.*/
static AnalyzeResult
analyze_resolve_type(Context *cnt, MirInstr *resolver_call, MirType **out_type);

static AnalyzeResult
analyze_instr_compound(Context *cnt, MirInstrCompound *cmp);

static AnalyzeResult
analyze_instr_phi(Context *cnt, MirInstrPhi *phi);

static AnalyzeResult
analyze_instr_toany(Context *cnt, MirInstrToAny *toany);

static AnalyzeResult
analyze_instr_vargs(Context *cnt, MirInstrVArgs *vargs);

static AnalyzeResult
analyze_instr_elem_ptr(Context *cnt, MirInstrElemPtr *elem_ptr);

static AnalyzeResult
analyze_instr_member_ptr(Context *cnt, MirInstrMemberPtr *member_ptr);

static AnalyzeResult
analyze_instr_addrof(Context *cnt, MirInstrAddrof *addrof);

static AnalyzeResult
analyze_instr_block(Context *cnt, MirInstrBlock *block);

static AnalyzeResult
analyze_instr_ret(Context *cnt, MirInstrRet *ret);

static AnalyzeResult
analyze_instr_arg(Context *cnt, MirInstrArg *arg);

static AnalyzeResult
analyze_instr_unop(Context *cnt, MirInstrUnop *unop);

static AnalyzeResult
analyze_instr_unreachable(Context *cnt, MirInstrUnreachable *unr);

static AnalyzeResult
analyze_instr_cond_br(Context *cnt, MirInstrCondBr *br);

static AnalyzeResult
analyze_instr_br(Context *cnt, MirInstrBr *br);

static AnalyzeResult
analyze_instr_switch(Context *cnt, MirInstrSwitch *sw);

static AnalyzeResult
analyze_instr_load(Context *cnt, MirInstrLoad *load);

static AnalyzeResult
analyze_instr_store(Context *cnt, MirInstrStore *store);

static AnalyzeResult
analyze_instr_fn_proto(Context *cnt, MirInstrFnProto *fn_proto);

static AnalyzeResult
analyze_instr_type_fn(Context *cnt, MirInstrTypeFn *type_fn);

static AnalyzeResult
analyze_instr_type_struct(Context *cnt, MirInstrTypeStruct *type_struct);

static AnalyzeResult
analyze_instr_type_slice(Context *cnt, MirInstrTypeSlice *type_slice);

static AnalyzeResult
analyze_instr_type_vargs(Context *cnt, MirInstrTypeVArgs *type_vargs);

static AnalyzeResult
analyze_instr_type_ptr(Context *cnt, MirInstrTypePtr *type_ptr);

static AnalyzeResult
analyze_instr_type_array(Context *cnt, MirInstrTypeArray *type_arr);

static AnalyzeResult
analyze_instr_type_enum(Context *cnt, MirInstrTypeEnum *type_enum);

static AnalyzeResult
analyze_instr_decl_var(Context *cnt, MirInstrDeclVar *decl);

static AnalyzeResult
analyze_instr_decl_member(Context *cnt, MirInstrDeclMember *decl);

static AnalyzeResult
analyze_instr_decl_variant(Context *cnt, MirInstrDeclVariant *variant_instr);

static AnalyzeResult
analyze_instr_decl_arg(Context *cnt, MirInstrDeclArg *decl);

static AnalyzeResult
analyze_instr_decl_ref(Context *cnt, MirInstrDeclRef *ref);

static AnalyzeResult
analyze_instr_decl_direct_ref(Context *cnt, MirInstrDeclDirectRef *ref);

static AnalyzeResult
analyze_instr_const(Context *cnt, MirInstrConst *cnst);

static AnalyzeResult
analyze_instr_call(Context *cnt, MirInstrCall *call);

static AnalyzeResult
analyze_instr_cast(Context *cnt, MirInstrCast *cast, bool analyze_op_only);

static AnalyzeResult
analyze_instr_sizeof(Context *cnt, MirInstrSizeof *szof);

static AnalyzeResult
analyze_instr_type_info(Context *cnt, MirInstrTypeInfo *type_info);

static AnalyzeResult
analyze_instr_alignof(Context *cnt, MirInstrAlignof *alof);

static AnalyzeResult
analyze_instr_binop(Context *cnt, MirInstrBinop *binop);

static void
analyze(Context *cnt);

static void
analyze_report_unresolved(Context *cnt);

/* Runtime type info data generation. Following functions are used for RTTI generation. */
static MirVar *
gen_RTTI(Context *cnt, MirType *type);

static MirConstValue *
gen_RTTI_base(Context *cnt, s32 kind, usize size_bytes);

static MirVar *
gen_RTTI_empty(Context *cnt, MirType *type, MirType *rtti_type);

static MirVar *
gen_RTTI_int(Context *cnt, MirType *type);

static MirVar *
gen_RTTI_real(Context *cnt, MirType *type);

static MirVar *
gen_RTTI_ptr(Context *cnt, MirType *type);

static MirConstValue *
gen_RTTI_enum_variant(Context *cnt, MirVariant *variant);

static MirConstValue *
gen_RTTI_slice_of_enum_variants(Context *cnt, TSmallArray_VariantPtr *variants);

static MirVar *
gen_RTTI_enum(Context *cnt, MirType *type);

static MirVar *
gen_RTTI_array(Context *cnt, MirType *type);

static MirConstValue *
gen_RTTI_slice_of_TypeInfo_ptr(Context *cnt, TSmallArray_TypePtr *types);

static MirConstValue *
gen_RTTI_struct_member(Context *cnt, MirMember *member);

static MirConstValue *
gen_RTTI_fn_arg(Context *cnt, MirArg *arg);

static MirConstValue *
gen_RTTI_slice_of_struct_members(Context *cnt, TSmallArray_MemberPtr *members);

static MirConstValue *
gen_RTTI_slice_of_fn_args(Context *cnt, TSmallArray_ArgPtr *args);

static MirVar *
gen_RTTI_struct(Context *cnt, MirType *type);

static MirVar *
gen_RTTI_fn(Context *cnt, MirType *type);

static void
gen_RTTI_types(Context *cnt);

/* INLINES */

static inline MirFn *
get_owner_fn(MirInstr *instr)
{
	if (!instr->owner_block) return NULL;
	return instr->owner_block->owner_fn;
}

static inline bool
is_var_comptime(MirVar *var)
{
	if (var->is_mutable) return false;
	return mir_is_comptime(&var->value);
}

/* Return MIR_VEM_STATIC or MIR_VEM_LAZY for constant value type. */
static inline MirValueEvaluationMode
choose_eval_mode_for_comptime(MirType *type)
{
	if (type->kind == MIR_TYPE_VOID) return MIR_VEM_NONE;
	return mir_is_composit_type(type) || type->kind == MIR_TYPE_ARRAY ? MIR_VEM_LAZY
	                                                                  : MIR_VEM_STATIC;
}

/* Get struct base type if there is one. */
static inline MirType *
get_base_type(MirType *struct_type)
{
	if (struct_type->kind != MIR_TYPE_STRUCT) return NULL;
	MirType *base_type = struct_type->data.strct.base_type;
	return base_type;
}

/* Get base type scope if there is one. */
static inline Scope *
get_base_type_scope(MirType *struct_type)
{
	MirType *base_type = get_base_type(struct_type);
	if (!base_type) return NULL;
	if (!mir_is_composit_type(base_type)) return NULL;

	return base_type->data.strct.scope;
}

/* Determinate if type is incomplete struct type. */
static inline bool
is_incomplete_struct_type(MirType *type)
{
	return type->kind == MIR_TYPE_STRUCT && type->data.strct.is_incomplete;
}

/* Determinate if instruction has volatile type, that means we can change type of the value during
 * analyze pass as needed. This is used for constant integer literals. */
static inline bool
is_instr_type_volatile(MirInstr *instr)
{
	MirType *type = instr->value.type;

	if (!type) return false;
	if (type->kind != MIR_TYPE_INT) return false;

	switch (instr->kind) {
	case MIR_INSTR_CONST:
		/* Integer constant literals has always volatile type. */
		return true;
	case MIR_INSTR_UNOP:
		return ((MirInstrUnop *)instr)->volatile_type;
	case MIR_INSTR_BINOP:
		return ((MirInstrBinop *)instr)->volatile_type;
	default:
		return false;
	}
}

/* True when type is pointer type. */
static inline bool
is_pointer_to_type_type(MirType *type)
{
	while (mir_is_pointer_type(type)) {
		type = mir_deref_type(type);
	}

	return type->kind == MIR_TYPE_TYPE;
}

/* Compare two types. True when they are equal. */
static inline bool
type_cmp(MirType *first, MirType *second)
{
	BL_ASSERT(first && second);
	return first->id.hash == second->id.hash;
}

/* True when iomplicit cast can be generated. */
static inline bool
can_impl_cast(MirType *from, MirType *to)
{
	if (from->kind != to->kind) return false;

	/* Check base types for structs. */
	if (from->kind == MIR_TYPE_PTR) {
		from = mir_deref_type(from);
		to   = mir_deref_type(to);

		while (true) {
			if (!from) return false;
			if (type_cmp(from, to)) {
				return true;
			} else {
				from = get_base_type(from);
			}
		}

		return false;
	}

	if (from->kind != MIR_TYPE_INT) return false;
	if (from->data.integer.is_signed != to->data.integer.is_signed) return false;

	const s32 fb = from->data.integer.bitcount;
	const s32 tb = to->data.integer.bitcount;

	if (fb > tb) return false;

	return true;
}

/* Get callee function from call instruction. Assert in debug when no such function exists. */
static inline MirFn *
get_callee(MirInstrCall *call)
{
	MirConstValue *callee_val = &call->callee->value;
	BL_ASSERT(callee_val->type && callee_val->type->kind == MIR_TYPE_FN);

	MirFn *fn = mir_get_const_ptr(MirFn *, &callee_val->data.v_ptr, MIR_CP_FN);
	BL_ASSERT(fn);
	return fn;
}

/* Return current basic block or NULL. */
static inline MirInstrBlock *
get_current_block(Context *cnt)
{
	return cnt->ast.current_block;
}

/* Return current insert function or NULL. */
static inline MirFn *
get_current_fn(Context *cnt)
{
	return cnt->ast.current_block ? cnt->ast.current_block->owner_fn : NULL;
}

/* Terminate block by terminator instruction. */
static inline void
terminate_block(MirInstrBlock *block, MirInstr *terminator)
{
	BL_ASSERT(block);
	if (block->terminal) BL_ABORT("basic block '%s' already terminated!", block->name);
	block->terminal = terminator;
}

/* True when block is terminated. */
static inline bool
is_block_terminated(MirInstrBlock *block)
{
	return block->terminal;
}

/* True when current insert block is terminated. */
static inline bool
is_current_block_terminated(Context *cnt)
{
	return get_current_block(cnt)->terminal;
}

static inline void
schedule_RTTI_generation(Context *cnt, MirType *type)
{
	if (!thtbl_has_key(&cnt->analyze.RTTI_entry_types, (u64)type))
		thtbl_insert_empty(&cnt->analyze.RTTI_entry_types, (u64)type);
}

/* Change instruction kind. */
static inline MirInstr *
mutate_instr(MirInstr *instr, MirInstrKind kind)
{
	BL_ASSERT(instr);
	instr->kind = kind;
	return instr;
}

/* Erase instruction from block. */
static inline void
erase_instr(MirInstr *instr)
{
	if (!instr) return;
	MirInstrBlock *block = instr->owner_block;
	if (!block) return;

	/* first in block */
	if (block->entry_instr == instr) block->entry_instr = instr->next;
	if (instr->prev) instr->prev->next = instr->next;
	if (instr->next) instr->next->prev = instr->prev;

	instr->prev = NULL;
	instr->next = NULL;
}

/* Insert instruction in block after 'after' instruction. */
static inline void
insert_instr_after(MirInstr *after, MirInstr *instr)
{
	BL_ASSERT(after && instr);

	MirInstrBlock *block = after->owner_block;
	instr->unrechable    = after->unrechable;

	instr->next = after->next;
	instr->prev = after;
	if (after->next) after->next->prev = instr;
	after->next = instr;

	instr->owner_block = block;
	if (block->last_instr == after) instr->owner_block->last_instr = instr;
}

/* Insert instruction in block before 'before' instruction. */
static inline void
insert_instr_before(MirInstr *before, MirInstr *instr)
{
	BL_ASSERT(before && instr);

	MirInstrBlock *block = before->owner_block;
	instr->unrechable    = before->unrechable;

	instr->next = before;
	instr->prev = before->prev;
	if (before->prev) before->prev->next = instr;
	before->prev = instr;

	instr->owner_block = block;
	if (block->entry_instr == before) instr->owner_block->entry_instr = instr;
}

/* Insert instruction into the global scope of assembly. */
static inline void
push_into_gscope(Context *cnt, MirInstr *instr)
{
	BL_ASSERT(instr);
	instr->id = cnt->assembly->MIR.global_instrs.size;
	tarray_push(&cnt->assembly->MIR.global_instrs, instr);
};

static inline void
analyze_push_back(Context *cnt, MirInstr *instr)
{
	BL_ASSERT(instr);
	tlist_push_back(&cnt->analyze.queue, instr);
}

static inline void
analyze_push_front(Context *cnt, MirInstr *instr)
{
	BL_ASSERT(instr);
	tlist_push_front(&cnt->analyze.queue, instr);
}

static inline void
analyze_notify_provided(Context *cnt, u64 hash)
{
	TIterator iter = thtbl_find(&cnt->analyze.waiting, hash);
	TIterator end  = thtbl_end(&cnt->analyze.waiting);
	if (TITERATOR_EQUAL(iter, end)) return; /* No one is waiting for this... */

#if BL_DEBUG && VERBOSE_ANALYZE
	printf("Analyze: Notify '%llu'.\n", (unsigned long long)hash);
#endif

	TArray *wq = &thtbl_iter_peek_value(TArray, iter);
	BL_ASSERT(wq);

	MirInstr *instr;
	TARRAY_FOREACH(MirInstr *, wq, instr)
	{
		analyze_push_back(cnt, instr);
	}

	/* Also clear element content! */
	thtbl_erase(&cnt->analyze.waiting, iter);
	tarray_terminate(wq);
}

/* Required analyze of complier-generated instruction. */
static inline void
analyze_instr_rq(Context *cnt, MirInstr *instr)
{
	if (analyze_instr(cnt, instr).state != ANALYZE_PASSED)
		BL_WARNING("invalid analyze of compiler-generated instruction: %s",
		           mir_instr_name(instr));
}

/* Generate unique name with optional prefix. */
static inline const char *
gen_uq_name(const char *prefix)
{
	static s32 ui = 0;
	/* RACECOND */
	/* RACECOND */
	/* RACECOND */
	TString *s = builder_create_cached_str();

	tstring_append(s, prefix);
	char ui_str[22];
	sprintf(ui_str, ".%i", ui++);
	tstring_append(s, ui_str);
	return s->data;
}

static inline bool
is_builtin(Ast *ident, MirBuiltinIdKind kind)
{
	if (!ident) return false;
	BL_ASSERT(ident->kind == AST_IDENT);
	return ident->data.ident.id.hash == builtin_ids[kind].hash;
}

static inline bool
get_block_terminator(MirInstrBlock *block)
{
	return block->terminal;
}

static inline void
set_current_block(Context *cnt, MirInstrBlock *block)
{
	cnt->ast.current_block = block;
}

static inline void
error_types(MirType *from, MirType *to, Ast *loc, const char *msg)
{
	BL_ASSERT(from && to);
	if (!msg) msg = "No implicit cast for type '%s' and '%s'.";

	char tmp_from[256];
	char tmp_to[256];
	mir_type_to_str(tmp_from, 256, from, true);
	mir_type_to_str(tmp_to, 256, to, true);

	builder_msg(BUILDER_MSG_ERROR,
	            ERR_INVALID_TYPE,
	            loc->location,
	            BUILDER_CUR_WORD,
	            msg,
	            tmp_from,
	            tmp_to);
}

static inline void
commit_fn(Context *cnt, MirFn *fn)
{
	ID *id = fn->id;
	BL_ASSERT(id);

	ScopeEntry *entry = scope_lookup(fn->decl_node->owner_scope, id, true, false);
	BL_ASSERT(entry && "cannot commit unregistred function");

	entry->kind    = SCOPE_ENTRY_FN;
	entry->data.fn = fn;

	analyze_notify_provided(cnt, id->hash);
}

static inline void
commit_variant(Context *cnt, MirVariant *v)
{
	ID *id = v->id;
	BL_ASSERT(id);

	ScopeEntry *entry = scope_lookup(v->decl_scope, id, false, true);
	BL_ASSERT(entry && "cannot commit unregistred variant");

	entry->kind         = SCOPE_ENTRY_VARIANT;
	entry->data.variant = v;
}

static inline void
commit_member(Context *cnt, MirMember *member)
{
	ID *id = member->id;
	BL_ASSERT(id);

	ScopeEntry *entry = scope_lookup(member->decl_scope, id, false, true);
	BL_ASSERT(entry && "cannot commit unregistred member");

	entry->kind        = SCOPE_ENTRY_MEMBER;
	entry->data.member = member;
}

static inline void
commit_var(Context *cnt, MirVar *var)
{
	ID *id = var->id;
	BL_ASSERT(id);

	ScopeEntry *entry = scope_lookup(var->decl_scope, id, true, false);
	BL_ASSERT(entry && "cannot commit unregistred var");

	entry->kind     = SCOPE_ENTRY_VAR;
	entry->data.var = var;

	if (var->is_in_gscope) analyze_notify_provided(cnt, id->hash);
}

/*
 * Provide builtin type. Register & commit.
 */
static inline void
provide_builtin_type(Context *cnt, MirType *type)
{
	ScopeEntry *entry =
	    register_symbol(cnt, NULL, type->user_id, cnt->assembly->gscope, true, false);
	if (!entry) return;

	entry->kind      = SCOPE_ENTRY_TYPE;
	entry->data.type = type;
}

static inline void
provide_builtin_member(Context *cnt, Scope *scope, MirMember *member)
{
	ScopeEntry *entry = register_symbol(cnt, NULL, member->id, scope, false, false);
	if (!entry) return;

	entry->kind        = SCOPE_ENTRY_MEMBER;
	entry->data.member = member;
}

static inline void
unref_instr(MirInstr *instr)
{
	if (!instr || instr->ref_count == NO_REF_COUNTING) return;
	--instr->ref_count;
}

static inline void
ref_instr(MirInstr *instr)
{
	if (!instr || instr->ref_count == NO_REF_COUNTING) return;
	++instr->ref_count;
}

static inline void
phi_add_income(MirInstrPhi *phi, MirInstr *value, MirInstrBlock *block)
{
	BL_ASSERT(phi && value && block);
	ref_instr(value);
	ref_instr(&block->base);

	tsa_push_InstrPtr(phi->incoming_values, value);
	tsa_push_InstrPtr(phi->incoming_blocks, &block->base);
}

static inline bool
is_load_needed(MirInstr *instr)
{
	if (!instr) return false;
	if (!mir_is_pointer_type(instr->value.type)) return false;

	switch (instr->kind) {
	case MIR_INSTR_ARG:
	case MIR_INSTR_UNOP:
	case MIR_INSTR_CONST:
	case MIR_INSTR_BINOP:
	case MIR_INSTR_CALL:
	case MIR_INSTR_ADDROF:
	case MIR_INSTR_TYPE_ARRAY:
	case MIR_INSTR_TYPE_FN:
	case MIR_INSTR_TYPE_PTR:
	case MIR_INSTR_TYPE_STRUCT:
	case MIR_INSTR_CAST:
	case MIR_INSTR_DECL_MEMBER:
	case MIR_INSTR_TYPE_INFO:
		return false;

	default:
		break;
	}

	return true;
}

static inline bool
is_to_any_needed(Context *cnt, MirInstr *src, MirType *dest_type)
{
	if (!dest_type || !src) return false;
	MirType *any_type = lookup_builtin(cnt, MIR_BUILTIN_ID_ANY);
	BL_ASSERT(any_type);
	if (dest_type != any_type) return false;

	if (is_load_needed(src)) {
		MirType *src_type = src->value.type;
		if (mir_deref_type(src_type) == any_type) return false;
	}

	return true;
}

/* string hash functions for types */
static inline const char *
sh_type_null(Context *cnt, MirType *base_type)
{
	BL_ASSERT(base_type->id.str);
	TString *tmp = &cnt->tmp_sh;
	tstring_clear(tmp);
	tstring_append(tmp, "n.");
	tstring_append(tmp, base_type->id.str);
	return tmp->data;
}

static inline const char *
sh_type_ptr(Context *cnt, MirType *src_type)
{
	BL_ASSERT(src_type->id.str);
	TString *tmp = &cnt->tmp_sh;
	tstring_clear(tmp);
	tstring_append(tmp, "p.");
	tstring_append(tmp, src_type->id.str);
	return tmp->data;
}

static inline const char *
sh_type_fn(Context *cnt, MirType *ret_type, TSmallArray_ArgPtr *args, bool is_vargs)
{
	// BL_ASSERT(src_type->id.str);
	TString *tmp = &cnt->tmp_sh;
	tstring_clear(tmp);
	tstring_append(tmp, "f.(");

	/* append all arg types isd */
	if (args) {
		MirArg *arg;
		TSA_FOREACH(args, arg)
		{
			BL_ASSERT(arg->type->id.str);
			tstring_append(tmp, arg->type->id.str);

			if (i != args->size - 1) tstring_append(tmp, ",");
		}
	}

	tstring_append(tmp, ")");

	if (ret_type) {
		BL_ASSERT(ret_type->id.str);
		tstring_append(tmp, ret_type->id.str);
	} else {
		/* implicit return void */
		tstring_append(tmp, cnt->builtin_types.t_void->id.str);
	}

	return tmp->data;
}

static inline const char *
sh_type_arr(Context *cnt, MirType *elem_type, s64 len)
{
	BL_ASSERT(elem_type->id.str);
	TString *tmp = &cnt->tmp_sh;
	tstring_clear(tmp);

	char ui_str[21];
	sprintf(ui_str, "%llu", (unsigned long long)len);

	tstring_append(tmp, ui_str);
	tstring_append(tmp, ".");
	tstring_append(tmp, elem_type->id.str);
	return tmp->data;
}

static inline const char *
sh_type_struct(Context *              cnt,
               MirTypeKind            kind,
               ID *                   id,
               TSmallArray_MemberPtr *members,
               bool                   is_packed)
{
	BL_ASSERT(!is_packed);
	TString *tmp = &cnt->tmp_sh;
	tstring_clear(tmp);

	switch (kind) {
	case MIR_TYPE_STRUCT:
		tstring_append(tmp, "s.");
		break;
	case MIR_TYPE_SLICE:
		tstring_append(tmp, "sl.");
		break;
	case MIR_TYPE_STRING:
		tstring_append(tmp, "ss.");
		break;
	case MIR_TYPE_VARGS:
		tstring_append(tmp, "sv.");
		break;
	default:
		BL_ABORT("Expected struct base type.");
	}

	if (id) {
		tstring_append(tmp, id->str);
	}

	tstring_append(tmp, "{");
	if (members) {
		MirMember *member;
		TSA_FOREACH(members, member)
		{
			BL_ASSERT(member->type->id.str);
			tstring_append(tmp, member->type->id.str);

			if (i != members->size - 1) tstring_append(tmp, ",");
		}
	}

	tstring_append(tmp, "}");
	return tmp->data;
}

static inline const char *
sh_type_enum(Context *cnt, ID *id, MirType *base_type, TSmallArray_VariantPtr *variants)
{
	BL_ASSERT(base_type->id.str);
	TString *tmp = &cnt->tmp_sh;
	tstring_clear(tmp);

	tstring_append(tmp, "e.");

	if (id) tstring_append(tmp, id->str);

	tstring_append(tmp, "(");
	tstring_append(tmp, base_type->id.str);
	tstring_append(tmp, ")");

	tstring_append(tmp, "{");
	if (variants) {
		MirVariant *variant;
		TSA_FOREACH(variants, variant)
		{
			BL_ASSERT(variant->value);

			char value_str[35];
			snprintf(value_str,
			         TARRAY_SIZE(value_str),
			         "%lld",
			         (long long)variant->value->data.v_s64);
			tstring_append(tmp, value_str);

			if (i != variants->size - 1) tstring_append(tmp, ",");
		}
	}
	tstring_append(tmp, "}");
	return tmp->data;
}

void
init_type_id(Context *cnt, MirType *type)
{
	/******************************************************************************************/
#define GEN_ID_STRUCT                                                                              \
	if (type->user_id) {                                                                       \
		tstring_append(tmp, type->user_id->str);                                           \
	}                                                                                          \
                                                                                                   \
	tstring_append(tmp, "{");                                                                  \
	if (type->data.strct.members) {                                                            \
		MirMember *member;                                                                 \
		TSA_FOREACH(type->data.strct.members, member)                                      \
		{                                                                                  \
			BL_ASSERT(member->type->id.str);                                           \
			tstring_append(tmp, member->type->id.str);                                 \
                                                                                                   \
			if (i != type->data.strct.members->size - 1) tstring_append(tmp, ",");     \
		}                                                                                  \
	}                                                                                          \
                                                                                                   \
	tstring_append(tmp, "}");                                                                  \
	/******************************************************************************************/

	BL_ASSERT(type && "Invalid type pointer!");
	TString *tmp = &cnt->tmp_sh;
	tstring_clear(tmp);

	switch (type->kind) {
	case MIR_TYPE_BOOL:
	case MIR_TYPE_VOID:
	case MIR_TYPE_TYPE:
	case MIR_TYPE_REAL:
	case MIR_TYPE_INT: {
		BL_ASSERT(type->user_id);
		tstring_append(tmp, type->user_id->str);
		break;
	}

	case MIR_TYPE_NULL: {
		/* n.<name> */
		tstring_clear(tmp);
		tstring_append(tmp, "n.");
		tstring_append(tmp, type->data.null.base_type->id.str);
		break;
	}

	case MIR_TYPE_PTR: {
		/* p.<name> */
		tstring_clear(tmp);
		tstring_append(tmp, "p.");
		tstring_append(tmp, type->data.ptr.expr->id.str);

		break;
	}

	case MIR_TYPE_FN: {
		tstring_append(tmp, "f.(");

		/* append all arg types isd */
		if (type->data.fn.args) {
			MirArg *arg;
			TSA_FOREACH(type->data.fn.args, arg)
			{
				BL_ASSERT(arg->type->id.str);
				tstring_append(tmp, arg->type->id.str);

				if (i != type->data.fn.args->size - 1) tstring_append(tmp, ",");
			}
		}

		tstring_append(tmp, ")");

		if (type->data.fn.ret_type) {
			BL_ASSERT(type->data.fn.ret_type->id.str);
			tstring_append(tmp, type->data.fn.ret_type->id.str);
		} else {
			/* implicit return void */
			tstring_append(tmp, cnt->builtin_types.t_void->id.str);
		}
		break;
	}

	case MIR_TYPE_ARRAY: {
		char ui_str[21];
		sprintf(ui_str, "%llu", (unsigned long long)type->data.array.len);

		tstring_append(tmp, ui_str);
		tstring_append(tmp, ".");
		tstring_append(tmp, type->data.array.elem_type->id.str);
		break;
	}

	case MIR_TYPE_STRING: {
		tstring_append(tmp, "ss.");
		GEN_ID_STRUCT;
		break;
	}

	case MIR_TYPE_SLICE: {
		tstring_append(tmp, "sl.");
		GEN_ID_STRUCT;
		break;
	}

	case MIR_TYPE_VARGS: {
		tstring_append(tmp, "sv.");
		GEN_ID_STRUCT;
		break;
	}

	case MIR_TYPE_STRUCT: {
		tstring_append(tmp, "s.");
		if (type->data.strct.is_incomplete) {
			BL_ASSERT(type->user_id &&
			          "Missing user id for incomplete structure type!");
			tstring_append(tmp, type->user_id->str);
		} else {
			GEN_ID_STRUCT;
		}

		break;
	}

	case MIR_TYPE_ENUM: {
		tstring_append(tmp, "e.");

		if (type->user_id) tstring_append(tmp, type->user_id->str);

		tstring_append(tmp, "(");
		tstring_append(tmp, type->data.enm.base_type->id.str);
		tstring_append(tmp, ")");

		tstring_append(tmp, "{");
		if (type->data.enm.variants) {
			MirVariant *variant;
			TSA_FOREACH(type->data.enm.variants, variant)
			{
				BL_ASSERT(variant->value);

				char value_str[35];
				snprintf(value_str,
				         TARRAY_SIZE(value_str),
				         "%lld",
				         (long long)variant->value->data.v_s64);
				tstring_append(tmp, value_str);

				if (i != type->data.enm.variants->size - 1)
					tstring_append(tmp, ",");
			}
		}
		tstring_append(tmp, "}");
		break;
	}

	default:
		BL_UNIMPLEMENTED;
	}

	TString *copy = builder_create_cached_str();
	tstring_append(copy, tmp->data);

	type->id.str  = copy->data;
	type->id.hash = thash_from_str(copy->data);

#undef GEN_ID_STRUCT
}

MirType *
create_type(Context *cnt, MirTypeKind kind, ID *user_id)
{
	MirType *type = arena_alloc(&cnt->assembly->arenas.mir.type);
	type->kind    = kind;
	type->user_id = user_id;

	return type;
}

ScopeEntry *
register_symbol(Context *cnt, Ast *node, ID *id, Scope *scope, bool is_builtin, bool enable_groups)
{
	BL_ASSERT(id && "Missing symbol ID.");
	BL_ASSERT(scope && "Missing entry scope.");

	const bool  is_private = scope->kind == SCOPE_PRIVATE;
	ScopeEntry *collision  = scope_lookup(scope, id, is_private, false);

	if (collision) {
		if (!is_private) goto COLLIDE;

		const bool collision_in_same_unit =
		    (node ? node->location->unit : NULL) ==
		    (collision->node ? collision->node->location->unit : NULL);

		if (collision_in_same_unit) {
			goto COLLIDE;
		}
	}

	/* no collision */
	ScopeEntry *entry = scope_create_entry(
	    &cnt->assembly->arenas.scope, SCOPE_ENTRY_INCOMPLETE, id, node, is_builtin);

	scope_insert(scope, entry);
	return entry;

COLLIDE : {
	char *err_msg = collision->is_buildin || is_builtin
	                    ? "Symbol name colision with compiler builtin '%s'."
	                    : "Duplicate symbol";

	builder_msg(BUILDER_MSG_ERROR,
	            ERR_DUPLICATE_SYMBOL,
	            node ? node->location : NULL,
	            BUILDER_CUR_WORD,
	            err_msg,
	            id->str);

	if (collision->node) {
		builder_msg(BUILDER_MSG_NOTE,
		            0,
		            collision->node->location,
		            BUILDER_CUR_WORD,
		            "Previous declaration found here.");
	}

	return NULL;
}
}

MirType *
lookup_builtin(Context *cnt, MirBuiltinIdKind kind)
{
	ID *        id    = &builtin_ids[kind];
	Scope *     scope = cnt->assembly->gscope;
	ScopeEntry *found = scope_lookup(scope, id, true, false);

	if (!found) BL_ABORT("Missing compiler internal symbol '%s'", id->str);
	if (found->kind == SCOPE_ENTRY_INCOMPLETE) return NULL;

	if (!found->is_buildin) {
		builder_msg(BUILDER_MSG_WARNING,
		            0,
		            found->node ? found->node->location : NULL,
		            BUILDER_CUR_WORD,
		            "Builtins used by compiler must have '#compiler' flag!");
	}

	BL_ASSERT(found->kind == SCOPE_ENTRY_VAR);

	MirVar *var = found->data.var;

	BL_ASSERT(var);
	BL_ASSERT(mir_is_comptime(&var->value));

	MirType *type = mir_get_const_ptr(MirType *, &var->value.data.v_ptr, MIR_CP_TYPE);

	/* Wait when internal is not complete!  */
	if (is_incomplete_struct_type(type)) {
		return NULL;
	}

	return type;
}

MirType *
create_type_type(Context *cnt)
{
	MirType *tmp = create_type(cnt, MIR_TYPE_TYPE, &builtin_ids[MIR_BUILTIN_ID_TYPE_TYPE]);
	/* NOTE: TypeType has no LLVM representation */
	tmp->alignment        = __alignof(MirType *);
	tmp->size_bits        = sizeof(MirType *) * 8;
	tmp->store_size_bytes = sizeof(MirType *);

	init_type_id(cnt, tmp);

	return tmp;
}

MirType *
create_type_null(Context *cnt, MirType *base_type)
{
	BL_ASSERT(base_type);
	MirType *tmp = create_type(cnt, MIR_TYPE_NULL, &builtin_ids[MIR_BUILTIN_ID_NULL]);
	tmp->data.null.base_type = base_type;

	init_type_id(cnt, tmp);
	init_llvm_type_null(cnt, tmp);

	return tmp;
}

MirType *
create_type_void(Context *cnt)
{
	MirType *tmp = create_type(cnt, MIR_TYPE_VOID, &builtin_ids[MIR_BUILTIN_ID_TYPE_VOID]);

	init_type_id(cnt, tmp);
	init_llvm_type_void(cnt, tmp);

	return tmp;
}

MirType *
create_type_bool(Context *cnt)
{
	MirType *tmp = create_type(cnt, MIR_TYPE_BOOL, &builtin_ids[MIR_BUILTIN_ID_TYPE_BOOL]);

	init_type_id(cnt, tmp);
	init_llvm_type_bool(cnt, tmp);

	return tmp;
}

MirType *
create_type_int(Context *cnt, ID *id, s32 bitcount, bool is_signed)
{
	BL_ASSERT(id);
	BL_ASSERT(bitcount > 0);
	MirType *tmp                = create_type(cnt, MIR_TYPE_INT, id);
	tmp->data.integer.bitcount  = bitcount;
	tmp->data.integer.is_signed = is_signed;

	init_type_id(cnt, tmp);
	init_llvm_type_int(cnt, tmp);

	return tmp;
}

MirType *
create_type_real(Context *cnt, ID *id, s32 bitcount)
{
	BL_ASSERT(bitcount > 0);
	MirType *tmp            = create_type(cnt, MIR_TYPE_REAL, id);
	tmp->data.real.bitcount = bitcount;

	init_type_id(cnt, tmp);
	init_llvm_type_real(cnt, tmp);

	return tmp;
}

MirType *
create_type_ptr(Context *cnt, MirType *src_type)
{
	BL_ASSERT(src_type && "Invalid src type for pointer type.");
	MirType *tmp       = create_type(cnt, MIR_TYPE_PTR, NULL);
	tmp->data.ptr.expr = src_type;

	init_type_id(cnt, tmp);
	init_llvm_type_ptr(cnt, tmp);

	return tmp;
}

MirType *
create_type_fn(Context *cnt, ID *id, MirType *ret_type, TSmallArray_ArgPtr *args, bool is_vargs)
{
	MirType *tmp          = create_type(cnt, MIR_TYPE_FN, id);
	tmp->data.fn.args     = args;
	tmp->data.fn.is_vargs = is_vargs;
	tmp->data.fn.ret_type = ret_type ? ret_type : cnt->builtin_types.t_void;

	init_type_id(cnt, tmp);
	init_llvm_type_fn(cnt, tmp);

	return tmp;
}

MirType *
create_type_array(Context *cnt, MirType *elem_type, s64 len)
{
	MirType *tmp              = create_type(cnt, MIR_TYPE_ARRAY, NULL);
	tmp->data.array.elem_type = elem_type;
	tmp->data.array.len       = len;

	init_type_id(cnt, tmp);
	init_llvm_type_array(cnt, tmp);

	return tmp;
}

MirType *
create_type_struct(Context *              cnt,
                   MirTypeKind            kind,
                   ID *                   id,
                   Scope *                scope,
                   TSmallArray_MemberPtr *members,   /* MirMember */
                   MirType *              base_type, /* optional */
                   bool                   is_packed)
{
	MirType *tmp = create_type(cnt, kind, id);

	tmp->data.strct.members   = members;
	tmp->data.strct.scope     = scope;
	tmp->data.strct.is_packed = is_packed;
	tmp->data.strct.base_type = base_type;

	init_type_id(cnt, tmp);
	init_llvm_type_struct(cnt, tmp);

	return tmp;
}

MirType *
complete_type_struct(Context *              cnt,
                     MirInstr *             fwd_decl,
                     Scope *                scope,
                     TSmallArray_MemberPtr *members,
                     MirType *              base_type,
                     bool                   is_packed)
{
	BL_ASSERT(fwd_decl && "Invalid fwd_decl pointer!");

	BL_ASSERT(fwd_decl->value.type->kind == MIR_TYPE_TYPE &&
	          "Forward struct declaration does not point to type definition!");

	MirType *incomplete_type =
	    mir_get_const_ptr(MirType *, &fwd_decl->value.data.v_ptr, MIR_CP_TYPE);
	BL_ASSERT(incomplete_type);

	BL_ASSERT(incomplete_type->kind == MIR_TYPE_STRUCT &&
	          "Incomplete type is not struct type!");

	BL_ASSERT(incomplete_type->data.strct.is_incomplete &&
	          "Incomplete struct type is not marked as incomplete!");

	incomplete_type->data.strct.members       = members;
	incomplete_type->data.strct.scope         = scope;
	incomplete_type->data.strct.is_packed     = is_packed;
	incomplete_type->data.strct.is_incomplete = false;
	incomplete_type->data.strct.base_type     = base_type;

	init_llvm_type_struct(cnt, incomplete_type);
	return incomplete_type;
}

MirType *
create_type_struct_incomplete(Context *cnt, ID *user_id)
{
	MirType *tmp                  = create_type(cnt, MIR_TYPE_STRUCT, user_id);
	tmp->data.strct.is_incomplete = true;

	init_type_id(cnt, tmp);
	init_llvm_type_struct(cnt, tmp);
	return tmp;
}

MirType *
create_type_struct_special(Context *cnt, MirTypeKind kind, ID *id, MirType *elem_ptr_type)
{
	BL_ASSERT(mir_is_pointer_type(elem_ptr_type));
	BL_ASSERT(kind == MIR_TYPE_STRING || kind == MIR_TYPE_VARGS || kind == MIR_TYPE_SLICE);

	TSmallArray_MemberPtr *members = create_sarr(TSmallArray_MemberPtr, cnt->assembly);

	/* Slice layout struct { s64, *T } */
	Scope *body_scope = scope_create(
	    &cnt->assembly->arenas.scope, SCOPE_TYPE_STRUCT, cnt->assembly->gscope, 2, NULL);

	MirMember *tmp;
	tmp = create_member(cnt,
	                    NULL,
	                    &builtin_ids[MIR_BUILTIN_ID_ARR_LEN],
	                    body_scope,
	                    0,
	                    cnt->builtin_types.t_s64);

	tsa_push_MemberPtr(members, tmp);
	provide_builtin_member(cnt, body_scope, tmp);

	tmp = create_member(
	    cnt, NULL, &builtin_ids[MIR_BUILTIN_ID_ARR_PTR], body_scope, 1, elem_ptr_type);

	tsa_push_MemberPtr(members, tmp);
	provide_builtin_member(cnt, body_scope, tmp);

	return create_type_struct(cnt, kind, id, body_scope, members, NULL, false);
}

MirType *
create_type_enum(Context *               cnt,
                 ID *                    id,
                 Scope *                 scope,
                 MirType *               base_type,
                 TSmallArray_VariantPtr *variants)
{
	BL_ASSERT(base_type);
	MirType *tmp            = create_type(cnt, MIR_TYPE_ENUM, id);
	tmp->data.enm.scope     = scope;
	tmp->data.enm.base_type = base_type;
	tmp->data.enm.variants  = variants;

	init_type_id(cnt, tmp);
	init_llvm_type_enum(cnt, tmp);

	return tmp;
}

void
init_llvm_type_int(Context *cnt, MirType *type)
{
	type->llvm_type        = LLVMIntTypeInContext(cnt->assembly->llvm.cnt,
                                               (unsigned int)type->data.integer.bitcount);
	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment        = LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	/*** DI ***/
	if (!cnt->debug_mode) return;

	const char *    name = type->user_id ? type->user_id->str : type->id.str;
	DW_ATE_Encoding encoding;

	if (type->data.integer.is_signed) {
		if (type->size_bits == 8)
			encoding = DW_ATE_signed_char;
		else
			encoding = DW_ATE_signed;
	} else {
		if (type->size_bits == 8)
			encoding = DW_ATE_unsigned_char;
		else
			encoding = DW_ATE_unsigned;
	}

	type->llvm_meta = llvm_di_create_basic_type(
	    cnt->analyze.llvm_di_builder, name, type->size_bits, encoding);
}

void
init_llvm_type_real(Context *cnt, MirType *type)
{
	if (type->data.real.bitcount == 32)
		type->llvm_type = LLVMFloatTypeInContext(cnt->assembly->llvm.cnt);
	else if (type->data.real.bitcount == 64)
		type->llvm_type = LLVMDoubleTypeInContext(cnt->assembly->llvm.cnt);
	else
		BL_ABORT("invalid floating point type");

	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment = (s32)LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	/*** DI ***/
	if (!cnt->debug_mode) return;

	const char *name = type->user_id ? type->user_id->str : type->id.str;

	type->llvm_meta = llvm_di_create_basic_type(
	    cnt->analyze.llvm_di_builder, name, (unsigned)type->size_bits, DW_ATE_float);
}

void
init_llvm_type_ptr(Context *cnt, MirType *type)
{
	MirType *tmp = mir_deref_type(type);
	/* Pointer to Type has no LLVM representation and cannot not be generated into IR.*/
	if (tmp->kind == MIR_TYPE_TYPE) return;

	BL_ASSERT(tmp);
	BL_ASSERT(tmp->llvm_type);
	type->llvm_type        = LLVMPointerType(tmp->llvm_type, 0);
	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment = (s32)LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	/*** DI ***/
	if (!cnt->debug_mode) return;

	const char *name = type->user_id ? type->user_id->str : type->id.str;
	type->llvm_meta  = llvm_di_create_pointer_type(cnt->analyze.llvm_di_builder,
                                                      tmp->llvm_meta,
                                                      type->size_bits,
                                                      (unsigned)type->alignment * 8,
                                                      name);
}

void
init_llvm_type_void(Context *cnt, MirType *type)
{
	type->alignment        = 0;
	type->size_bits        = 0;
	type->store_size_bytes = 0;
	type->llvm_type        = LLVMVoidTypeInContext(cnt->assembly->llvm.cnt);

	/*** DI ***/
	if (!cnt->debug_mode) return;

	type->llvm_meta = llvm_di_create_basic_type(
	    cnt->analyze.llvm_di_builder, "void", 8, DW_ATE_unsigned_char);
}

void
init_llvm_type_null(Context *cnt, MirType *type)
{
	MirType *tmp = type->data.null.base_type;
	BL_ASSERT(tmp);
	BL_ASSERT(tmp->llvm_type);
	type->llvm_type        = tmp->llvm_type;
	type->alignment        = tmp->alignment;
	type->size_bits        = tmp->size_bits;
	type->store_size_bytes = tmp->store_size_bytes;

	/*** DI ***/
	if (!cnt->debug_mode) return;
	type->llvm_meta = llvm_di_create_null_type(cnt->analyze.llvm_di_builder);
}

void
init_llvm_type_bool(Context *cnt, MirType *type)
{
	type->llvm_type        = LLVMIntTypeInContext(cnt->assembly->llvm.cnt, 1);
	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment = (s32)LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	/*** DI ***/
	if (!cnt->debug_mode) return;

	const char *name = type->user_id ? type->user_id->str : type->id.str;
	type->llvm_meta =
	    llvm_di_create_basic_type(cnt->analyze.llvm_di_builder, name, 8, DW_ATE_boolean);
}

static inline usize
struct_split_fit(Context *cnt, MirType *struct_type, u32 bound, u32 *start)
{
	s64 so     = mir_get_struct_elem_offest(cnt->assembly, struct_type, *start);
	u32 offset = 0;
	u32 size   = 0;
	u32 total  = 0;
	for (; *start < struct_type->data.strct.members->size; ++(*start)) {
		offset =
		    (u32)mir_get_struct_elem_offest(cnt->assembly, struct_type, *start) - (u32)so;
		size = (u32)mir_get_struct_elem_type(struct_type, *start)->store_size_bytes;
		if (offset + size > bound) return bound;
		total = offset + size;
	}

	return total > 1 ? next_pow_2((u32)total) : total;
}

void
init_llvm_type_fn(Context *cnt, MirType *type)
{
	MirType *ret_type = type->data.fn.ret_type;

	LLVMTypeRef         llvm_ret  = NULL;
	TSmallArray_ArgPtr *args      = type->data.fn.args;
	const bool          has_args  = args;
	const bool          has_ret   = ret_type;
	bool                has_byval = false;

	if (has_ret && ret_type->kind == MIR_TYPE_TYPE) {
		return;
	}

	TSmallArray_LLVMType llvm_args;
	tsa_init(&llvm_args);

	if (has_ret) {
		if (builder.options.reg_split && mir_is_composit_type(ret_type) &&
		    ret_type->store_size_bytes > 16) {
			type->data.fn.has_sret = true;
			tsa_push_LLVMType(&llvm_args, LLVMPointerType(ret_type->llvm_type, 0));
			llvm_ret = LLVMVoidTypeInContext(cnt->assembly->llvm.cnt);
		} else {
			llvm_ret = ret_type->llvm_type;
		}
	} else {
		llvm_ret = LLVMVoidTypeInContext(cnt->assembly->llvm.cnt);
	}

	BL_ASSERT(llvm_ret);

	if (has_args) {
		MirArg *arg;
		TSA_FOREACH(args, arg)
		{
			arg->llvm_index = (u32)llvm_args.size;

			/* Composit types. */
			if (builder.options.reg_split && mir_is_composit_type(arg->type)) {
				LLVMContextRef llvm_cnt = cnt->assembly->llvm.cnt;
				u32            start    = 0;
				usize          low      = 0;
				usize          high     = 0;

				if (!has_byval) has_byval = true;

				low = struct_split_fit(cnt, arg->type, sizeof(usize), &start);

				if (start < arg->type->data.strct.members->size)
					high =
					    struct_split_fit(cnt, arg->type, sizeof(usize), &start);

				if (start < arg->type->data.strct.members->size) {
					arg->llvm_easgm = LLVM_EASGM_BYVAL;

					BL_ASSERT(arg->type->llvm_type);
					tsa_push_LLVMType(&llvm_args,
					                  LLVMPointerType(arg->type->llvm_type, 0));
				} else {
					switch (low) {
					case 1:
						arg->llvm_easgm = LLVM_EASGM_8;
						tsa_push_LLVMType(&llvm_args,
						                  LLVMInt8TypeInContext(llvm_cnt));
						break;
					case 2:
						arg->llvm_easgm = LLVM_EASGM_16;
						tsa_push_LLVMType(&llvm_args,
						                  LLVMInt16TypeInContext(llvm_cnt));
						break;
					case 4:
						arg->llvm_easgm = LLVM_EASGM_32;
						tsa_push_LLVMType(&llvm_args,
						                  LLVMInt32TypeInContext(llvm_cnt));
						break;
					case 8: {
						switch (high) {
						case 0:
							arg->llvm_easgm = LLVM_EASGM_64;
							tsa_push_LLVMType(
							    &llvm_args,
							    LLVMInt64TypeInContext(llvm_cnt));
							break;
						case 1:
							arg->llvm_easgm = LLVM_EASGM_64_8;
							tsa_push_LLVMType(
							    &llvm_args,
							    LLVMInt64TypeInContext(llvm_cnt));
							tsa_push_LLVMType(
							    &llvm_args,
							    LLVMInt8TypeInContext(llvm_cnt));
							break;
						case 2:
							arg->llvm_easgm = LLVM_EASGM_64_16;
							tsa_push_LLVMType(
							    &llvm_args,
							    LLVMInt64TypeInContext(llvm_cnt));
							tsa_push_LLVMType(
							    &llvm_args,
							    LLVMInt16TypeInContext(llvm_cnt));
							break;
						case 4:
							arg->llvm_easgm = LLVM_EASGM_64_32;
							tsa_push_LLVMType(
							    &llvm_args,
							    LLVMInt64TypeInContext(llvm_cnt));
							tsa_push_LLVMType(
							    &llvm_args,
							    LLVMInt32TypeInContext(llvm_cnt));
							break;
						case 8:
							arg->llvm_easgm = LLVM_EASGM_64_64;
							tsa_push_LLVMType(
							    &llvm_args,
							    LLVMInt64TypeInContext(llvm_cnt));
							tsa_push_LLVMType(
							    &llvm_args,
							    LLVMInt64TypeInContext(llvm_cnt));
							break;
						default:
							BL_ASSERT(false);
							break;
						}
						break;
					}
					default:
						BL_ASSERT(false);
						break;
					}
				}
			} else {
				BL_ASSERT(arg->type->llvm_type);
				tsa_push_LLVMType(&llvm_args, arg->type->llvm_type);
			}
		}
	}

	type->llvm_type =
	    LLVMFunctionType(llvm_ret, llvm_args.data, (unsigned)llvm_args.size, false);
	type->alignment         = __alignof(MirFn *);
	type->size_bits         = sizeof(MirFn *) * 8;
	type->store_size_bytes  = sizeof(MirFn *);
	type->data.fn.has_byval = has_byval;

	tsa_terminate(&llvm_args);

	/*** DI ***/
	if (!cnt->debug_mode) return;
	TSmallArray_LLVMMetadata params;
	tsa_init(&params);

	/* return type is first */
	tsa_push_LLVMMetadata(&params, type->data.fn.ret_type->llvm_meta);

	if (type->data.fn.args) {
		MirArg *it;
		TSA_FOREACH(type->data.fn.args, it)
		{
			tsa_push_LLVMMetadata(&params, it->type->llvm_meta);
		}
	}

	type->llvm_meta = llvm_di_create_function_type(
	    cnt->analyze.llvm_di_builder, params.data, (unsigned)params.size);

	tsa_terminate(&params);
}

void
init_llvm_type_array(Context *cnt, MirType *type)
{
	LLVMTypeRef llvm_elem_type = type->data.array.elem_type->llvm_type;
	BL_ASSERT(llvm_elem_type);
	const unsigned int len = (const unsigned int)type->data.array.len;

	type->llvm_type        = LLVMArrayType(llvm_elem_type, len);
	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment = (s32)LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	/*** DI ***/
	if (!cnt->debug_mode) return;
	type->llvm_meta = llvm_di_create_array_type(cnt->analyze.llvm_di_builder,
	                                            type->size_bits,
	                                            (unsigned)type->alignment * 8,
	                                            type->data.array.elem_type->llvm_meta,
	                                            (unsigned)type->data.array.len);
}

void
init_llvm_type_struct(Context *cnt, MirType *type)
{
	if (type->data.strct.is_incomplete) {
		BL_ASSERT(type->user_id && "Missing user id for incomplete struct type.");
		type->llvm_type =
		    LLVMStructCreateNamed(cnt->assembly->llvm.cnt, type->user_id->str);
		return;
	}

	TSmallArray_MemberPtr *members = type->data.strct.members;
	BL_ASSERT(members);

	const bool  is_packed = type->data.strct.is_packed;
	const usize memc      = members->size;
	BL_ASSERT(memc > 0);
	TSmallArray_LLVMType llvm_members;
	tsa_init(&llvm_members);

	MirMember *member;
	TSA_FOREACH(members, member)
	{
		BL_ASSERT(member->type->llvm_type);
		tsa_push_LLVMType(&llvm_members, member->type->llvm_type);
	}

	/* named structure type */
	if (type->user_id) {
		if (type->llvm_type == NULL) {
			/* Create new named type only if it's not already created (by incomplete
			 * type declaration). */
			type->llvm_type =
			    LLVMStructCreateNamed(cnt->assembly->llvm.cnt, type->user_id->str);
		}

		LLVMStructSetBody(type->llvm_type, llvm_members.data, (unsigned)memc, is_packed);
	} else {
		type->llvm_type = LLVMStructTypeInContext(
		    cnt->assembly->llvm.cnt, llvm_members.data, (unsigned)memc, is_packed);
	}

	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment = (s32)LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	tsa_terminate(&llvm_members);

	/* set offsets for members */
	TSA_FOREACH(members, member)
	member->offset_bytes = (s32)mir_get_struct_elem_offest(cnt->assembly, type, (u32)i);

	/*** DI ***/
	if (!cnt->debug_mode) return;

	BL_ASSERT(type->data.strct.scope);
	if (!type->data.strct.scope->llvm_di_meta) init_llvm_DI_scope(cnt, type->data.strct.scope);

	const bool      is_implicit = !type->data.strct.scope->location;
	LLVMMetadataRef llvm_file;
	unsigned        struct_line;

	if (is_implicit) {
		struct_line = 0;
		llvm_file   = cnt->assembly->gscope->llvm_di_meta;
	} else {
		Location *location = type->data.strct.scope->location;
		llvm_file          = location->unit->llvm_file_meta;
		struct_line        = (unsigned)location->line;
	}

	LLVMMetadataRef llvm_scope  = type->data.strct.scope->llvm_di_meta;
	const char *    struct_name = "<implicit_struct>";
	if (type->user_id) {
		struct_name = type->user_id->str;
	} else {
		/* NOTE: string has buildin ID */
		switch (type->kind) {
		case MIR_TYPE_STRUCT: {
			struct_name = "struct";
			break;
		}

		case MIR_TYPE_SLICE: {
			struct_name = "slice";
			break;
		}

		case MIR_TYPE_VARGS: {
			struct_name = "vargs";
			break;
		}

		default:
			/* use default implicit name */
			break;
		}
	}

	TSmallArray_LLVMMetadata llvm_elems;
	tsa_init(&llvm_elems);

	MirMember *elem;
	TSA_FOREACH(type->data.strct.members, elem)
	{
		unsigned elem_line =
		    elem->decl_node ? (unsigned)elem->decl_node->location->line : 0;
		LLVMMetadataRef llvm_elem = llvm_di_create_member_type(
		    cnt->analyze.llvm_di_builder,
		    llvm_scope,
		    elem->id->str,
		    llvm_file,
		    elem_line,
		    elem->type->size_bits,
		    (unsigned)elem->type->alignment * 8,
		    (unsigned)mir_get_struct_elem_offest(cnt->assembly, type, (u32)i) * 8,
		    elem->type->llvm_meta);

		tsa_push_LLVMMetadata(&llvm_elems, llvm_elem);
	}

	LLVMMetadataRef llvm_struct =
	    llvm_di_create_struct_type(cnt->analyze.llvm_di_builder,
	                               type->data.strct.scope->parent->llvm_di_meta,
	                               struct_name,
	                               llvm_file,
	                               struct_line,
	                               type->size_bits,
	                               (unsigned)type->alignment * 8,
	                               llvm_elems.data,
	                               llvm_elems.size);

	type->llvm_meta = llvm_di_replace_temporary(
	    cnt->analyze.llvm_di_builder, type->data.strct.scope->llvm_di_meta, llvm_struct);

	tsa_terminate(&llvm_elems);
	return;
}

void
init_llvm_type_enum(Context *cnt, MirType *type)
{
	MirType *base_type = type->data.enm.base_type;
	BL_ASSERT(base_type->kind == MIR_TYPE_INT);
	LLVMTypeRef llvm_base_type = base_type->llvm_type;
	BL_ASSERT(llvm_base_type);

	type->llvm_type        = llvm_base_type;
	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment = (s32)LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	/*** DI ***/
	if (!cnt->debug_mode) return;
	const char *name = type->user_id ? type->user_id->str : "enum";

	TSmallArray_LLVMMetadata llvm_elems;
	tsa_init(&llvm_elems);

	MirVariant *variant;
	TSA_FOREACH(type->data.enm.variants, variant)
	{
		LLVMMetadataRef llvm_variant =
		    llvm_di_create_enum_variant(cnt->analyze.llvm_di_builder,
		                                variant->id->str,
		                                variant->value->data.v_u64,
		                                !base_type->data.integer.is_signed);

		tsa_push_LLVMMetadata(&llvm_elems, llvm_variant);
	}

	LLVMMetadataRef llvm_type =
	    llvm_di_create_enum_type(cnt->analyze.llvm_di_builder,
	                             type->data.enm.scope->parent->llvm_di_meta,
	                             name,
	                             type->data.enm.scope->location->unit->llvm_file_meta,
	                             (unsigned)type->data.enm.scope->location->line,
	                             type->size_bits,
	                             (unsigned)type->alignment * 8,
	                             llvm_elems.data,
	                             llvm_elems.size,
	                             base_type->llvm_meta);

	type->llvm_meta = llvm_di_replace_temporary(
	    cnt->analyze.llvm_di_builder, type->data.enm.scope->llvm_di_meta, llvm_type);
	tsa_terminate(&llvm_elems);
}

void
init_llvm_DI_scope(Context *cnt, Scope *scope)
{
	switch (scope->kind) {
	case SCOPE_LEXICAL: {
		BL_ASSERT(scope->location);
		LLVMMetadataRef llvm_parent_scope = scope->parent->llvm_di_meta;
		LLVMMetadataRef llvm_unit         = scope->location->unit->llvm_file_meta;

		BL_ASSERT(llvm_parent_scope);
		BL_ASSERT(llvm_unit);

		scope->llvm_di_meta = llvm_di_create_lexical_scope(cnt->analyze.llvm_di_builder,
		                                                   llvm_parent_scope,
		                                                   llvm_unit,
		                                                   (unsigned)scope->location->line,
		                                                   (unsigned)scope->location->col);
		break;
	}

	case SCOPE_FN: {
		scope->llvm_di_meta = llvm_di_create_fn_fwd_decl(
		    cnt->analyze.llvm_di_builder, NULL, "", "", NULL, 0, NULL, 0);
		break;
	}

	case SCOPE_TYPE_STRUCT: {
		scope->llvm_di_meta = llvm_di_create_replecable_composite_type(
		    cnt->analyze.llvm_di_builder, DW_TAG_structure_type, "", NULL, NULL, 0);
		break;
	}

	case SCOPE_TYPE_ENUM: {
		scope->llvm_di_meta = llvm_di_create_replecable_composite_type(
		    cnt->analyze.llvm_di_builder, DW_TAG_enumeration_type, "", NULL, NULL, 0);
		break;
	}

	default:
		BL_ABORT("unsuported scope type for DI generation");
	}
}

static inline void
push_var(Context *cnt, MirVar *var)
{
	BL_ASSERT(var);

	if (var->is_in_gscope) return;

	MirFn *fn = get_current_fn(cnt);
	BL_ASSERT(fn);
	tarray_push(fn->variables, var);
}

MirVar *
create_var(Context *cnt,
           Ast *    decl_node,
           Scope *  scope,
           ID *     id,
           MirType *alloc_type,
           bool     is_mutable,
           bool     is_in_gscope,
           u32      flags)
{
	BL_ASSERT(id);
	MirVar *tmp       = arena_alloc(&cnt->assembly->arenas.mir.var);
	tmp->id           = id;
	tmp->value.type   = alloc_type;
	tmp->decl_scope   = scope;
	tmp->decl_node    = decl_node;
	tmp->is_mutable   = is_mutable;
	tmp->is_in_gscope = is_in_gscope;
	tmp->llvm_name    = id->str;
	tmp->flags        = flags;
	tmp->gen_llvm     = true;

	push_var(cnt, tmp);
	return tmp;
}

MirVar *
create_var_impl(Context *   cnt,
                const char *name,
                MirType *   alloc_type,
                bool        is_mutable,
                bool        is_in_gscope,
                bool        comptime)
{
	BL_ASSERT(name);
	MirVar *tmp     = arena_alloc(&cnt->assembly->arenas.mir.var);
	tmp->value.type = alloc_type;
	tmp->value.eval_mode =
	    comptime ? choose_eval_mode_for_comptime(alloc_type) : MIR_VEM_RUNTIME;
	tmp->is_mutable   = is_mutable;
	tmp->is_in_gscope = is_in_gscope;
	tmp->llvm_name    = name;
	tmp->is_implicit  = true;
	tmp->gen_llvm     = true;

	push_var(cnt, tmp);
	return tmp;
}

MirFn *
create_fn(Context *        cnt,
          Ast *            node,
          ID *             id,
          const char *     linkage_name,
          u32              flags,
          MirInstrFnProto *prototype,
          bool             emit_llvm,
          bool             is_in_gscope)
{
	MirFn *tmp        = arena_alloc(&cnt->assembly->arenas.mir.fn);
	tmp->variables    = create_arr(cnt->assembly, sizeof(MirVar *));
	tmp->linkage_name = linkage_name;
	tmp->id           = id;
	tmp->flags        = flags;
	tmp->decl_node    = node;
	tmp->prototype    = &prototype->base;
	tmp->emit_llvm    = emit_llvm;
	tmp->is_in_gscope = is_in_gscope;
	return tmp;
}

MirMember *
create_member(Context *cnt, Ast *node, ID *id, Scope *scope, s64 index, MirType *type)
{
	MirMember *tmp  = arena_alloc(&cnt->assembly->arenas.mir.member);
	tmp->decl_node  = node;
	tmp->id         = id;
	tmp->index      = index;
	tmp->type       = type;
	tmp->decl_scope = scope;
	return tmp;
}

MirArg *
create_arg(Context *cnt, Ast *node, ID *id, Scope *scope, MirType *type)
{
	MirArg *tmp     = arena_alloc(&cnt->assembly->arenas.mir.arg);
	tmp->decl_node  = node;
	tmp->id         = id;
	tmp->type       = type;
	tmp->decl_scope = scope;
	return tmp;
}

MirVariant *
create_variant(Context *cnt, ID *id, Scope *scope, MirConstValue *value)
{
	MirVariant *tmp = arena_alloc(&cnt->assembly->arenas.mir.variant);
	tmp->id         = id;
	tmp->decl_scope = scope;
	tmp->value      = value;
	return tmp;
}

MirConstValue *
create_const_value(Context *cnt, MirType *type)
{
	MirConstValue *tmp = arena_alloc(&cnt->assembly->arenas.mir.value);
	tmp->type          = type;
	tmp->addr_mode     = MIR_VAM_LVALUE_CONST;
	tmp->eval_mode     = choose_eval_mode_for_comptime(type);

	return tmp;
}

MirConstValue *
init_or_create_const_integer(Context *cnt, MirConstValue *v, MirType *type, u64 i)
{
	if (!v) v = arena_alloc(&cnt->assembly->arenas.mir.value);
	v->type       = type;
	v->addr_mode  = MIR_VAM_RVALUE;
	v->eval_mode  = MIR_VEM_STATIC;
	v->data.v_u64 = i;

	return v;
}

MirConstValue *
init_or_create_const_null(Context *cnt, MirConstValue *v, MirType *type)
{
	if (!v) v = arena_alloc(&cnt->assembly->arenas.mir.value);
	v->type                = type;
	v->addr_mode           = MIR_VAM_RVALUE;
	v->eval_mode           = MIR_VEM_STATIC;
	v->data.v_ptr.data.any = NULL;

	return v;
}

MirConstValue *
init_or_create_const_bool(Context *cnt, MirConstValue *v, bool b)
{
	if (!v) v = arena_alloc(&cnt->assembly->arenas.mir.value);
	v->type        = cnt->builtin_types.t_bool;
	v->addr_mode   = MIR_VAM_RVALUE;
	v->eval_mode   = MIR_VEM_STATIC;
	v->data.v_bool = b;

	return v;
}

MirConstValue *
init_or_create_const_var_ptr(Context *cnt, MirConstValue *v, MirType *type, MirVar *var)
{
	if (!v) v = arena_alloc(&cnt->assembly->arenas.mir.value);
	v->type      = type;
	v->addr_mode = MIR_VAM_LVALUE_CONST;
	v->eval_mode = MIR_VEM_STATIC;

	mir_set_const_ptr(&v->data.v_ptr, var, MIR_CP_VAR);
	return v;
}

MirConstValue *
init_or_create_const_array(Context *                  cnt,
                           MirConstValue *            v,
                           MirType *                  elem_type,
                           TSmallArray_ConstValuePtr *elems)
{
	if (!v) v = arena_alloc(&cnt->assembly->arenas.mir.value);
	v->type               = create_type_array(cnt, elem_type, (s64)elems->size);
	v->addr_mode          = MIR_VAM_LVALUE_CONST;
	v->eval_mode          = MIR_VEM_LAZY;
	v->data.v_array.elems = elems;

	return v;
}

MirConstValue *
init_or_create_const_struct(Context *                  cnt,
                            MirConstValue *            v,
                            MirType *                  type,
                            TSmallArray_ConstValuePtr *members)
{
	if (!v) v = arena_alloc(&cnt->assembly->arenas.mir.value);
	v->type                  = type;
	v->addr_mode             = MIR_VAM_LVALUE_CONST;
	v->eval_mode             = MIR_VEM_LAZY;
	v->data.v_struct.members = members;

	return v;
}

MirConstValue *
init_or_create_const_string(Context *cnt, MirConstValue *v, const char *str)
{
	if (!v) v = arena_alloc(&cnt->assembly->arenas.mir.value);
	v->type      = cnt->builtin_types.t_string;
	v->addr_mode = MIR_VAM_RVALUE;
	v->eval_mode = MIR_VEM_LAZY;

	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	/* .len */
	tsa_push_ConstValuePtr(
	    m, init_or_create_const_integer(cnt, NULL, cnt->builtin_types.t_s64, strlen(str)));

	/* .ptr */
	MirConstValue *ptr = create_const_value(
	    cnt, mir_get_struct_elem_type(cnt->builtin_types.t_string, MIR_SLICE_PTR_INDEX));

	MirConstPtr *const_ptr = &ptr->data.v_ptr;
	mir_set_const_ptr(const_ptr, (void *)str, MIR_CP_STR);
	tsa_push_ConstValuePtr(m, ptr);

	v->data.v_struct.members = m;
	return v;
}

/* instructions */
void
maybe_mark_as_unrechable(MirInstrBlock *block, MirInstr *instr)
{
	if (!is_block_terminated(block)) return;
	instr->unrechable         = true;
	MirFn *          fn       = block->owner_fn;
	MirInstrFnProto *fn_proto = (MirInstrFnProto *)fn->prototype;
	if (!fn_proto->first_unrechable_location && instr->node)
		fn_proto->first_unrechable_location = instr->node->location;
}

void
append_current_block(Context *cnt, MirInstr *instr)
{
	BL_ASSERT(instr);
	MirInstrBlock *block = get_current_block(cnt);
	BL_ASSERT(block);

	maybe_mark_as_unrechable(block, instr);

	instr->owner_block = block;
	instr->prev        = block->last_instr;

	if (!block->entry_instr) block->entry_instr = instr;
	if (instr->prev) instr->prev->next = instr;
	block->last_instr = instr;
}

MirInstr *
insert_instr_cast(Context *cnt, MirInstr *src, MirType *to_type)
{
	MirInstrCast *tmp    = CREATE_INSTR(cnt, MIR_INSTR_CAST, src->node, MirInstrCast *);
	tmp->base.value.type = to_type;
	tmp->base.implicit   = true;
	tmp->expr            = src;
	ref_instr(&tmp->base);

	insert_instr_after(src, &tmp->base);
	analyze_instr_rq(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
insert_instr_addrof(Context *cnt, MirInstr *src)
{
	MirInstr *tmp = create_instr_addrof(cnt, src->node, src);
	tmp->implicit = true;

	insert_instr_after(src, tmp);
	analyze_instr_rq(cnt, tmp);
	return tmp;
}

MirInstr *
insert_instr_toany(Context *cnt, MirInstr *expr)
{
	MirInstrToAny *tmp   = CREATE_INSTR(cnt, MIR_INSTR_TOANY, expr->node, MirInstrToAny *);
	tmp->base.value.type = create_type_ptr(cnt, lookup_builtin(cnt, MIR_BUILTIN_ID_ANY));
	tmp->base.implicit   = true;
	tmp->expr            = expr;
	ref_instr(&tmp->base);

	insert_instr_after(expr, &tmp->base);
	analyze_instr_rq(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
insert_instr_load(Context *cnt, MirInstr *src)
{
	BL_ASSERT(src);
	BL_ASSERT(src->value.type);
	BL_ASSERT(src->value.type->kind == MIR_TYPE_PTR);
	MirInstrLoad *tmp  = CREATE_INSTR(cnt, MIR_INSTR_LOAD, src->node, MirInstrLoad *);
	tmp->base.implicit = true;
	tmp->src           = src;

	ref_instr(&tmp->base);
	insert_instr_after(src, &tmp->base);
	analyze_instr_rq(cnt, &tmp->base);

	return &tmp->base;
}

MirCastOp
get_cast_op(MirType *from, MirType *to)
{
	BL_ASSERT(from);
	BL_ASSERT(to);
	const usize fsize = from->size_bits;
	const usize tsize = to->size_bits;

	if (type_cmp(from, to)) return MIR_CAST_NONE;

	switch (from->kind) {
	case MIR_TYPE_ENUM:
	case MIR_TYPE_INT: {
		/* from integer */
		switch (to->kind) {
		case MIR_TYPE_INT: {
			/* to integer */
			const bool is_to_signed = to->data.integer.is_signed;
			if (fsize < tsize) {
				return is_to_signed ? MIR_CAST_SEXT : MIR_CAST_ZEXT;
			} else {
				return MIR_CAST_TRUNC;
			}
		}

		case MIR_TYPE_REAL: {
			const bool is_from_signed = from->data.integer.is_signed;
			return is_from_signed ? MIR_CAST_SITOFP : MIR_CAST_UITOFP;
		}

		case MIR_TYPE_PTR: {
			/* to ptr */
			return MIR_CAST_INTTOPTR;
		}

		default:
			return MIR_CAST_INVALID;
		}
	}

	case MIR_TYPE_PTR: {
		/* from pointer */
		switch (to->kind) {
		case MIR_TYPE_PTR: {
			/* to pointer */
			return MIR_CAST_BITCAST;
		}

		case MIR_TYPE_INT: {
			/* to int */
			return MIR_CAST_PTRTOINT;
		}

		default:
			return MIR_CAST_INVALID;
		}
	}

	case MIR_TYPE_REAL: {
		/* from real */
		switch (to->kind) {
		case MIR_TYPE_INT: {
			/* to integer */
			const bool is_to_signed = to->data.integer.is_signed;
			return is_to_signed ? MIR_CAST_FPTOSI : MIR_CAST_FPTOUI;
		}

		case MIR_TYPE_REAL: {
			/* to integer */
			if (fsize < tsize) {
				return MIR_CAST_FPEXT;
			} else {
				return MIR_CAST_FPTRUNC;
			}
		}

		default:
			return MIR_CAST_INVALID;
		}
	}

	default:
		return MIR_CAST_INVALID;
	}
}

static u64 _id_counter = 0;

MirInstr *
_create_instr(Context *cnt, MirInstrKind kind, Ast *node)
{
	MirInstr *tmp = arena_alloc(&cnt->assembly->arenas.mir.instr);
	tmp->kind     = kind;
	tmp->node     = node;
	tmp->id       = _id_counter++;

	return tmp;
}

MirInstrBlock *
append_block(Context *cnt, MirFn *fn, const char *name)
{
	BL_ASSERT(fn && name);
	MirInstrBlock *tmp = CREATE_INSTR(cnt, MIR_INSTR_BLOCK, NULL, MirInstrBlock *);
	tmp->name          = name;
	tmp->owner_fn      = fn;

	if (!fn->first_block) {
		fn->first_block = tmp;

		/* first block is referenced everytime!!! */
		ref_instr(&tmp->base);
	}

	tmp->base.prev = &fn->last_block->base;
	if (fn->last_block) fn->last_block->base.next = &tmp->base;
	fn->last_block = tmp;

	return tmp;
}

MirInstr *
create_instr_call_comptime(Context *cnt, Ast *node, MirInstr *fn)
{
	BL_ASSERT(fn && fn->kind == MIR_INSTR_FN_PROTO);
	MirInstrCall *tmp = CREATE_INSTR(cnt, MIR_INSTR_CALL, node, MirInstrCall *);

	/* Comptime call has by default static evaluation mode. This could be cahnged during analyze
	 * pass. */
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->base.ref_count       = 2;
	tmp->callee               = fn;

	ref_instr(fn);
	return &tmp->base;
}

MirInstr *
append_instr_type_fn(Context *cnt, Ast *node, MirInstr *ret_type, TSmallArray_InstrPtr *args)
{
	MirInstrTypeFn *tmp       = CREATE_INSTR(cnt, MIR_INSTR_TYPE_FN, node, MirInstrTypeFn *);
	tmp->base.value.type      = cnt->builtin_types.t_type;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->ret_type             = ret_type;
	tmp->args                 = args;

	if (args) {
		MirInstr *it;
		TSA_FOREACH(args, it)
		{
			ref_instr(it);
		}
	}

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_type_struct(Context *             cnt,
                         Ast *                 node,
                         ID *                  id,
                         MirInstr *            fwd_decl,
                         Scope *               scope,
                         TSmallArray_InstrPtr *members,
                         bool                  is_packed)
{
	MirInstrTypeStruct *tmp =
	    CREATE_INSTR(cnt, MIR_INSTR_TYPE_STRUCT, node, MirInstrTypeStruct *);
	tmp->base.value.type      = cnt->builtin_types.t_type;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->members              = members;
	tmp->scope                = scope;
	tmp->is_packed            = is_packed;
	tmp->id                   = id;
	tmp->fwd_decl             = fwd_decl;

	if (members) {
		MirInstr *it;
		TSA_FOREACH(members, it)
		{
			ref_instr(it);
		}
	}

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_type_enum(Context *             cnt,
                       Ast *                 node,
                       ID *                  id,
                       Scope *               scope,
                       TSmallArray_InstrPtr *variants,
                       MirInstr *            base_type)
{
	MirInstrTypeEnum *tmp = CREATE_INSTR(cnt, MIR_INSTR_TYPE_ENUM, node, MirInstrTypeEnum *);
	tmp->base.value.type  = cnt->builtin_types.t_type;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->variants             = variants;
	tmp->scope                = scope;
	tmp->id                   = id;
	tmp->base_type            = base_type;

	if (variants) {
		MirInstr *it;
		TSA_FOREACH(variants, it)
		{
			ref_instr(it);
		}
	}

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_type_ptr(Context *cnt, Ast *node, MirInstr *type)
{
	MirInstrTypePtr *tmp      = CREATE_INSTR(cnt, MIR_INSTR_TYPE_PTR, node, MirInstrTypePtr *);
	tmp->base.value.type      = cnt->builtin_types.t_type;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->base.value.addr_mode = MIR_VAM_LVALUE_CONST;
	tmp->type                 = type;

	ref_instr(type);
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_type_array(Context *cnt, Ast *node, MirInstr *elem_type, MirInstr *len)
{
	MirInstrTypeArray *tmp = CREATE_INSTR(cnt, MIR_INSTR_TYPE_ARRAY, node, MirInstrTypeArray *);
	tmp->base.value.type   = cnt->builtin_types.t_type;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->elem_type            = elem_type;
	tmp->len                  = len;

	ref_instr(elem_type);
	ref_instr(len);
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_type_slice(Context *cnt, Ast *node, MirInstr *elem_type)
{
	MirInstrTypeSlice *tmp = CREATE_INSTR(cnt, MIR_INSTR_TYPE_SLICE, node, MirInstrTypeSlice *);
	tmp->base.value.type   = cnt->builtin_types.t_type;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->elem_type            = elem_type;

	ref_instr(elem_type);
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_type_vargs(Context *cnt, Ast *node, MirInstr *elem_type)
{
	MirInstrTypeVArgs *tmp = CREATE_INSTR(cnt, MIR_INSTR_TYPE_VARGS, node, MirInstrTypeVArgs *);
	tmp->base.value.type   = cnt->builtin_types.t_type;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->elem_type            = elem_type;

	ref_instr(elem_type);
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_arg(Context *cnt, Ast *node, unsigned i)
{
	MirInstrArg *tmp          = CREATE_INSTR(cnt, MIR_INSTR_ARG, node, MirInstrArg *);
	tmp->base.value.eval_mode = MIR_VEM_RUNTIME;
	tmp->i                    = i;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_phi(Context *cnt, Ast *node)
{
	MirInstrPhi *tmp     = CREATE_INSTR(cnt, MIR_INSTR_PHI, node, MirInstrPhi *);
	tmp->incoming_values = create_sarr(TSmallArray_InstrPtr, cnt->assembly);
	tmp->incoming_blocks = create_sarr(TSmallArray_InstrPtr, cnt->assembly);
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_compound(Context *cnt, Ast *node, MirInstr *type, TSmallArray_InstrPtr *values)
{
	if (values) {
		MirInstr *value;
		TSA_FOREACH(values, value) ref_instr(value);
	}
	ref_instr(type);

	MirInstrCompound *tmp = CREATE_INSTR(cnt, MIR_INSTR_COMPOUND, node, MirInstrCompound *);
	tmp->type             = type;
	tmp->values           = values;
	tmp->is_naked         = true;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_cast(Context *cnt, Ast *node, MirInstr *type, MirInstr *next)
{
	ref_instr(type);
	ref_instr(next);
	MirInstrCast *tmp = CREATE_INSTR(cnt, MIR_INSTR_CAST, node, MirInstrCast *);
	tmp->type         = type;
	tmp->expr         = next;
	tmp->auto_cast    = type == NULL;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_sizeof(Context *cnt, Ast *node, MirInstr *expr)
{
	ref_instr(expr);
	MirInstrSizeof *tmp       = CREATE_INSTR(cnt, MIR_INSTR_SIZEOF, node, MirInstrSizeof *);
	tmp->base.value.type      = cnt->builtin_types.t_usize;
	tmp->base.value.addr_mode = MIR_VAM_RVALUE;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->expr                 = expr;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
create_instr_type_info(Context *cnt, Ast *node, MirInstr *expr)
{
	ref_instr(expr);
	MirInstrTypeInfo *tmp = CREATE_INSTR(cnt, MIR_INSTR_TYPE_INFO, node, MirInstrTypeInfo *);
	tmp->base.value.addr_mode = MIR_VAM_RVALUE;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->expr                 = expr;
	return &tmp->base;
}

MirInstr *
append_instr_type_info(Context *cnt, Ast *node, MirInstr *expr)
{
	MirInstr *tmp = create_instr_type_info(cnt, node, expr);
	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_alignof(Context *cnt, Ast *node, MirInstr *expr)
{
	ref_instr(expr);
	MirInstrAlignof *tmp      = CREATE_INSTR(cnt, MIR_INSTR_ALIGNOF, node, MirInstrAlignof *);
	tmp->base.value.type      = cnt->builtin_types.t_usize;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->base.value.addr_mode = MIR_VAM_RVALUE;
	tmp->expr                 = expr;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_cond_br(Context *      cnt,
                     Ast *          node,
                     MirInstr *     cond,
                     MirInstrBlock *then_block,
                     MirInstrBlock *else_block)
{
	BL_ASSERT(cond && then_block && else_block);
	ref_instr(cond);
	ref_instr(&then_block->base);
	ref_instr(&else_block->base);
	MirInstrCondBr *tmp       = CREATE_INSTR(cnt, MIR_INSTR_COND_BR, node, MirInstrCondBr *);
	tmp->base.ref_count       = NO_REF_COUNTING;
	tmp->base.value.type      = cnt->builtin_types.t_void;
	tmp->base.value.eval_mode = MIR_VEM_NONE;
	tmp->cond                 = cond;
	tmp->then_block           = then_block;
	tmp->else_block           = else_block;

	MirInstrBlock *block = get_current_block(cnt);

	append_current_block(cnt, &tmp->base);
	if (!is_block_terminated(block)) terminate_block(block, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_br(Context *cnt, Ast *node, MirInstrBlock *then_block)
{
	BL_ASSERT(then_block);
	ref_instr(&then_block->base);
	MirInstrBr *tmp           = CREATE_INSTR(cnt, MIR_INSTR_BR, node, MirInstrBr *);
	tmp->base.ref_count       = NO_REF_COUNTING;
	tmp->base.value.type      = cnt->builtin_types.t_void;
	tmp->base.value.eval_mode = MIR_VEM_NONE;
	tmp->then_block           = then_block;

	MirInstrBlock *block = get_current_block(cnt);

	append_current_block(cnt, &tmp->base);
	if (!is_block_terminated(block)) terminate_block(block, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_switch(Context *               cnt,
                    Ast *                   node,
                    MirInstr *              value,
                    MirInstrBlock *         default_block,
                    bool                    user_defined_default,
                    TSmallArray_SwitchCase *cases)
{
	BL_ASSERT(default_block);
	BL_ASSERT(cases);
	BL_ASSERT(value);

	ref_instr(&default_block->base);
	ref_instr(value);

	for (usize i = 0; i < cases->size; ++i) {
		MirSwitchCase *c = &cases->data[i];
		ref_instr(&c->block->base);
		ref_instr(c->on_value);
	}

	MirInstrSwitch *tmp           = CREATE_INSTR(cnt, MIR_INSTR_SWITCH, node, MirInstrSwitch *);
	tmp->base.ref_count           = NO_REF_COUNTING;
	tmp->base.value.eval_mode     = MIR_VEM_NONE;
	tmp->base.value.type          = cnt->builtin_types.t_void;
	tmp->value                    = value;
	tmp->default_block            = default_block;
	tmp->cases                    = cases;
	tmp->has_user_defined_default = user_defined_default;

	MirInstrBlock *block = get_current_block(cnt);

	append_current_block(cnt, &tmp->base);
	if (!is_block_terminated(block)) terminate_block(block, &tmp->base);
	return &tmp->base;
}

MirInstr *
create_instr_elem_ptr(Context * cnt,
                      Ast *     node,
                      MirInstr *arr_ptr,
                      MirInstr *index,
                      bool      target_is_slice)
{
	BL_ASSERT(arr_ptr && index);
	ref_instr(arr_ptr);
	ref_instr(index);
	MirInstrElemPtr *tmp = CREATE_INSTR(cnt, MIR_INSTR_ELEM_PTR, node, MirInstrElemPtr *);
	tmp->arr_ptr         = arr_ptr;
	tmp->index           = index;
	tmp->target_is_slice = target_is_slice;

	return &tmp->base;
}

MirInstr *
append_instr_elem_ptr(Context * cnt,
                      Ast *     node,
                      MirInstr *arr_ptr,
                      MirInstr *index,
                      bool      target_is_slice)
{
	MirInstr *tmp = create_instr_elem_ptr(cnt, node, arr_ptr, index, target_is_slice);
	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
create_instr_member_ptr(Context *        cnt,
                        Ast *            node,
                        MirInstr *       target_ptr,
                        Ast *            member_ident,
                        ScopeEntry *     scope_entry,
                        MirBuiltinIdKind builtin_id)
{
	ref_instr(target_ptr);
	MirInstrMemberPtr *tmp = CREATE_INSTR(cnt, MIR_INSTR_MEMBER_PTR, node, MirInstrMemberPtr *);
	tmp->target_ptr        = target_ptr;
	tmp->member_ident      = member_ident;
	tmp->scope_entry       = scope_entry;
	tmp->builtin_id        = builtin_id;

	return &tmp->base;
}

MirInstr *
append_instr_member_ptr(Context *        cnt,
                        Ast *            node,
                        MirInstr *       target_ptr,
                        Ast *            member_ident,
                        ScopeEntry *     scope_entry,
                        MirBuiltinIdKind builtin_id)
{
	MirInstr *tmp =
	    create_instr_member_ptr(cnt, node, target_ptr, member_ident, scope_entry, builtin_id);

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_load(Context *cnt, Ast *node, MirInstr *src)
{
	ref_instr(src);
	MirInstrLoad *tmp = CREATE_INSTR(cnt, MIR_INSTR_LOAD, node, MirInstrLoad *);
	tmp->src          = src;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
create_instr_addrof(Context *cnt, Ast *node, MirInstr *src)
{
	ref_instr(src);
	MirInstrAddrof *tmp = CREATE_INSTR(cnt, MIR_INSTR_ADDROF, node, MirInstrAddrof *);
	tmp->src            = src;
	return &tmp->base;
}

MirInstr *
append_instr_addrof(Context *cnt, Ast *node, MirInstr *src)
{
	MirInstr *tmp = create_instr_addrof(cnt, node, src);
	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_unrecheable(Context *cnt, Ast *node)
{
	MirInstrUnreachable *tmp =
	    CREATE_INSTR(cnt, MIR_INSTR_UNREACHABLE, node, MirInstrUnreachable *);
	tmp->base.value.type = cnt->builtin_types.t_void;
	tmp->base.ref_count  = NO_REF_COUNTING;
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_fn_proto(Context * cnt,
                      Ast *     node,
                      MirInstr *type,
                      MirInstr *user_type,
                      bool      schedule_analyze)
{
	MirInstrFnProto *tmp      = CREATE_INSTR(cnt, MIR_INSTR_FN_PROTO, node, MirInstrFnProto *);
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->base.ref_count       = NO_REF_COUNTING;
	tmp->type                 = type;
	tmp->user_type            = user_type;
	tmp->pushed_for_analyze   = schedule_analyze;

	push_into_gscope(cnt, &tmp->base);

	if (schedule_analyze) analyze_push_back(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_decl_ref(Context *   cnt,
                      Ast *       node,
                      Unit *      parent_unit,
                      ID *        rid,
                      Scope *     scope,
                      ScopeEntry *scope_entry)
{
	BL_ASSERT(scope && rid);
	MirInstrDeclRef *tmp = CREATE_INSTR(cnt, MIR_INSTR_DECL_REF, node, MirInstrDeclRef *);
	tmp->scope_entry     = scope_entry;
	tmp->scope           = scope;
	tmp->rid             = rid;
	tmp->parent_unit     = parent_unit;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_decl_direct_ref(Context *cnt, MirInstr *ref)
{
	BL_ASSERT(ref);
	ref_instr(ref);
	MirInstrDeclDirectRef *tmp =
	    CREATE_INSTR(cnt, MIR_INSTR_DECL_DIRECT_REF, NULL, MirInstrDeclDirectRef *);
	tmp->ref = ref;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_call(Context *cnt, Ast *node, MirInstr *callee, TSmallArray_InstrPtr *args)
{
	BL_ASSERT(callee);
	MirInstrCall *tmp         = CREATE_INSTR(cnt, MIR_INSTR_CALL, node, MirInstrCall *);
	tmp->args                 = args;
	tmp->callee               = callee;
	tmp->base.value.addr_mode = MIR_VAM_RVALUE;
	/* call have by default runtime evaluated value */
	tmp->base.value.eval_mode = MIR_VEM_RUNTIME;

	ref_instr(&tmp->base);

	/* Callee must be referenced even if we call no-ref counted fn_proto instructions, because
	 * sometimes callee is declaration reference poining to variable containing pointer to some
	 * function. */
	ref_instr(callee);

	/* reference all arguments */
	if (args) {
		MirInstr *instr;
		TSA_FOREACH(args, instr) ref_instr(instr);
	}

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_decl_var(Context * cnt,
                      Ast *     node,
                      MirInstr *type,
                      MirInstr *init,
                      bool      is_mutable,
                      bool      is_in_gscope,
                      s32       order,
                      u32       flags)
{
	ref_instr(type);
	ref_instr(init);
	MirInstrDeclVar *tmp      = CREATE_INSTR(cnt, MIR_INSTR_DECL_VAR, node, MirInstrDeclVar *);
	tmp->base.value.type      = cnt->builtin_types.t_void;
	tmp->base.value.eval_mode = MIR_VEM_NONE;
	tmp->base.ref_count       = NO_REF_COUNTING;
	tmp->type                 = type;
	tmp->init                 = init;

	tmp->var = create_var(cnt,
	                      node,
	                      node->owner_scope,
	                      &node->data.ident.id,
	                      NULL,
	                      is_mutable,
	                      is_in_gscope,
	                      flags);

	if (is_in_gscope) {
		push_into_gscope(cnt, &tmp->base);
		analyze_push_back(cnt, &tmp->base);
	} else {
		append_current_block(cnt, &tmp->base);
	}

	if (init && init->kind == MIR_INSTR_COMPOUND) {
		((MirInstrCompound *)init)->is_naked = false;
	}

	return &tmp->base;
}

MirInstr *
append_instr_decl_var_impl(Context *   cnt,
                           const char *name,
                           MirInstr *  type,
                           MirInstr *  init,
                           bool        is_mutable,
                           bool        is_in_gscope,
                           s32         order,
                           u32         flags)
{
	ref_instr(type);
	ref_instr(init);
	MirInstrDeclVar *tmp      = CREATE_INSTR(cnt, MIR_INSTR_DECL_VAR, NULL, MirInstrDeclVar *);
	tmp->base.value.eval_mode = MIR_VEM_NONE;
	tmp->base.value.type      = cnt->builtin_types.t_void;
	tmp->base.ref_count       = NO_REF_COUNTING;
	tmp->type                 = type;
	tmp->init                 = init;

	tmp->var = create_var_impl(cnt, name, NULL, is_mutable, is_in_gscope, false);

	if (is_in_gscope) {
		push_into_gscope(cnt, &tmp->base);
		analyze_push_back(cnt, &tmp->base);
	} else {
		append_current_block(cnt, &tmp->base);
	}

	if (init && init->kind == MIR_INSTR_COMPOUND) {
		((MirInstrCompound *)init)->is_naked = false;
	}

	return &tmp->base;
}

MirInstr *
append_instr_decl_member(Context *cnt, Ast *node, MirInstr *type)
{
	ID *id = node ? &node->data.ident.id : NULL;
	return append_instr_decl_member_impl(cnt, node, id, type);
}

MirInstr *
append_instr_decl_member_impl(Context *cnt, Ast *node, ID *id, MirInstr *type)
{
	ref_instr(type);
	MirInstrDeclMember *tmp =
	    CREATE_INSTR(cnt, MIR_INSTR_DECL_MEMBER, node, MirInstrDeclMember *);

	tmp->base.value.type      = cnt->builtin_types.t_void;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->base.ref_count       = NO_REF_COUNTING;
	tmp->type                 = type;

	tmp->member = create_member(cnt, node, id, NULL, -1, NULL);

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_decl_arg(Context *cnt, Ast *node, MirInstr *type)
{
	ref_instr(type);
	MirInstrDeclArg *tmp = CREATE_INSTR(cnt, MIR_INSTR_DECL_ARG, node, MirInstrDeclArg *);

	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->base.value.type      = cnt->builtin_types.t_void;
	tmp->base.ref_count       = NO_REF_COUNTING;
	tmp->type                 = type;

	ID *id   = node ? &node->data.ident.id : NULL;
	tmp->arg = create_arg(cnt, node, id, NULL, NULL);

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_decl_variant(Context *cnt, Ast *node, MirInstr *value)
{
	MirInstrDeclVariant *tmp =
	    CREATE_INSTR(cnt, MIR_INSTR_DECL_VARIANT, node, MirInstrDeclVariant *);

	tmp->base.value.type      = cnt->builtin_types.t_void;
	tmp->base.value.eval_mode = MIR_VEM_STATIC;
	tmp->base.ref_count       = NO_REF_COUNTING;
	tmp->value                = value;

	BL_ASSERT(node && node->kind == AST_IDENT);
	ID *   id    = &node->data.ident.id;
	Scope *scope = node->owner_scope;
	tmp->variant = create_variant(cnt, id, scope, NULL);

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

static MirInstr *
create_instr_const_int(Context *cnt, Ast *node, MirType *type, u64 val)
{
	MirInstr *tmp = CREATE_INSTR(cnt, MIR_INSTR_CONST, node, MirInstr *);
	init_or_create_const_integer(cnt, &tmp->value, type, val);

	return tmp;
}

static MirInstr *
create_instr_const_type(Context *cnt, Ast *node, MirType *type)
{
	MirInstr *tmp        = CREATE_INSTR(cnt, MIR_INSTR_CONST, node, MirInstr *);
	tmp->value.type      = cnt->builtin_types.t_type;
	tmp->value.addr_mode = MIR_VAM_RVALUE;
	tmp->value.eval_mode = MIR_VEM_STATIC;

	mir_set_const_ptr(&tmp->value.data.v_ptr, type, MIR_CP_TYPE);
	return tmp;
}

MirInstr *
append_instr_const_int(Context *cnt, Ast *node, MirType *type, u64 val)
{
	MirInstr *tmp = create_instr_const_int(cnt, node, type, val);

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_float(Context *cnt, Ast *node, float val)
{
	MirInstr *tmp = CREATE_INSTR(cnt, MIR_INSTR_CONST, node, MirInstr *);

	tmp->value.type       = cnt->builtin_types.t_f32;
	tmp->value.data.v_f32 = val;
	tmp->value.addr_mode  = MIR_VAM_RVALUE;
	tmp->value.eval_mode  = MIR_VEM_STATIC;

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_double(Context *cnt, Ast *node, double val)
{
	MirInstr *tmp = CREATE_INSTR(cnt, MIR_INSTR_CONST, node, MirInstr *);

	tmp->value.type       = cnt->builtin_types.t_f64;
	tmp->value.data.v_f64 = val;
	tmp->value.addr_mode  = MIR_VAM_RVALUE;
	tmp->value.eval_mode  = MIR_VEM_STATIC;

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_bool(Context *cnt, Ast *node, bool val)
{
	MirInstr *tmp = CREATE_INSTR(cnt, MIR_INSTR_CONST, node, MirInstr *);
	init_or_create_const_bool(cnt, &tmp->value, val);

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_string(Context *cnt, Ast *node, const char *str)
{
	MirInstr *tmp = CREATE_INSTR(cnt, MIR_INSTR_CONST, node, MirInstr *);
	init_or_create_const_string(cnt, &tmp->value, str);

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_char(Context *cnt, Ast *node, char c)
{
	MirInstr *tmp        = CREATE_INSTR(cnt, MIR_INSTR_CONST, node, MirInstr *);
	tmp->value.type      = cnt->builtin_types.t_u8;
	tmp->value.addr_mode = MIR_VAM_RVALUE;
	tmp->value.eval_mode = MIR_VEM_STATIC;

	tmp->value.data.v_char = c;

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_null(Context *cnt, Ast *node)
{
	MirInstr *tmp        = CREATE_INSTR(cnt, MIR_INSTR_CONST, node, MirInstr *);
	tmp->value.type      = create_type_null(cnt, cnt->builtin_types.t_u8_ptr);
	tmp->value.addr_mode = MIR_VAM_RVALUE;
	tmp->value.eval_mode = MIR_VEM_STATIC;

	mir_set_const_ptr(&tmp->value.data.v_ptr, NULL, MIR_CP_VALUE);

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_ret(Context *cnt, Ast *node, MirInstr *value, bool infer_type)
{
	if (value) ref_instr(value);

	MirInstrRet *tmp          = CREATE_INSTR(cnt, MIR_INSTR_RET, node, MirInstrRet *);
	tmp->base.value.type      = cnt->builtin_types.t_void;
	tmp->base.value.eval_mode = MIR_VEM_NONE;
	tmp->base.ref_count       = NO_REF_COUNTING;
	tmp->value                = value;
	tmp->infer_type           = infer_type;

	MirInstrBlock *block = get_current_block(cnt);

	append_current_block(cnt, &tmp->base);
	if (!is_block_terminated(block)) terminate_block(block, &tmp->base);

	MirFn *fn = block->owner_fn;
	BL_ASSERT(fn);

	fn->terminal_instr = tmp;

	return &tmp->base;
}

MirInstr *
append_instr_store(Context *cnt, Ast *node, MirInstr *src, MirInstr *dest)
{
	BL_ASSERT(src && dest);
	ref_instr(src);
	ref_instr(dest);

	MirInstrStore *tmp        = CREATE_INSTR(cnt, MIR_INSTR_STORE, node, MirInstrStore *);
	tmp->base.value.type      = cnt->builtin_types.t_void;
	tmp->base.value.eval_mode = MIR_VEM_NONE;
	tmp->base.ref_count       = NO_REF_COUNTING;
	tmp->src                  = src;
	tmp->dest                 = dest;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_binop(Context *cnt, Ast *node, MirInstr *lhs, MirInstr *rhs, BinopKind op)
{
	BL_ASSERT(lhs && rhs);
	ref_instr(lhs);
	ref_instr(rhs);
	MirInstrBinop *tmp = CREATE_INSTR(cnt, MIR_INSTR_BINOP, node, MirInstrBinop *);
	tmp->lhs           = lhs;
	tmp->rhs           = rhs;
	tmp->op            = op;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_unop(Context *cnt, Ast *node, MirInstr *instr, UnopKind op)
{
	BL_ASSERT(instr);
	ref_instr(instr);
	MirInstrUnop *tmp = CREATE_INSTR(cnt, MIR_INSTR_UNOP, node, MirInstrUnop *);
	tmp->expr         = instr;
	tmp->op           = op;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
create_instr_vargs_impl(Context *cnt, MirType *type, TSmallArray_InstrPtr *values)
{
	BL_ASSERT(type);
	MirInstrVArgs *tmp = CREATE_INSTR(cnt, MIR_INSTR_VARGS, NULL, MirInstrVArgs *);
	tmp->type          = type;
	tmp->values        = values;

	return &tmp->base;
}

/* analyze */
void
erase_instr_tree(MirInstr *instr)
{
	if (!instr) return;

	TSmallArray_InstrPtr64 queue;
	tsa_init(&queue);

	tsa_push_InstrPtr64(&queue, instr);

	MirInstr *top;
	while (queue.size) {
		top = tsa_pop_InstrPtr64(&queue);

		if (!top) continue;

		BL_ASSERT(top->analyzed && "Trying to erase not analyzed instruction.");
		if (top->ref_count == NO_REF_COUNTING) continue;
		if (top->ref_count > 0) continue;

		switch (top->kind) {
		case MIR_INSTR_BINOP: {
			MirInstrBinop *binop = (MirInstrBinop *)top;
			unref_instr(binop->lhs);
			unref_instr(binop->rhs);

			tsa_push_InstrPtr64(&queue, binop->rhs);
			tsa_push_InstrPtr64(&queue, binop->lhs);
			break;
		}

		case MIR_INSTR_LOAD: {
			MirInstrLoad *load = (MirInstrLoad *)top;
			unref_instr(load->src);

			tsa_push_InstrPtr64(&queue, load->src);
			break;
		}

		case MIR_INSTR_SIZEOF: {
			MirInstrSizeof *szof = (MirInstrSizeof *)top;
			unref_instr(szof->expr);

			tsa_push_InstrPtr64(&queue, szof->expr);
			break;
		}

		case MIR_INSTR_ELEM_PTR: {
			MirInstrElemPtr *ep = (MirInstrElemPtr *)top;
			unref_instr(ep->arr_ptr);
			unref_instr(ep->index);

			tsa_push_InstrPtr64(&queue, ep->arr_ptr);
			tsa_push_InstrPtr64(&queue, ep->index);
			break;
		}

		case MIR_INSTR_MEMBER_PTR: {
			MirInstrMemberPtr *mp = (MirInstrMemberPtr *)top;
			unref_instr(mp->target_ptr);

			tsa_push_InstrPtr64(&queue, mp->target_ptr);
			break;
		}

		case MIR_INSTR_TYPE_INFO: {
			MirInstrTypeInfo *info = (MirInstrTypeInfo *)top;
			unref_instr(info->expr);

			tsa_push_InstrPtr64(&queue, info->expr);
			break;
		}

		case MIR_INSTR_CAST: {
			MirInstrCast *cast = (MirInstrCast *)top;
			unref_instr(cast->expr);
			unref_instr(cast->type);

			tsa_push_InstrPtr64(&queue, cast->expr);
			tsa_push_InstrPtr64(&queue, cast->type);
			break;
		}

		case MIR_INSTR_CALL: {
			MirInstrCall *call = (MirInstrCall *)top;
			if (call->args) {
				MirInstr *it;
				TSA_FOREACH(call->args, it)
				{
					unref_instr(it);
					tsa_push_InstrPtr64(&queue, it);
				}
			}
			break;
		}

		case MIR_INSTR_ADDROF: {
			MirInstrAddrof *addrof = (MirInstrAddrof *)top;
			unref_instr(addrof->src);
			tsa_push_InstrPtr64(&queue, addrof->src);
			break;
		}

		case MIR_INSTR_UNOP: {
			MirInstrUnop *unop = (MirInstrUnop *)top;
			unref_instr(unop->expr);
			tsa_push_InstrPtr64(&queue, unop->expr);
			break;
		}

		case MIR_INSTR_TYPE_PTR: {
			MirInstrTypePtr *tp = (MirInstrTypePtr *)top;
			unref_instr(tp->type);
			tsa_push_InstrPtr64(&queue, tp->type);
			break;
		}

		case MIR_INSTR_TYPE_ENUM: {
			MirInstrTypeEnum *te = (MirInstrTypeEnum *)top;
			unref_instr(te->base_type);
			tsa_push_InstrPtr64(&queue, te->base_type);

			MirInstr *it;
			TSA_FOREACH(te->variants, it)
			{
				unref_instr(it);
				tsa_push_InstrPtr64(&queue, it);
			}
			break;
		}

		case MIR_INSTR_TYPE_FN: {
			MirInstrTypeFn *tf = (MirInstrTypeFn *)top;
			unref_instr(tf->ret_type);
			tsa_push_InstrPtr64(&queue, tf->ret_type);

			if (tf->args) {
				MirInstr *it;
				TSA_FOREACH(tf->args, it)
				{
					unref_instr(it);
					tsa_push_InstrPtr64(&queue, it);
				}
			}
			break;
		}

		case MIR_INSTR_TYPE_ARRAY: {
			MirInstrTypeArray *ta = (MirInstrTypeArray *)top;
			unref_instr(ta->elem_type);
			unref_instr(ta->len);
			tsa_push_InstrPtr64(&queue, ta->elem_type);
			tsa_push_InstrPtr64(&queue, ta->len);
			break;
		}

		case MIR_INSTR_TYPE_SLICE:
		case MIR_INSTR_TYPE_STRUCT: {
			MirInstrTypeStruct *ts = (MirInstrTypeStruct *)top;

			if (ts->members) {
				MirInstr *it;
				TSA_FOREACH(ts->members, it)
				{
					unref_instr(it);
					tsa_push_InstrPtr64(&queue, it);
				}
			}
			break;
		}

		case MIR_INSTR_VARGS: {
			MirInstrVArgs *vargs = (MirInstrVArgs *)top;
			if (vargs->values) {
				MirInstr *it;
				TSA_FOREACH(vargs->values, it)
				{
					unref_instr(it);
					tsa_push_InstrPtr64(&queue, it);
				}
			}
			break;
		}

		case MIR_INSTR_BLOCK:
			continue;

		case MIR_INSTR_DECL_REF:
		case MIR_INSTR_CONST:
			break;

		default:
			BL_ABORT("Missing erase for instruction '%s'", mir_instr_name(top));
		}

		erase_instr(top);
	}

	tsa_terminate(&queue);
}

void
reduce_instr(Context *cnt, MirInstr *instr)
{
	if (!instr) return;
	/* instruction unknown in compile time cannot be reduced */
	if (!mir_is_comptime(&instr->value)) return;

	switch (instr->kind) {
		/* Const expr value set during analyze pass. */
	case MIR_INSTR_TYPE_FN:
	case MIR_INSTR_TYPE_ARRAY:
	case MIR_INSTR_TYPE_PTR:
	case MIR_INSTR_TYPE_STRUCT:
	case MIR_INSTR_TYPE_SLICE:
	case MIR_INSTR_TYPE_VARGS:
	case MIR_INSTR_TYPE_ENUM:
	case MIR_INSTR_CONST:
	case MIR_INSTR_DECL_MEMBER:
	case MIR_INSTR_DECL_VARIANT:
	case MIR_INSTR_DECL_ARG: {
		erase_instr(instr);
		break;
	}

	case MIR_INSTR_DECL_REF:
	case MIR_INSTR_DECL_DIRECT_REF:
	case MIR_INSTR_CAST:
	case MIR_INSTR_BINOP:
	case MIR_INSTR_UNOP:
	case MIR_INSTR_ELEM_PTR:
	case MIR_INSTR_ADDROF:
	case MIR_INSTR_SIZEOF:
	case MIR_INSTR_ALIGNOF:
	case MIR_INSTR_LOAD:
	case MIR_INSTR_COMPOUND:
	case MIR_INSTR_MEMBER_PTR: {
		vm_eval_comptime_instr(&cnt->vm, instr);
		erase_instr(instr);
		break;
	}

	default:
		break;
	}
}

AnalyzeResult
analyze_resolve_type(Context *cnt, MirInstr *resolver_call, MirType **out_type)
{
	BL_ASSERT(resolver_call && "Expected resolver call.");
	BL_ASSERT(resolver_call->kind == MIR_INSTR_CALL &&
	          "Type resolver is expected to be call to resolve function.");

	if (analyze_instr(cnt, resolver_call).state != ANALYZE_PASSED)
		return ANALYZE_RESULT(POSTPONE, 0);

	vm_eval_comptime_instr(&cnt->vm, resolver_call);
	*out_type =
	    mir_get_const_ptr(MirType *, &resolver_call->value.data.v_ptr, MIR_CP_TYPE | MIR_CP_FN);

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_toany(Context *cnt, MirInstrToAny *toany)
{
	/* INCOMPLETE: if we need to generate comptime version of ToAny instruction,
	 * there is issue with typeinfo which is generated after analyze pass, so ToAny cannot
	 * be executed in reduction function. */
	MirType *toany_type = mir_deref_type(toany->base.value.type);
	BL_ASSERT(toany->expr && "Missing expression as toany input.");

	reduce_instr(cnt, toany->expr);

	MirInstr *expr      = toany->expr;
	MirType * rtti_type = expr->value.type;

	if (expr->value.addr_mode == MIR_VAM_RVALUE && rtti_type->kind != MIR_TYPE_FN) {
		/* Target expression is not allocated object on the stack, so we need to crate
		 * temporary variable containing the value and fetch pointer to this variable. */
		const char *tmp_var_name = gen_uq_name(IMPL_ANY_EXPR_TMP);
		toany->expr_tmp =
		    create_var_impl(cnt, tmp_var_name, rtti_type, false, false, false);
	} else if (is_load_needed(expr)) {
		rtti_type = mir_deref_type(rtti_type);
	}

	BL_ASSERT(rtti_type);
	schedule_RTTI_generation(cnt, rtti_type);

	{ /* Tmp variable for Any */
		const char *tmp_var_name = gen_uq_name(IMPL_ANY_TMP);
		toany->tmp = create_var_impl(cnt, tmp_var_name, toany_type, false, false, false);
	}

	toany->has_data = rtti_type->kind != MIR_TYPE_VOID && rtti_type->kind != MIR_TYPE_NULL;

	/* When we pass type declaration reference as an expression into the toany instruction we
	 * need include pointer to type info of the real type passed. That means when we pass 's32'
	 * we get resulting any structure containing type info for Type and type info for s32 as
	 * data pointer. This is how we can later implement for example printing of type layout. */
	if (rtti_type->kind == MIR_TYPE_TYPE || rtti_type->kind == MIR_TYPE_FN) {
		MirConstPtr *cp                 = &expr->value.data.v_ptr;
		MirType *    specification_type = NULL;

		/* HACK: There is probably better solution, here we handle situation when type is
		 * not fundamental type but custom type declaration where actual type is stored in
		 * constant variable. */
		if (cp->kind == MIR_CP_TYPE) {
			specification_type = cp->data.type;
		} else if (cp->kind == MIR_CP_VAR) {
			specification_type = cp->data.var->value.data.v_ptr.data.type;
		} else if (cp->kind == MIR_CP_FN) {
			specification_type = cp->data.fn->type;
		}

		BL_ASSERT(specification_type && "Missing type specification data!");
		schedule_RTTI_generation(cnt, specification_type);
		toany->rtti_type_specification = specification_type;
	}

	toany->rtti_type = rtti_type;

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_phi(Context *cnt, MirInstrPhi *phi)
{
	BL_ASSERT(phi->incoming_blocks && phi->incoming_values);
	BL_ASSERT(phi->incoming_values->size == phi->incoming_blocks->size);

	const usize count = phi->incoming_values->size;

	MirInstr **value_ref;
	MirInstr * block;
	MirType *  type = NULL;

	bool is_comptime = true;

	for (usize i = 0; i < count; ++i) {
		value_ref = &phi->incoming_values->data[i];
		block     = phi->incoming_blocks->data[i];
		BL_ASSERT(block && block->kind == MIR_INSTR_BLOCK)

		const AnalyzeSlotConfig *conf =
		    type ? &analyze_slot_conf_default : &analyze_slot_conf_basic;

		if (analyze_slot(cnt, conf, value_ref, type) != ANALYZE_PASSED)
			return ANALYZE_RESULT(FAILED, 0);

		if (!type) type = (*value_ref)->value.type;
		is_comptime = mir_is_comptime(&(*value_ref)->value) ? is_comptime : false;
	}

	BL_ASSERT(type && "Cannot resolve type of phi instruction!");
	phi->base.value.type      = type;
	phi->base.value.addr_mode = MIR_VAM_RVALUE;

	/* INCOMPLETE: comptime phi? */
	phi->base.value.eval_mode = is_comptime ? MIR_VEM_STATIC : MIR_VEM_RUNTIME;

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_compound(Context *cnt, MirInstrCompound *cmp)
{
	/* Setup compound type. */
	MirType *             type   = cmp->base.value.type;
	TSmallArray_InstrPtr *values = cmp->values;
	if (!type) {
		/* generate load instruction if needed */
		BL_ASSERT(cmp->type->analyzed);
		if (analyze_slot(cnt, &analyze_slot_conf_basic, &cmp->type, NULL) != ANALYZE_PASSED)
			return ANALYZE_RESULT(FAILED, 0);

		MirInstr *instr_type = cmp->type;
		if (instr_type->value.type->kind != MIR_TYPE_TYPE) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_INVALID_TYPE,
			            instr_type->node->location,
			            BUILDER_CUR_WORD,
			            "Expected type before compound expression.");
			return ANALYZE_RESULT(FAILED, 0);
		}
		type = mir_get_const_ptr(MirType *, &instr_type->value.data.v_ptr, MIR_CP_TYPE);
	}

	BL_ASSERT(type);

	if (!values) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_INITIALIZER,
		            cmp->type->node->location,
		            BUILDER_CUR_AFTER,
		            "Expected value after ':'.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	cmp->base.value.type      = type;
	cmp->base.value.eval_mode = MIR_VEM_LAZY;

	/* Check if array is supposed to be initilialized to {0} */
	if (values->size == 1) {
		MirInstr *value = values->data[0];
		if (value->kind == MIR_INSTR_CONST && value->value.type->kind == MIR_TYPE_INT &&
		    value->value.data.v_u64 == 0) {
			reduce_instr(cnt, value);
			cmp->is_zero_initialized = true;
		}
	}

	switch (type->kind) {
	case MIR_TYPE_ARRAY: {
		if (cmp->is_zero_initialized) {
			cmp->base.value.data.v_array.is_zero_initializer = true;
			break;
		}

		if (values->size != (usize)type->data.array.len) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_INVALID_INITIALIZER,
			            cmp->base.node->location,
			            BUILDER_CUR_WORD,
			            "Array initializer must explicitly set all array elements of "
			            "the array or "
			            "initialize array to 0 by zero initializer {0}. Expected is "
			            "%llu but given %llu.",
			            (unsigned long long)type->data.array.len,
			            (unsigned long long)values->size);
			return ANALYZE_RESULT(FAILED, 0);
		}

		/* Else iterate over values */
		MirInstr **value_ref;
		for (usize i = 0; i < values->size; ++i) {
			value_ref = &values->data[i];

			if (analyze_slot(cnt,
			                 &analyze_slot_conf_default,
			                 value_ref,
			                 type->data.array.elem_type) != ANALYZE_PASSED)
				return ANALYZE_RESULT(FAILED, 0);

			cmp->base.value.eval_mode = mir_is_comptime(&(*value_ref)->value)
			                                ? cmp->base.value.eval_mode
			                                : MIR_VEM_RUNTIME;
		}

		break;
	}

	case MIR_TYPE_SLICE:
	case MIR_TYPE_STRING:
	case MIR_TYPE_VARGS:
	case MIR_TYPE_STRUCT: {
		BL_UNIMPLEMENTED;
		if (cmp->is_zero_initialized) {
			cmp->base.value.data.v_struct.is_zero_initializer = true;
			break;
		}

		const usize memc = type->data.strct.members->size;
		if (values->size != memc) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_INVALID_INITIALIZER,
			            cmp->base.node->location,
			            BUILDER_CUR_WORD,
			            "Structure initializer must explicitly set all members of the "
			            "structure or initialize structure to 0 by zero initializer "
			            "{0}. Expected is %llu but given %llu.",
			            (unsigned long long)memc,
			            (unsigned long long)values->size);
			return ANALYZE_RESULT(FAILED, 0);
		}

		/* Else iterate over values */
		MirInstr **value_ref;
		MirType *  member_type;
		for (u32 i = 0; i < values->size; ++i) {
			value_ref   = &values->data[i];
			member_type = mir_get_struct_elem_type(type, i);

			if (analyze_slot(cnt, &analyze_slot_conf_default, value_ref, member_type) !=
			    ANALYZE_PASSED)
				return ANALYZE_RESULT(FAILED, 0);

			cmp->base.value.eval_mode = mir_is_comptime(&(*value_ref)->value)
			                                ? cmp->base.value.eval_mode
			                                : MIR_VEM_RUNTIME;
		}

		// NOTE: Instructions can be used as values!!!
		if (mir_is_comptime(&cmp->base.value)) {
			init_or_create_const_struct(
			    cnt, &cmp->base.value, type, (TSmallArray_ConstValuePtr *)values);
		}
		break;
	}

	default: {
		BL_UNIMPLEMENTED;
		/* Non-agregate type. */
		if (values->size > 1) {
			MirInstr *value = values->data[1];
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_INVALID_INITIALIZER,
			            value->node->location,
			            BUILDER_CUR_WORD,
			            "One value only is expected for non-agragate types.");
			return ANALYZE_RESULT(FAILED, 0);
		}

		MirInstr **value_ref = &values->data[0];

		const AnalyzeSlotConfig *conf =
		    type ? &analyze_slot_conf_default : &analyze_slot_conf_basic;

		if (analyze_slot(cnt, conf, value_ref, type) != ANALYZE_PASSED)
			return ANALYZE_RESULT(FAILED, 0);

		cmp->base.value.eval_mode = (*value_ref)->value.eval_mode;
		if (mir_is_comptime(&cmp->base.value)) {
			cmp->base.value = (*value_ref)->value;
		}
	}
	}

	if (!mir_is_comptime(&cmp->base.value) && cmp->is_naked) {
		/* For naked non-compile time compounds we need to generate implicit temp storage to
		 * keep all data. */

		const char *tmp_name = gen_uq_name(IMPL_COMPOUND_TMP);
		MirVar *    tmp_var  = create_var_impl(cnt, tmp_name, type, true, false, false);
		cmp->tmp_var         = tmp_var;
	}

	if (mir_is_comptime(&cmp->base.value)) {
		cmp->base.value.addr_mode = MIR_VAM_LVALUE_CONST;
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_vargs(Context *cnt, MirInstrVArgs *vargs)
{
	MirType *             type   = vargs->type;
	TSmallArray_InstrPtr *values = vargs->values;
	BL_ASSERT(type && values);

	type = create_type_struct_special(cnt, MIR_TYPE_VARGS, NULL, create_type_ptr(cnt, type));

	const usize valc = values->size;

	if (valc > 0) {
		/* Prepare tmp array for values */
		const char *tmp_name = gen_uq_name(IMPL_VARGS_TMP_ARR);
		MirType *   tmp_type = create_type_array(cnt, vargs->type, valc);
		vargs->arr_tmp       = create_var_impl(cnt, tmp_name, tmp_type, true, false, false);
	}

	{
		/* Prepare tmp slice for vargs */
		const char *tmp_name = gen_uq_name(IMPL_VARGS_TMP);
		vargs->vargs_tmp     = create_var_impl(cnt, tmp_name, type, true, false, false);
	}

	MirInstr **value;
	bool       is_valid = true;

	for (usize i = 0; i < valc && is_valid; ++i) {
		value = &values->data[i];

		if (analyze_slot(cnt, &analyze_slot_conf_full, value, vargs->type) !=
		    ANALYZE_PASSED)
			return ANALYZE_RESULT(FAILED, 0);
	}

	vargs->base.value.type = type;
	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_elem_ptr(Context *cnt, MirInstrElemPtr *elem_ptr)
{
	if (analyze_slot(
	        cnt, &analyze_slot_conf_default, &elem_ptr->index, cnt->builtin_types.t_s64) !=
	    ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	MirInstr *arr_ptr = elem_ptr->arr_ptr;
	BL_ASSERT(arr_ptr);
	BL_ASSERT(arr_ptr->value.type);

	if (!mir_is_pointer_type(arr_ptr->value.type)) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            elem_ptr->arr_ptr->node->location,
		            BUILDER_CUR_WORD,
		            "Expected array type or slice.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	MirType *arr_type = mir_deref_type(arr_ptr->value.type);
	BL_ASSERT(arr_type);

	if (arr_type->kind == MIR_TYPE_ARRAY) {
		/* array */
		if (mir_is_comptime(&elem_ptr->index->value)) {
			const s64 len = arr_type->data.array.len;
			const s64 i   = elem_ptr->index->value.data.v_u64;
			if (i >= len || i < 0) {
				builder_msg(BUILDER_MSG_ERROR,
				            ERR_BOUND_CHECK_FAILED,
				            elem_ptr->index->node->location,
				            BUILDER_CUR_WORD,
				            "Array index is out of the bounds, array size is %lli "
				            "so index must fit in range from 0 to %lli.",
				            len,
				            len - 1);
				return ANALYZE_RESULT(FAILED, 0);
			}
		}

		/* setup ElemPtr instruction const_value type */
		MirType *elem_type = arr_type->data.array.elem_type;
		BL_ASSERT(elem_type);
		elem_ptr->base.value.type = create_type_ptr(cnt, elem_type);
	} else if (arr_type->kind == MIR_TYPE_SLICE || arr_type->kind == MIR_TYPE_STRING ||
	           arr_type->kind == MIR_TYPE_VARGS) {
		/* Support of direct slice access -> slice[N]
		 * Since slice is special kind of structure data we need to handle
		 * access to pointer and lenght later during execuion. We cannot create
		 * member poiner instruction here because we need check boundaries on
		 * array later during runtime. This leads to special kind of elemptr
		 * interpretation and IR generation also.
		 */

		/* setup type */
		MirType *elem_type = mir_get_struct_elem_type(arr_type, MIR_SLICE_PTR_INDEX);
		BL_ASSERT(elem_type);
		elem_ptr->base.value.type = elem_type;

		/* this is important!!! */
		elem_ptr->target_is_slice = true;
	} else {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            arr_ptr->node->location,
		            BUILDER_CUR_WORD,
		            "Expected array or slice type.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	elem_ptr->base.value.addr_mode = elem_ptr->arr_ptr->value.addr_mode;

	/* Elem ptr is comptime only when target and index are comptime. */
	elem_ptr->base.value.eval_mode =
	    mir_is_comptime(&elem_ptr->arr_ptr->value) && mir_is_comptime(&elem_ptr->index->value)
	        ? MIR_VEM_STATIC
	        : MIR_VEM_RUNTIME;

	reduce_instr(cnt, elem_ptr->arr_ptr);
	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_member_ptr(Context *cnt, MirInstrMemberPtr *member_ptr)
{
	MirInstr *target_ptr = member_ptr->target_ptr;
	BL_ASSERT(target_ptr);
	MirType *target_type = target_ptr->value.type;

	if (target_type->kind != MIR_TYPE_PTR) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            target_ptr->node->location,
		            BUILDER_CUR_WORD,
		            "Expected structure type.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	MirValueAddressMode target_addr_mode = target_ptr->value.addr_mode;
	Ast *               ast_member_ident = member_ptr->member_ident;

	target_type = mir_deref_type(target_type);

	/* Array type */
	if (target_type->kind == MIR_TYPE_ARRAY) {
		/* check array builtin members */
		if (member_ptr->builtin_id == MIR_BUILTIN_ID_ARR_LEN ||
		    is_builtin(ast_member_ident, MIR_BUILTIN_ID_ARR_LEN)) {
			/* .len */
			/* mutate instruction into constant */
			unref_instr(member_ptr->target_ptr);
			erase_instr_tree(member_ptr->target_ptr);
			MirInstr *len         = mutate_instr(&member_ptr->base, MIR_INSTR_CONST);
			len->value.eval_mode  = MIR_VEM_STATIC;
			len->value.type       = cnt->builtin_types.t_s64;
			len->value.data.v_s64 = target_type->data.array.len;
		} else if (member_ptr->builtin_id == MIR_BUILTIN_ID_ARR_PTR ||
		           is_builtin(ast_member_ident, MIR_BUILTIN_ID_ARR_PTR)) {
			/* .ptr -> This will be replaced by:
			 *     elemptr
			 *     addrof
			 * to match syntax: &array[0]
			 */

			MirInstr *index =
			    create_instr_const_int(cnt, NULL, cnt->builtin_types.t_s64, 0);
			MirInstr *elem_ptr =
			    create_instr_elem_ptr(cnt, NULL, target_ptr, index, false);
			ref_instr(elem_ptr);

			insert_instr_before(&member_ptr->base, elem_ptr);

			analyze_instr_rq(cnt, index);
			analyze_instr_rq(cnt, elem_ptr);

			MirInstrAddrof *addrof_elem =
			    (MirInstrAddrof *)mutate_instr(&member_ptr->base, MIR_INSTR_ADDROF);
			addrof_elem->src = elem_ptr;
			analyze_instr_rq(cnt, &addrof_elem->base);
		} else {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_INVALID_MEMBER_ACCESS,
			            ast_member_ident->location,
			            BUILDER_CUR_WORD,
			            "Unknown member.");
			return ANALYZE_RESULT(FAILED, 0);
		}

		member_ptr->base.value.addr_mode = target_addr_mode;
		return ANALYZE_RESULT(PASSED, 0);
	}

	bool additional_load_needed = false;
	if (target_type->kind == MIR_TYPE_PTR) {
		/* We try to access structure member via pointer so we need one more load.
		 */

		additional_load_needed = true;
		target_type            = mir_deref_type(target_type);
	}

	/* composit types */
	if (mir_is_composit_type(target_type)) {
		/* Check if structure type is complete, if not analyzer must wait for it!  */
		if (is_incomplete_struct_type(target_type))
			return ANALYZE_RESULT(WAITING, target_type->user_id->hash);

		if (additional_load_needed) {
			member_ptr->target_ptr = insert_instr_load(cnt, member_ptr->target_ptr);
			BL_ASSERT(member_ptr->target_ptr);
		}

		reduce_instr(cnt, member_ptr->target_ptr);

		Scope *     scope = target_type->data.strct.scope;
		ID *        rid   = &ast_member_ident->data.ident.id;
		ScopeEntry *found = NULL;
		MirType *   type  = target_type;

		while (true) {
			found = scope_lookup(scope, rid, false, true);
			if (found) break;

			scope = get_base_type_scope(type);
			type  = get_base_type(type);
			if (!scope) break;
		}

		/* Check if member was found in base type's scope. */
		if (found && found->parent_scope != target_type->data.strct.scope) {
			/* HACK: It seems to be the best way for now just create implicit
			 * cast to desired base type and use this as target, that also
			 * should solve problems with deeper nesting (bitcast of pointer is
			 * better then multiple GEPs?) */
			if (is_load_needed(member_ptr->target_ptr))
				member_ptr->target_ptr =
				    insert_instr_addrof(cnt, member_ptr->target_ptr);

			member_ptr->target_ptr = insert_instr_cast(
			    cnt, member_ptr->target_ptr, create_type_ptr(cnt, type));
		}

		if (!found) {
			/* Member not found! */
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_UNKNOWN_SYMBOL,
			            member_ptr->member_ident->location,
			            BUILDER_CUR_WORD,
			            "Unknown structure member.");
			return ANALYZE_RESULT(FAILED, 0);
		}

		BL_ASSERT(found->kind == SCOPE_ENTRY_MEMBER);
		MirMember *member = found->data.member;

		/* setup member_ptr type */
		member_ptr->base.value.type      = create_type_ptr(cnt, member->type);
		member_ptr->base.value.addr_mode = target_addr_mode;
		member_ptr->base.value.eval_mode = target_ptr->value.eval_mode;
		member_ptr->scope_entry          = found;

		return ANALYZE_RESULT(PASSED, 0);
	}

	/* Sub type member. */
	if (target_type->kind == MIR_TYPE_TYPE) {
		if (analyze_slot(cnt, &analyze_slot_conf_basic, &member_ptr->target_ptr, NULL) !=
		    ANALYZE_PASSED) {
			return ANALYZE_RESULT(FAILED, 0);
		}

		MirType *sub_type = mir_get_const_ptr(
		    MirType *, &member_ptr->target_ptr->value.data.v_ptr, MIR_CP_TYPE);
		BL_ASSERT(sub_type);

		if (sub_type->kind != MIR_TYPE_ENUM) {
			goto INVALID;
		}

		/* lookup for member inside scope */
		Scope *     scope = sub_type->data.enm.scope;
		ID *        rid   = &ast_member_ident->data.ident.id;
		ScopeEntry *found = scope_lookup(scope, rid, false, true);
		if (!found) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_UNKNOWN_SYMBOL,
			            member_ptr->member_ident->location,
			            BUILDER_CUR_WORD,
			            "Unknown enumerator variant.");
			return ANALYZE_RESULT(FAILED, 0);
		}

		BL_ASSERT(found->kind == SCOPE_ENTRY_VARIANT);

		member_ptr->scope_entry          = found;
		member_ptr->base.value.type      = sub_type;
		member_ptr->base.value.addr_mode = target_addr_mode;
		member_ptr->base.value.eval_mode = target_ptr->value.eval_mode;

		return ANALYZE_RESULT(PASSED, 0);
	}

	/* Invalid */
INVALID:
	builder_msg(BUILDER_MSG_ERROR,
	            ERR_INVALID_MEMBER_ACCESS,
	            target_ptr->node->location,
	            BUILDER_CUR_WORD,
	            "Expected structure or enumerator type.");
	return ANALYZE_RESULT(FAILED, 0);
}

AnalyzeResult
analyze_instr_addrof(Context *cnt, MirInstrAddrof *addrof)
{
	MirInstr *src = addrof->src;
	BL_ASSERT(src);
	const MirValueAddressMode src_addr_mode = src->value.addr_mode;

	const bool can_grab_address = src_addr_mode == MIR_VAM_LVALUE ||
	                              src_addr_mode == MIR_VAM_LVALUE_CONST ||
	                              src->value.type->kind == MIR_TYPE_FN;

	if (!can_grab_address) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_EXPECTED_DECL,
		            addrof->base.node->location,
		            BUILDER_CUR_WORD,
		            "Cannot take the address of unallocated object.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	/* setup type */
	MirType *type = NULL;
	BL_ASSERT(src->value.type);
	if (src->value.type->kind == MIR_TYPE_FN) {
		type = create_type_ptr(cnt, src->value.type);
	} else {
		type = src->value.type;
	}

	reduce_instr(cnt, addrof->src);

	addrof->base.value.type      = type;
	addrof->base.value.eval_mode = addrof->src->value.eval_mode;
	addrof->base.value.addr_mode = MIR_VAM_LVALUE_CONST;
	BL_ASSERT(addrof->base.value.type && "invalid type");

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_cast(Context *cnt, MirInstrCast *cast, bool analyze_op_only)
{
	MirType *dest_type = cast->base.value.type;

	if (!analyze_op_only) {
		if (!dest_type && !cast->auto_cast) {
			AnalyzeResult result = analyze_resolve_type(cnt, cast->type, &dest_type);
			if (result.state != ANALYZE_PASSED) return result;
		}

		const AnalyzeSlotConfig *config =
		    cast->base.implicit ? &analyze_slot_conf_reduce_only : &analyze_slot_conf_basic;

		if (analyze_slot(cnt, config, &cast->expr, dest_type) != ANALYZE_PASSED) {
			return ANALYZE_RESULT(FAILED, 0);
		}

		BL_ASSERT(cast->expr->value.type && "invalid cast source type");

		if (!dest_type && cast->auto_cast) {
			dest_type = cast->expr->value.type;
		}
	}

	BL_ASSERT(dest_type && "invalid cast destination type");
	BL_ASSERT(cast->expr->value.type && "invalid cast source type");

	MirType *expr_type = cast->expr->value.type;

	/* Setup const int type. */
	if (analyze_stage_set_volatile_expr(cnt, &cast->expr, dest_type) == ANALYZE_STAGE_BREAK) {
		cast->op = MIR_CAST_NONE;
		goto DONE;
	}

	cast->op = get_cast_op(expr_type, dest_type);
	if (cast->op == MIR_CAST_INVALID) {
		error_types(
		    expr_type, dest_type, cast->base.node, "Invalid cast from '%s' to '%s'.");
		return ANALYZE_RESULT(FAILED, 0);
	}

DONE:
	cast->base.value.type      = dest_type;
	cast->base.value.eval_mode = cast->expr->value.eval_mode;

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_sizeof(Context *cnt, MirInstrSizeof *szof)
{
	BL_ASSERT(szof->expr);

	if (analyze_slot(cnt, &analyze_slot_conf_basic, &szof->expr, NULL) != ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	/* sizeof operator needs only type of input expression so we can erase whole call
	 * tree generated to get this expression */
	unref_instr(szof->expr);
	erase_instr_tree(szof->expr);
	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_type_info(Context *cnt, MirInstrTypeInfo *type_info)
{
	BL_ASSERT(type_info->expr);

	/* Resolve TypeInfo struct type */
	MirType *ret_type = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO);
	if (!ret_type) return ANALYZE_RESULT(POSTPONE, 0);

	if (analyze_slot(cnt, &analyze_slot_conf_basic, &type_info->expr, NULL) != ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	MirType *type = type_info->expr->value.type;
	BL_ASSERT(type);

	if (type->kind == MIR_TYPE_TYPE) {
		type =
		    mir_get_const_ptr(MirType *, &type_info->expr->value.data.v_ptr, MIR_CP_TYPE);
		BL_ASSERT(type);
	}

	type_info->expr_type = type;

	schedule_RTTI_generation(cnt, type);

	ret_type                   = create_type_ptr(cnt, ret_type);
	type_info->base.value.type = ret_type;

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_alignof(Context *cnt, MirInstrAlignof *alof)
{
	BL_ASSERT(alof->expr);

	if (analyze_slot(cnt, &analyze_slot_conf_basic, &alof->expr, NULL) != ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_decl_ref(Context *cnt, MirInstrDeclRef *ref)
{
	BL_ASSERT(ref->rid && ref->scope);

	ScopeEntry *found         = NULL;
	Scope *     private_scope = ref->parent_unit->private_scope;

	if (!private_scope) { /* reference in unit without private scope  */
		found = scope_lookup(ref->scope, ref->rid, true, false);
	} else { /* reference in unit with private scope */
		/* search in current tree and ignore global scope */
		found = scope_lookup(ref->scope, ref->rid, true, true);

		/* lookup in private scope and global scope also (private scope has global
		 * scope as parent every time) */
		if (!found) found = scope_lookup(private_scope, ref->rid, true, false);
	}

	if (!found) return ANALYZE_RESULT(WAITING, ref->rid->hash);
	if (found->kind == SCOPE_ENTRY_INCOMPLETE) return ANALYZE_RESULT(WAITING, ref->rid->hash);

	switch (found->kind) {
	case SCOPE_ENTRY_FN: {
		MirFn *fn = found->data.fn;
		BL_ASSERT(fn);
		MirType *type = fn->type;
		BL_ASSERT(type);

		ref->base.value.type      = type;
		ref->base.value.addr_mode = MIR_VAM_RVALUE;
		ref->base.value.eval_mode = MIR_VEM_STATIC;
		ref_instr(fn->prototype);
		break;
	}

	case SCOPE_ENTRY_TYPE: {
		ref->base.value.type      = cnt->builtin_types.t_type;
		ref->base.value.addr_mode = MIR_VAM_LVALUE_CONST;
		ref->base.value.eval_mode = MIR_VEM_STATIC;
		break;
	}

	case SCOPE_ENTRY_VARIANT: {
		MirVariant *variant = found->data.variant;
		BL_ASSERT(variant);

		MirType *type = variant->value->type;
		BL_ASSERT(type);

		type                      = create_type_ptr(cnt, type);
		ref->base.value.type      = type;
		ref->base.value.addr_mode = MIR_VAM_LVALUE_CONST;
		ref->base.value.eval_mode = MIR_VEM_STATIC;
		break;
	}

	case SCOPE_ENTRY_VAR: {
		MirVar *var = found->data.var;
		BL_ASSERT(var);

		MirType *type = var->value.type;
		BL_ASSERT(type);

		/* Check if we try get reference to incomplete structure type. */
		if (type->kind == MIR_TYPE_TYPE &&
		    is_incomplete_struct_type(var->value.data.v_ptr.data.type) &&
		    !ref->accept_incomplete_type) {
			return ANALYZE_RESULT(WAITING,
			                      var->value.data.v_ptr.data.type->user_id->hash);
		}
		++var->ref_count;

		type                      = create_type_ptr(cnt, type);
		ref->base.value.type      = type;
		ref->base.value.addr_mode = var->is_mutable ? MIR_VAM_LVALUE : MIR_VAM_LVALUE_CONST;
		ref->base.value.eval_mode = var->is_mutable ? MIR_VEM_RUNTIME : MIR_VEM_STATIC;
		break;
	}

	default:
		BL_ABORT("invalid scope entry kind");
	}

	ref->scope_entry = found;
	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_decl_direct_ref(Context *cnt, MirInstrDeclDirectRef *ref)
{
	BL_ASSERT(ref->ref && "Missing declaration reference for direct ref.");
	BL_ASSERT(ref->ref->kind == MIR_INSTR_DECL_VAR && "Expected variable declaration.");
	BL_ASSERT(ref->ref->analyzed && "Reference not analyzed.");

	MirVar *var = ((MirInstrDeclVar *)ref->ref)->var;
	BL_ASSERT(var);
	++var->ref_count;
	MirType *type = var->value.type;
	BL_ASSERT(type);

	type                      = create_type_ptr(cnt, type);
	ref->base.value.type      = type;
	ref->base.value.eval_mode = mir_is_comptime(&var->value) ? MIR_VEM_STATIC : MIR_VEM_RUNTIME;
	ref->base.value.addr_mode = var->is_mutable ? MIR_VAM_LVALUE : MIR_VAM_LVALUE_CONST;

	/* set pointer to variable const value directly when variable is compile
	 * time known
	 */
	if (mir_is_comptime(&var->value))
		mir_set_const_ptr(&ref->base.value.data.v_ptr, var, MIR_CP_VAR);

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_arg(Context *cnt, MirInstrArg *arg)
{
	MirFn *fn = arg->base.owner_block->owner_fn;
	BL_ASSERT(fn);

	MirType *type = mir_get_fn_arg_type(fn->type, arg->i);
	BL_ASSERT(type);
	arg->base.value.type = type;

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_unreachable(Context *cnt, MirInstrUnreachable *unr)
{
	/* nothing to do :( */
	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_fn_proto(Context *cnt, MirInstrFnProto *fn_proto)
{
	/* resolve type */
	if (!fn_proto->base.value.type) {
		MirType *     fn_type = NULL;
		AnalyzeResult result  = analyze_resolve_type(cnt, fn_proto->type, &fn_type);
		if (result.state != ANALYZE_PASSED) return result;

		/* Analyze user defined type (this must be compared with infered type).
		 */
		if (fn_proto->user_type) {
			MirType *user_fn_type = NULL;
			result = analyze_resolve_type(cnt, fn_proto->user_type, &user_fn_type);
			if (result.state != ANALYZE_PASSED) return result;

			if (!type_cmp(fn_type, user_fn_type)) {
				error_types(fn_type, user_fn_type, fn_proto->user_type->node, NULL);
			}
		}

		fn_proto->base.value.type = fn_type;
	}

	MirConstValue *value = &fn_proto->base.value;

	BL_ASSERT(value->type && "function has no valid type");
	BL_ASSERT(value->data.v_ptr.data.fn);
	value->data.v_ptr.data.fn->type = fn_proto->base.value.type;

	MirFn *fn = fn_proto->base.value.data.v_ptr.data.fn;
	BL_ASSERT(fn);

	if (fn->ret_tmp) {
		BL_ASSERT(fn->ret_tmp->kind == MIR_INSTR_DECL_VAR);
		((MirInstrDeclVar *)fn->ret_tmp)->var->value.type = value->type->data.fn.ret_type;
	}

	/* set type name */
	fn_proto->base.value.type->user_id = fn->id;

	/* Setup function linkage name, this will be later used by LLVM backend. */
	if (fn->id) {
		if (IS_FLAG(fn->flags, FLAG_EXTERN)) {
			fn->linkage_name = fn->id->str;
		} else if (IS_FLAG(fn->flags, FLAG_PRIVATE)) {
			fn->linkage_name = gen_uq_name(fn->id->str);
		} else if (fn->is_in_gscope) {
			fn->linkage_name = fn->id->str;
		} else {
			fn->linkage_name = gen_uq_name(fn->id->str);
		}
	} else {
		/* Anonymous function use implicit unique name. */
		fn->linkage_name = gen_uq_name(IMPL_FN_NAME);
	}

	BL_ASSERT(fn->linkage_name && "Function without linkage name!");

	if (IS_FLAG(fn->flags, FLAG_EXTERN)) {
		/* lookup external function exec handle */
		BL_ASSERT(fn->linkage_name);
		fn->dyncall.extern_entry = assembly_find_extern(cnt->assembly, fn->linkage_name);

		/* I'm not sure if we want this...
		if (!fn->dyncall.extern_entry) {
		        builder_msg(BUILDER_MSG_ERROR,
		                ERR_UNKNOWN_SYMBOL,
		                fn_proto->base.node->location,
		                BUILDER_CUR_WORD,
		                "External symbol '%s' not found.",
		                fn->linkage_name);
		} else {
		        fn->fully_analyzed = true;
		}
		*/
		fn->fully_analyzed = true;
	} else {
		/* Add entry block of the function into analyze queue. */
		MirInstr *entry_block = (MirInstr *)fn->first_block;
		if (!entry_block) {
			/* INCOMPLETE: not the best place to do this check, move into ast
			 * generation later
			 */
			/* INCOMPLETE: not the best place to do this check, move into ast
			 * generation later
			 */
			/* INCOMPLETE: not the best place to do this check, move into ast
			 * generation later
			 */
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_EXPECTED_BODY,
			            fn_proto->base.node->location,
			            BUILDER_CUR_WORD,
			            "Missing function body.");
			return ANALYZE_RESULT(FAILED, 0);
		}

		analyze_push_front(cnt, entry_block);
	}

	if (fn->id) commit_fn(cnt, fn);

	if (fn_proto->first_unrechable_location) {
		builder_msg(BUILDER_MSG_WARNING,
		            0,
		            fn_proto->first_unrechable_location,
		            BUILDER_CUR_NONE,
		            "Unrechable code detected.");
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_cond_br(Context *cnt, MirInstrCondBr *br)
{
	BL_ASSERT(br->cond && br->then_block && br->else_block);
	BL_ASSERT(br->cond->analyzed);

	if (analyze_slot(cnt, &analyze_slot_conf_default, &br->cond, cnt->builtin_types.t_bool) !=
	    ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	/* PERFORMANCE: When condition is known in compile time, we can discard
	 * whole else/then block based on condition resutl. It is not possible
	 * because we don't have tracked down execution tree for now. */

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_br(Context *cnt, MirInstrBr *br)
{
	BL_ASSERT(br->then_block);
	return ANALYZE_RESULT(PASSED, 0);
}

/* True when switch contains same case value from start_from to the end of cases array. */
static inline bool
_analyze_switch_has_case_value(TSmallArray_SwitchCase *cases,
                               usize                   start_from,
                               MirInstr *              const_value)
{
	const s64 v = const_value->value.data.v_s64;
	for (usize i = start_from; i < cases->size; ++i) {
		MirSwitchCase *c  = &cases->data[i];
		const s64      cv = c->on_value->value.data.v_s64;

		if (v == cv) return true;
	}

	return false;
}

AnalyzeResult
analyze_instr_switch(Context *cnt, MirInstrSwitch *sw)
{
	if (analyze_slot(cnt, &analyze_slot_conf_basic, &sw->value, NULL) != ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	MirType *expected_case_type = sw->value->value.type;
	BL_ASSERT(expected_case_type);

	if (expected_case_type->kind != MIR_TYPE_INT && expected_case_type->kind != MIR_TYPE_ENUM) {
		builder_msg(
		    BUILDER_MSG_ERROR,
		    ERR_INVALID_TYPE,
		    sw->value->node->location,
		    BUILDER_CUR_WORD,
		    "Invalid type of switch expression. Only integer types and enums can be used.");

		return ANALYZE_RESULT(FAILED, 0);
	}

	if (!sw->cases->size) {
		builder_msg(BUILDER_MSG_WARNING,
		            0,
		            sw->base.node->location,
		            BUILDER_CUR_WORD,
		            "Empty switch statement.");

		return ANALYZE_RESULT(PASSED, 0);
	}

	MirSwitchCase *c;
	for (usize i = 0; i < sw->cases->size; ++i) {
		c = &sw->cases->data[i];

		if (!mir_is_comptime(&c->on_value->value)) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_EXPECTED_COMPTIME,
			            c->on_value->node->location,
			            BUILDER_CUR_WORD,
			            "Switch case value must be compile-time known.");
			return ANALYZE_RESULT(FAILED, 0);
		}

		if (analyze_slot(cnt, &analyze_slot_conf_basic, &c->on_value, expected_case_type) !=
		    ANALYZE_PASSED) {
			return ANALYZE_RESULT(FAILED, 0);
		}

		if (_analyze_switch_has_case_value(sw->cases, i + 1, c->on_value)) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_DUPLICIT_SWITCH_CASE,
			            c->on_value->node->location,
			            BUILDER_CUR_WORD,
			            "Switch already contains case for this value!");
		}
	}

	s64 expected_case_count = expected_case_type->kind == MIR_TYPE_ENUM
	                              ? expected_case_type->data.enm.variants->size
	                              : -1;

	if ((expected_case_count > (s64)sw->cases->size) && !sw->has_user_defined_default) {
		builder_msg(BUILDER_MSG_WARNING,
		            0,
		            sw->base.node->location,
		            BUILDER_CUR_WORD,
		            "Switch does not handle all possible enumerator values.");

		BL_ASSERT(expected_case_type->kind == MIR_TYPE_ENUM);
		MirVariant *variant;
		TSA_FOREACH(expected_case_type->data.enm.variants, variant)
		{
			bool hit = false;
			for (usize i = 0; i < sw->cases->size; ++i) {
				MirSwitchCase *c = &sw->cases->data[i];
				if (c->on_value->value.data.v_s64 == variant->value->data.v_s64) {
					hit = true;
					break;
				}
			}

			if (!hit) {
				builder_msg(BUILDER_MSG_NOTE,
				            0,
				            NULL,
				            BUILDER_CUR_NONE,
				            "Missing case for: %s",
				            variant->id->str);
			}
		}
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_load(Context *cnt, MirInstrLoad *load)
{
	MirInstr *src = load->src;
	BL_ASSERT(src);
	if (!mir_is_pointer_type(src->value.type)) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            src->node->location,
		            BUILDER_CUR_WORD,
		            "Expected pointer.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	MirType *type = mir_deref_type(src->value.type);
	BL_ASSERT(type);
	load->base.value.type = type;

	reduce_instr(cnt, src);
	load->base.value.eval_mode = src->value.eval_mode;

	/* INCOMPLETE: is this correct??? */
	/* INCOMPLETE: is this correct??? */
	/* INCOMPLETE: is this correct??? */
	load->base.value.addr_mode = src->value.addr_mode;
	// load->base.value.addr_mode = MIR_VAM_RVALUE;

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_type_fn(Context *cnt, MirInstrTypeFn *type_fn)
{
	BL_ASSERT(type_fn->base.value.type);
	BL_ASSERT(type_fn->ret_type ? type_fn->ret_type->analyzed : true);

	bool is_vargs = false;

	TSmallArray_ArgPtr *args = NULL;
	if (type_fn->args) {
		const usize argc = type_fn->args->size;
		args             = create_sarr(TSmallArray_ArgPtr, cnt->assembly);

		MirInstrDeclArg **arg_ref;
		MirArg *          arg;
		for (usize i = 0; i < argc; ++i) {
			BL_ASSERT(type_fn->args->data[i]->kind == MIR_INSTR_DECL_ARG);
			arg_ref = (MirInstrDeclArg **)&type_fn->args->data[i];
			BL_ASSERT(mir_is_comptime(&(*arg_ref)->base.value));

			if (analyze_slot(
			        cnt, &analyze_slot_conf_basic, (MirInstr **)arg_ref, NULL) !=
			    ANALYZE_PASSED) {
				return ANALYZE_RESULT(FAILED, 0);
			}

			arg = (*arg_ref)->arg;
			BL_ASSERT(arg);

			is_vargs = arg->type->kind == MIR_TYPE_VARGS;
			if (is_vargs && i != type_fn->args->size - 1) {
				builder_msg(
				    BUILDER_MSG_ERROR,
				    ERR_INVALID_TYPE,
				    arg->decl_node->location,
				    BUILDER_CUR_WORD,
				    "VArgs function argument must be last in argument list.");
			}

			tsa_push_ArgPtr(args, arg);
		}
	}

	MirType *ret_type = NULL;
	if (type_fn->ret_type) {
		if (analyze_slot(cnt, &analyze_slot_conf_basic, &type_fn->ret_type, NULL) !=
		    ANALYZE_PASSED) {
			return ANALYZE_RESULT(FAILED, 0);
		}

		BL_ASSERT(mir_is_comptime(&type_fn->ret_type->value));
		ret_type =
		    mir_get_const_ptr(MirType *, &type_fn->ret_type->value.data.v_ptr, MIR_CP_TYPE);
		BL_ASSERT(ret_type);
	}

	MirConstPtr *const_ptr = &type_fn->base.value.data.v_ptr;
	mir_set_const_ptr(
	    const_ptr, create_type_fn(cnt, NULL, ret_type, args, is_vargs), MIR_CP_FN);

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_decl_member(Context *cnt, MirInstrDeclMember *decl)
{
	if (analyze_slot(cnt, &analyze_slot_conf_basic, &decl->type, NULL) != ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	/* NOTE: Members will be provided by instr type struct because we need to
	 * know right ordering of members inside structure layout. (index and llvm
	 * element offet need to be calculated)*/
	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_decl_variant(Context *cnt, MirInstrDeclVariant *variant_instr)
{
	MirVariant *variant = variant_instr->variant;
	BL_ASSERT(variant && "Missing variant.");

	if (variant_instr->value) {
		/* User defined initialization value. */
		if (!mir_is_comptime(&variant_instr->value->value)) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_INVALID_EXPR,
			            variant_instr->value->node->location,
			            BUILDER_CUR_WORD,
			            "Enum variant value must be compile time known.");
			return ANALYZE_RESULT(FAILED, 0);
		}

		if (analyze_slot(cnt, &analyze_slot_conf_basic, &variant_instr->value, NULL) !=
		    ANALYZE_PASSED) {
			return ANALYZE_RESULT(FAILED, 0);
		}

		/* Setup value. */
		variant_instr->variant->value = &variant_instr->value->value;
	} else {
		/*
		 * CLENUP: Automatic initialization value is set in parser, mabye we will
		 * prefer to do automatic initialization here instead of doing so in parser
		 * pass.
		 */
		BL_UNIMPLEMENTED;
	}

	commit_variant(cnt, variant);

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_decl_arg(Context *cnt, MirInstrDeclArg *decl)
{
	if (analyze_slot(cnt, &analyze_slot_conf_basic, &decl->type, NULL) != ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	decl->arg->type = mir_get_const_ptr(MirType *, &decl->type->value.data.v_ptr, MIR_CP_TYPE);
	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_type_struct(Context *cnt, MirInstrTypeStruct *type_struct)
{
	TSmallArray_MemberPtr *members   = NULL;
	MirType *              base_type = NULL;

	if (type_struct->members) {
		MirInstr **         member_instr;
		MirInstrDeclMember *decl_member;
		MirType *           member_type;
		Scope *             scope = type_struct->scope;
		const usize         memc  = type_struct->members->size;

		members = create_sarr(TSmallArray_MemberPtr, cnt->assembly);

		for (usize i = 0; i < memc; ++i) {
			member_instr = &type_struct->members->data[i];

			if (analyze_slot(cnt, &analyze_slot_conf_basic, member_instr, NULL) !=
			    ANALYZE_PASSED) {
				return ANALYZE_RESULT(FAILED, 0);
			}

			decl_member = (MirInstrDeclMember *)*member_instr;
			BL_ASSERT(decl_member->base.kind == MIR_INSTR_DECL_MEMBER);
			BL_ASSERT(mir_is_comptime(&decl_member->base.value));

			/* solve member type */
			member_type = mir_get_const_ptr(
			    MirType *, &decl_member->type->value.data.v_ptr, MIR_CP_TYPE);

			if (member_type->kind == MIR_TYPE_FN) {
				builder_msg(BUILDER_MSG_ERROR,
				            ERR_INVALID_TYPE,
				            (*member_instr)->node->location,
				            BUILDER_CUR_WORD,
				            "Invalid type of the structure member, functions can "
				            "be referenced only by pointers.");
				return ANALYZE_RESULT(FAILED, 0);
			}

			BL_ASSERT(member_type);

			/* setup and provide member */
			MirMember *member = decl_member->member;
			BL_ASSERT(member);
			member->type       = member_type;
			member->decl_scope = scope;
			member->index      = (s64)i;

			if (member->is_base) {
				BL_ASSERT(!base_type &&
				          "Structure cannot have more than one base type!");
				base_type = member_type;
			}

			tsa_push_MemberPtr(members, member);
			commit_member(cnt, member);
		}
	}

	MirConstPtr *const_ptr   = &type_struct->base.value.data.v_ptr;
	MirType *    result_type = NULL;
	if (type_struct->fwd_decl) {
		/* Type has fwd declaration. In this case we set all desired information about
		 * struct type into previously created forward declaration. */
		result_type = complete_type_struct(cnt,
		                                   type_struct->fwd_decl,
		                                   type_struct->scope,
		                                   members,
		                                   base_type,
		                                   type_struct->is_packed);

		analyze_notify_provided(cnt, result_type->user_id->hash);
	} else {
		result_type = create_type_struct(cnt,
		                                 MIR_TYPE_STRUCT,
		                                 type_struct->id,
		                                 type_struct->scope,
		                                 members,
		                                 base_type,
		                                 type_struct->is_packed);
	}

	BL_ASSERT(result_type);
	mir_set_const_ptr(const_ptr, result_type, MIR_CP_TYPE);
	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_type_slice(Context *cnt, MirInstrTypeSlice *type_slice)
{
	BL_ASSERT(type_slice->elem_type);

	if (analyze_slot(cnt, &analyze_slot_conf_basic, &type_slice->elem_type, NULL) !=
	    ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	ID *id = NULL;
	if (type_slice->base.node && type_slice->base.node->kind == AST_IDENT) {
		id = &type_slice->base.node->data.ident.id;
	}

	if (type_slice->elem_type->value.type->kind != MIR_TYPE_TYPE) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            type_slice->elem_type->node->location,
		            BUILDER_CUR_WORD,
		            "Expected type.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	BL_ASSERT(mir_is_comptime(&type_slice->elem_type->value) && "This should be an error");
	MirType *elem_type =
	    mir_get_const_ptr(MirType *, &type_slice->elem_type->value.data.v_ptr, MIR_CP_TYPE);
	BL_ASSERT(elem_type);

	elem_type = create_type_ptr(cnt, elem_type);
	elem_type = create_type_struct_special(cnt, MIR_TYPE_SLICE, id, elem_type);

	{ /* set const pointer value */
		MirConstPtr *const_ptr = &type_slice->base.value.data.v_ptr;
		mir_set_const_ptr(const_ptr, elem_type, MIR_CP_TYPE);
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_type_vargs(Context *cnt, MirInstrTypeVArgs *type_vargs)
{
	MirType *elem_type = NULL;
	if (type_vargs->elem_type) {
		if (analyze_slot(cnt, &analyze_slot_conf_basic, &type_vargs->elem_type, NULL) !=
		    ANALYZE_PASSED) {
			return ANALYZE_RESULT(FAILED, 0);
		}

		if (type_vargs->elem_type->value.type->kind != MIR_TYPE_TYPE) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_INVALID_TYPE,
			            type_vargs->elem_type->node->location,
			            BUILDER_CUR_WORD,
			            "Expected type.");
			return ANALYZE_RESULT(FAILED, 0);
		}

		BL_ASSERT(mir_is_comptime(&type_vargs->elem_type->value) &&
		          "This should be an error");
		elem_type = type_vargs->elem_type->value.data.v_ptr.data.type;
	} else {
		/* use Any */
		elem_type = lookup_builtin(cnt, MIR_BUILTIN_ID_ANY);
		if (!elem_type)
			return ANALYZE_RESULT(WAITING, builtin_ids[MIR_BUILTIN_ID_ANY].hash);
	}

	BL_ASSERT(elem_type);

	elem_type = create_type_ptr(cnt, elem_type);
	elem_type = create_type_struct_special(cnt, MIR_TYPE_VARGS, NULL, elem_type);

	{ /* set const pointer value */
		MirConstPtr *const_ptr = &type_vargs->base.value.data.v_ptr;
		mir_set_const_ptr(const_ptr, elem_type, MIR_CP_TYPE);
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_type_array(Context *cnt, MirInstrTypeArray *type_arr)
{
	BL_ASSERT(type_arr->base.value.type);
	BL_ASSERT(type_arr->elem_type->analyzed);

	if (analyze_slot(
	        cnt, &analyze_slot_conf_default, &type_arr->len, cnt->builtin_types.t_s64) !=
	    ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	if (analyze_slot(cnt, &analyze_slot_conf_basic, &type_arr->elem_type, NULL) !=
	    ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	/* len */
	if (!mir_is_comptime(&type_arr->len->value)) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_EXPECTED_CONST,
		            type_arr->len->node->location,
		            BUILDER_CUR_WORD,
		            "Array size must be compile-time constant.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	if (type_arr->elem_type->value.type->kind != MIR_TYPE_TYPE) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            type_arr->elem_type->node->location,
		            BUILDER_CUR_WORD,
		            "Expected type.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	BL_ASSERT(mir_is_comptime(&type_arr->len->value) && "this must be error");
	reduce_instr(cnt, type_arr->len);

	const s64 len = type_arr->len->value.data.v_s64;
	if (len == 0) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_ARR_SIZE,
		            type_arr->len->node->location,
		            BUILDER_CUR_WORD,
		            "Array size cannot be 0.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	/* elem type */
	BL_ASSERT(mir_is_comptime(&type_arr->elem_type->value));
	reduce_instr(cnt, type_arr->elem_type);

	MirType *elem_type = type_arr->elem_type->value.data.v_ptr.data.type;
	BL_ASSERT(elem_type);

	elem_type = create_type_array(cnt, elem_type, len);

	{ /* set const pointer value */
		MirConstPtr *const_ptr = &type_arr->base.value.data.v_ptr;
		mir_set_const_ptr(const_ptr, elem_type, MIR_CP_TYPE);
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_type_enum(Context *cnt, MirInstrTypeEnum *type_enum)
{
	TSmallArray_InstrPtr *variant_instrs = type_enum->variants;
	Scope *               scope          = type_enum->scope;
	BL_ASSERT(variant_instrs);
	BL_ASSERT(scope);
	BL_ASSERT(variant_instrs->size);

	/*
	 * Validate and settup enum base type.
	 */
	MirType *base_type;
	if (type_enum->base_type) {
		reduce_instr(cnt, type_enum->base_type);
		base_type = type_enum->base_type->value.data.v_ptr.data.type;

		/* Enum type must be integer! */
		if (base_type->kind != MIR_TYPE_INT) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_INVALID_TYPE,
			            type_enum->base_type->node->location,
			            BUILDER_CUR_WORD,
			            "Base type of enumerator must be an integer type.");
			return ANALYZE_RESULT(FAILED, 0);
		}
	} else {
		/* Use s32 by default. */
		base_type = cnt->builtin_types.t_s32;
	}

	BL_ASSERT(base_type && "Invalid enum base type.");

	TSmallArray_VariantPtr *variants = create_sarr(TSmallArray_VariantPtr, cnt->assembly);

	/* Iterate over all enum variants and validate them. */
	MirInstr *  it;
	MirVariant *variant;

	TSA_FOREACH(variant_instrs, it)
	{
		MirInstrDeclVariant *variant_instr = (MirInstrDeclVariant *)it;
		variant                            = variant_instr->variant;
		BL_ASSERT(variant && "Missing variant.");

		if (analyze_slot(
		        cnt, &analyze_slot_conf_default, &variant_instr->value, base_type) !=
		    ANALYZE_PASSED) {
			return ANALYZE_RESULT(FAILED, 0);
		}

		reduce_instr(cnt, &variant_instr->base);

		tsa_push_VariantPtr(variants, variant);
	}

	MirType *enum_type = create_type_enum(cnt, type_enum->id, scope, base_type, variants);

	{ /* set const pointer value */
		MirConstPtr *const_ptr = &type_enum->base.value.data.v_ptr;
		mir_set_const_ptr(const_ptr, enum_type, MIR_CP_TYPE);
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_type_ptr(Context *cnt, MirInstrTypePtr *type_ptr)
{
	BL_ASSERT(type_ptr->type);

	if (analyze_slot(cnt, &analyze_slot_conf_basic, &type_ptr->type, NULL) != ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	BL_ASSERT(mir_is_comptime(&type_ptr->type->value));

	{ /* Target value must be a type. */
		MirType *src_type = type_ptr->type->value.type;
		BL_ASSERT(src_type);

		if (src_type->kind != MIR_TYPE_TYPE) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_INVALID_TYPE,
			            type_ptr->type->node->location,
			            BUILDER_CUR_WORD,
			            "Expected type name.");
			return ANALYZE_RESULT(FAILED, 0);
		}
	}

	// BL_ASSERT(type_ptr->type->value.data.v_ptr.kind == MIR_CP_TYPE);
	MirType *src_type_value = type_ptr->type->value.data.v_ptr.data.type;
	BL_ASSERT(src_type_value);

	if (src_type_value->kind == MIR_TYPE_TYPE) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            type_ptr->base.node->location,
		            BUILDER_CUR_WORD,
		            "Cannot create pointer to type.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	MirType *tmp = create_type_ptr(cnt, src_type_value);

	{ /* set const pointer value */
		MirConstPtr *const_ptr = &type_ptr->base.value.data.v_ptr;
		mir_set_const_ptr(const_ptr, tmp, MIR_CP_TYPE);
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_binop(Context *cnt, MirInstrBinop *binop)
{
	/******************************************************************************************/
#define is_valid(_type, _op)                                                                       \
	(((_type)->kind == MIR_TYPE_INT) || ((_type)->kind == MIR_TYPE_NULL) ||                    \
	 ((_type)->kind == MIR_TYPE_REAL) || ((_type)->kind == MIR_TYPE_PTR) ||                    \
	 ((_type)->kind == MIR_TYPE_BOOL && ast_binop_is_logic(_op)) ||                            \
	 ((_type)->kind == MIR_TYPE_ENUM && (_op == BINOP_EQ || _op == BINOP_NEQ)))
	/******************************************************************************************/

	{ /* Handle type propagation. */
		MirType *lhs_type = binop->lhs->value.type;
		MirType *rhs_type = binop->rhs->value.type;

		if (is_load_needed(binop->lhs)) lhs_type = mir_deref_type(lhs_type);
		if (is_load_needed(binop->rhs)) rhs_type = mir_deref_type(rhs_type);

		const bool lhs_is_null = binop->lhs->value.type->kind == MIR_TYPE_NULL;
		const bool lhs_is_const_int =
		    binop->lhs->kind == MIR_INSTR_CONST && lhs_type->kind == MIR_TYPE_INT;
		const bool can_propagate_LtoR =
		    can_impl_cast(lhs_type, rhs_type) || lhs_is_const_int;

		/*
		char type_nameL[256];
		mir_type_to_str(type_nameL, 256, lhs_type, true);
		char type_nameR[256];
		mir_type_to_str(type_nameR, 256, rhs_type, true);
		*/

		if (can_propagate_LtoR) {
			if (analyze_slot(cnt, &analyze_slot_conf_default, &binop->lhs, rhs_type) !=
			    ANALYZE_PASSED)
				return ANALYZE_RESULT(FAILED, 0);

			if (analyze_slot(cnt, &analyze_slot_conf_basic, &binop->rhs, NULL) !=
			    ANALYZE_PASSED)
				return ANALYZE_RESULT(FAILED, 0);
		} else {
			if (analyze_slot(cnt, &analyze_slot_conf_basic, &binop->lhs, NULL) !=
			    ANALYZE_PASSED)
				return ANALYZE_RESULT(FAILED, 0);

			if (analyze_slot(
			        cnt,
			        lhs_is_null ? &analyze_slot_conf_basic : &analyze_slot_conf_default,
			        &binop->rhs,
			        lhs_is_null ? NULL : binop->lhs->value.type) != ANALYZE_PASSED)
				return ANALYZE_RESULT(FAILED, 0);

			if (lhs_is_null) {
				if (analyze_stage_set_null(
				        cnt, &binop->lhs, binop->rhs->value.type) !=
				    ANALYZE_STAGE_BREAK)
					return ANALYZE_RESULT(FAILED, 0);
			}
		}
	}

	MirInstr *lhs = binop->lhs;
	MirInstr *rhs = binop->rhs;
	BL_ASSERT(lhs && rhs);
	BL_ASSERT(lhs->analyzed);
	BL_ASSERT(rhs->analyzed);

	const bool lhs_valid = is_valid(lhs->value.type, binop->op);
	const bool rhs_valid = is_valid(rhs->value.type, binop->op);

	if (!(lhs_valid && rhs_valid)) {
		error_types(lhs->value.type,
		            rhs->value.type,
		            binop->base.node,
		            "invalid operation for %s type");
		return ANALYZE_RESULT(FAILED, 0);
	}

	MirType *type = ast_binop_is_logic(binop->op) ? cnt->builtin_types.t_bool : lhs->value.type;
	BL_ASSERT(type);
	binop->base.value.type = type;

	/* when binary operation has lhs and rhs values known in compile it is known
	 * in compile time also
	 */
	binop->base.value.eval_mode = mir_is_comptime(&lhs->value) && mir_is_comptime(&rhs->value)
	                                  ? MIR_VEM_STATIC
	                                  : MIR_VEM_RUNTIME;

	/* INCOMPLETE: I'm not sure if every binary operation is rvalue... */
	/* INCOMPLETE: I'm not sure if every binary operation is rvalue... */
	/* INCOMPLETE: I'm not sure if every binary operation is rvalue... */
	binop->base.value.addr_mode = MIR_VAM_RVALUE;
	binop->volatile_type        = is_instr_type_volatile(lhs) && is_instr_type_volatile(rhs);

	return ANALYZE_RESULT(PASSED, 0);
#undef is_valid
}

AnalyzeResult
analyze_instr_unop(Context *cnt, MirInstrUnop *unop)
{
	MirType *type = unop->expr->value.type;
	if (analyze_slot(cnt, &analyze_slot_conf_basic, &unop->expr, NULL) != ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	BL_ASSERT(unop->expr && unop->expr->analyzed);
	type = unop->expr->value.type;
	BL_ASSERT(type);
	unop->base.value.type = type;

	unop->base.value.eval_mode = unop->expr->value.eval_mode;
	unop->volatile_type        = is_instr_type_volatile(unop->expr);

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_const(Context *cnt, MirInstrConst *cnst)
{
	BL_ASSERT(cnst->base.value.type);
	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_ret(Context *cnt, MirInstrRet *ret)
{
	/* compare return value with current function type */
	MirInstrBlock *block = ret->base.owner_block;
	if (!block->terminal) block->terminal = &ret->base;

	MirType *fn_type = get_current_fn(cnt)->type;
	BL_ASSERT(fn_type);
	BL_ASSERT(fn_type->kind == MIR_TYPE_FN);

	if (ret->value) {
		const AnalyzeSlotConfig *conf =
		    ret->infer_type ? &analyze_slot_conf_basic : &analyze_slot_conf_default;
		if (analyze_slot(cnt,
		                 conf,
		                 &ret->value,
		                 ret->infer_type ? NULL : fn_type->data.fn.ret_type) !=
		    ANALYZE_PASSED) {
			return ANALYZE_RESULT(FAILED, 0);
		}
	}

	MirInstr *value = ret->value;
	if (value) {
		BL_ASSERT(value->analyzed);
	}

	if (ret->infer_type) {
		/* return is supposed to override function return type */
		if (ret->value) {
			BL_ASSERT(ret->value->value.type);
			if (fn_type->data.fn.ret_type != ret->value->value.type) {
				MirFn *fn = get_current_fn(cnt);
				BL_ASSERT(fn);
				fn->type = create_type_fn(cnt,
				                          NULL,
				                          ret->value->value.type,
				                          fn_type->data.fn.args,
				                          fn_type->data.fn.is_vargs);
				fn_type  = fn->type;
				/* HACK: Function type need to be set also for function
				 * prototype instruction, this is by the way only reason why
				 * we need poinetr to prototype inside MirFn. Better
				 * solution should be possible. */
				fn->prototype->value.type = fn_type;
			}
		}
	}

	const bool expected_ret_value =
	    !type_cmp(fn_type->data.fn.ret_type, cnt->builtin_types.t_void);

	/* return value is not expected, and it's not provided */
	if (!expected_ret_value && !value) {
		return ANALYZE_RESULT(PASSED, 0);
	}

	/* return value is expected, but it's not provided */
	if (expected_ret_value && !value) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_EXPR,
		            ret->base.node->location,
		            BUILDER_CUR_AFTER,
		            "Expected return value.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	/* return value is not expected, but it's provided */
	if (!expected_ret_value && value) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_EXPR,
		            ret->value->node->location,
		            BUILDER_CUR_WORD,
		            "Unexpected return value.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_decl_var(Context *cnt, MirInstrDeclVar *decl)
{
	MirVar *var = decl->var;
	BL_ASSERT(var);

	if (decl->type && var->value.type == NULL) {
		AnalyzeResult result = analyze_resolve_type(cnt, decl->type, &var->value.type);
		if (result.state != ANALYZE_PASSED) return result;
	}

	if (var->is_in_gscope) { // global variable
		/* All globals must be initialized. */
		if (!decl->init) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_UNINITIALIZED,
			            decl->base.node->location,
			            BUILDER_CUR_WORD,
			            "All globals must be initialized.");
			return ANALYZE_RESULT(FAILED, 0);
		}

		/* Global initializer must be compile time known. */
		if (!mir_is_comptime(&decl->init->value)) {
			builder_msg(
			    BUILDER_MSG_ERROR,
			    ERR_EXPECTED_COMPTIME,
			    decl->init->node->location,
			    BUILDER_CUR_WORD,
			    "Global variables must be initialized with compile time known value.");
			return ANALYZE_RESULT(FAILED, 0);
		}

		if (decl->init->kind == MIR_INSTR_CALL) {
			/* Initialized by call to value resolver function. */
			/* Just to be sure we have call instruction. */
			BL_ASSERT(decl->init->kind == MIR_INSTR_CALL &&
			          "Global initializer is supposed to be comptime implicit call.");

			/* Since all globals are initialized by call to comptime function and no
			 * type infer is needed (user specified expected type directly), we must
			 * disable type infering of terminal instruction in initializer and set
			 * exact type we are expecting to be returned by the initializer function.
			 */
			if (var->value.type) {
				MirInstrCall *initializer_call = (MirInstrCall *)decl->init;
				MirFn *       fn               = get_callee(initializer_call);
				MirInstrRet * terminal         = fn->terminal_instr;
				BL_ASSERT(terminal);

				if (terminal->infer_type) {
					terminal->infer_type = false;
					fn->type =
					    create_type_fn(cnt, NULL, var->value.type, NULL, false);
					fn->prototype->value.type = fn->type;
				}
			}

			/* Analyze and execute initializer. This could lead to POSTPONE when
			 * initializer function is not ready yet. */
			AnalyzeResult result = analyze_instr(cnt, decl->init);
			if (result.state != ANALYZE_PASSED) return result;

			/* All globals are initialized with comptime value, we can use evaluation
			 * here instead of VM execution. */
			vm_eval_comptime_instr(&cnt->vm, decl->init);

		} else { /* Initialized by constant value. */
			BL_ASSERT(decl->init->kind == MIR_INSTR_CONST &&
			          "Initializer of global value must be either comptime function "
			          "call or constant instruction.");

			BL_ASSERT(decl->init->analyzed &&
			          "Global variable const initializer is not analyzed!");
		}

		/* Infer type if needed */
		if (!var->value.type) {
			var->value.type = decl->init->value.type;
		}

		/* Global variable is comptime when it's immutable and initialization value is
		 * comptime. Existence of initialization value is guaranteed by previous error
		 * checking.  */
		var->is_comptime = !var->is_mutable && mir_is_comptime(&decl->init->value);
	} else { // local variable
		if (decl->init) {
			if (var->value.type) {
				if (analyze_slot(cnt,
				                 &analyze_slot_conf_default,
				                 &decl->init,
				                 var->value.type) != ANALYZE_PASSED) {
					return ANALYZE_RESULT(FAILED, 0);
				}
			} else {
				if (analyze_slot(
				        cnt, &analyze_slot_conf_basic, &decl->init, NULL) !=
				    ANALYZE_PASSED) {
					return ANALYZE_RESULT(FAILED, 0);
				}

				/* infer type */
				MirType *type = decl->init->value.type;
				BL_ASSERT(type);
				if (type->kind == MIR_TYPE_NULL) type = type->data.null.base_type;
				var->value.type = type;
			}

			/* Locals can be comtime only if they are immutable and initialization value
			 * is comptime. */
			var->is_comptime = !var->is_mutable && mir_is_comptime(&decl->init->value);
		} else {
			BL_ASSERT(var->is_mutable &&
			          "Immutable local variable declaration without initializer value "
			          "is illegal and should be handled in parser.");
			var->is_comptime = !var->is_mutable;
		}
	}

	if (!var->value.type) {
		BL_ABORT("unknown declaration type");
	}

	if (var->value.type->kind == MIR_TYPE_TYPE && var->is_mutable) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_MUTABILITY,
		            decl->base.node->location,
		            BUILDER_CUR_WORD,
		            "Type declaration must be immutable.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	if (var->value.type->kind == MIR_TYPE_FN) {
		/* Allocated type is function. */
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            decl->base.node->location,
		            BUILDER_CUR_WORD,
		            "Invalid type of the variable, functions can be referenced "
		            "only by pointers.");
		return ANALYZE_RESULT(FAILED, 0);
	} else if (var->value.type->kind == MIR_TYPE_VOID) {
		/* Allocated type is void type. */
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            decl->base.node->location,
		            BUILDER_CUR_WORD,
		            "Cannot allocate unsized type.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	if (decl->base.ref_count == 0) {
		builder_msg(BUILDER_MSG_WARNING,
		            0,
		            decl->base.node->location,
		            BUILDER_CUR_WORD,
		            "Unused declaration.");
	}

	reduce_instr(cnt, decl->init);

	if (var->is_comptime && decl->init) {
		BL_ASSERT(mir_is_comptime(&decl->init->value));
		/* Variable can be initialized when it's comptime. */
		var->value = decl->init->value;
	}

	if (!decl->var->is_implicit) commit_var(cnt, decl->var);

	/* Type declaration should not be generated in LLVM. */
	var->gen_llvm = var->value.type->kind != MIR_TYPE_TYPE;

	if (var->is_in_gscope && !var->is_comptime) {
		/* Global varibales which are not compile time constants are allocated
		 * on the stack, one option is to do allocation every time when we
		 * invoke comptime function execution, but we don't know which globals
		 * will be used by function and we also don't known whatever function
		 * has some side effect or not. So we produce allocation here. Variable
		 * will be stored in static data segment. There is no need to use
		 * relative pointers here. */
		vm_create_global(&cnt->vm, decl);
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_call(Context *cnt, MirInstrCall *call)
{
	BL_ASSERT(call->callee);

	/*
	 * Direct call is call without any reference lookup, usually call to anonymous
	 * function, type resolver or variable initializer. Contant value of callee
	 * instruction must containt pointer to the MirFn object.
	 */
	const MirInstrKind callee_kind = call->callee->kind;
	const bool         is_comptime = mir_is_comptime(&call->base.value);
	const bool         is_direct_call =
	    callee_kind != MIR_INSTR_DECL_REF && callee_kind != MIR_INSTR_MEMBER_PTR;

	/* callee has not been analyzed yet -> postpone call analyze */
	if (!call->callee->analyzed) {
		BL_ASSERT(call->callee->kind == MIR_INSTR_FN_PROTO);
		MirInstrFnProto *fn_proto = (MirInstrFnProto *)call->callee;
		if (!fn_proto->pushed_for_analyze) {
			fn_proto->pushed_for_analyze = true;
			analyze_push_back(cnt, call->callee);
		}
		return ANALYZE_RESULT(POSTPONE, 0);
	}

	if (analyze_slot(cnt, &analyze_slot_conf_basic, &call->callee, NULL) != ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	MirType *type = call->callee->value.type;
	BL_ASSERT(type && "invalid type of called object");

	if (mir_is_pointer_type(type)) {
		/* we want to make calls also via pointer to functions so in such case
		 * we need to resolve pointed function */
		type = mir_deref_type(type);
	}

	if (type->kind != MIR_TYPE_FN) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_EXPECTED_FUNC,
		            call->callee->node->location,
		            BUILDER_CUR_WORD,
		            "Expected a function name.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	if (is_direct_call) {
		MirFn *fn = mir_get_const_ptr(MirFn *, &call->callee->value.data.v_ptr, MIR_CP_FN);
		BL_ASSERT(fn && "Missing function reference for direct call!");
		if (mir_is_comptime(&call->base.value)) {
			if (!fn->fully_analyzed) return ANALYZE_RESULT(POSTPONE, 0);
		} else if (call->callee->kind == MIR_INSTR_FN_PROTO) {
			/* Direct call of anonymous function. */
			fn->emit_llvm = true;
		}
	}

	MirType *result_type = type->data.fn.ret_type;
	BL_ASSERT(result_type && "invalid type of call result");
	call->base.value.type = result_type;

	if (is_comptime) {
		/* Adjust evaluation mode for comptime call. By default comptime call has static
		 * evaluation mode but it could be changed to lazy. */
		call->base.value.eval_mode = choose_eval_mode_for_comptime(result_type);
	} else {
		call->base.value.eval_mode =
		    result_type->kind == MIR_TYPE_VOID ? MIR_VEM_NONE : MIR_VEM_RUNTIME;
	}

	/* validate arguments */
	const bool is_vargs = type->data.fn.is_vargs;

	usize       callee_argc = type->data.fn.args ? type->data.fn.args->size : 0;
	const usize call_argc   = call->args ? call->args->size : 0;

	if (is_vargs) {
		/* This is gonna be tricky... */
		--callee_argc;
		if ((call_argc < callee_argc)) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_INVALID_ARG_COUNT,
			            call->base.node->location,
			            BUILDER_CUR_WORD,
			            "Expected at least %u %s, but called with %u.",
			            callee_argc,
			            callee_argc == 1 ? "argument" : "arguments",
			            call_argc);
			return ANALYZE_RESULT(FAILED, 0);
		}

		MirType *vargs_type = mir_get_fn_arg_type(type, (u32)callee_argc);
		BL_ASSERT(vargs_type->kind == MIR_TYPE_VARGS && "VArgs is expected to be last!!!");

		vargs_type = mir_get_struct_elem_type(vargs_type, 1);
		BL_ASSERT(vargs_type && mir_is_pointer_type(vargs_type));

		vargs_type = mir_deref_type(vargs_type);

		/* Prepare vargs values. */
		const usize           vargsc = call_argc - callee_argc;
		TSmallArray_InstrPtr *values = create_sarr(TSmallArray_InstrPtr, cnt->assembly);
		MirInstr *            vargs  = create_instr_vargs_impl(cnt, vargs_type, values);
		ref_instr(vargs);

		if (vargsc > 0) {
			/* One or more vargs passed. */
			// INCOMPLETE: check it this is ok!!!
			// INCOMPLETE: check it this is ok!!!
			// INCOMPLETE: check it this is ok!!!
			// INCOMPLETE: check it this is ok!!!
			for (usize i = 0; i < vargsc; ++i) {
				tsa_push_InstrPtr(values, call->args->data[callee_argc + i]);
			}

			MirInstr *insert_loc = call->args->data[callee_argc];
			insert_instr_after(insert_loc, vargs);
		} else if (callee_argc > 0) {
			/* No arguments passed into vargs but there are more regular
			 * arguments before vargs. */
			MirInstr *insert_loc = call->args->data[0];
			insert_instr_before(insert_loc, vargs);
		} else {
			insert_instr_before(&call->base, vargs);
		}

		if (analyze_instr_vargs(cnt, (MirInstrVArgs *)vargs).state != ANALYZE_PASSED)
			return ANALYZE_RESULT(FAILED, 0);

		vargs->analyzed = true;

		/* Erase vargs from arguments. */
		tsa_resize_InstrPtr(call->args, callee_argc);

		/* Replace last with vargs. */
		tsa_push_InstrPtr(call->args, vargs);
	} else {
		if ((callee_argc != call_argc)) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_INVALID_ARG_COUNT,
			            call->base.node->location,
			            BUILDER_CUR_WORD,
			            "Expected %u %s, but called with %u.",
			            callee_argc,
			            callee_argc == 1 ? "argument" : "arguments",
			            call_argc);
			return ANALYZE_RESULT(FAILED, 0);
		}
	}

	/* validate argument types */
	if (callee_argc) {
		MirInstr **call_arg;
		MirArg *   callee_arg;
		bool       valid = true;

		for (u32 i = 0; i < callee_argc && valid; ++i) {
			call_arg   = &call->args->data[i];
			callee_arg = type->data.fn.args->data[i];
			BL_ASSERT(callee_arg);

			if (analyze_slot(
			        cnt, &analyze_slot_conf_full, call_arg, callee_arg->type) !=
			    ANALYZE_PASSED) {
				return ANALYZE_RESULT(FAILED, 0);
			}
		}
	}

	// call->base.value.eval_mode =

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_store(Context *cnt, MirInstrStore *store)
{
	MirInstr *dest = store->dest;
	BL_ASSERT(dest);
	BL_ASSERT(dest->analyzed);

	if (!mir_is_pointer_type(dest->value.type)) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_EXPR,
		            store->base.node->location,
		            BUILDER_CUR_WORD,
		            "Left hand side of the expression cannot be assigned.");
		return ANALYZE_RESULT(FAILED, 0);
	}

	if (dest->value.addr_mode == MIR_VAM_LVALUE_CONST) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_INVALID_EXPR,
		            store->base.node->location,
		            BUILDER_CUR_WORD,
		            "Cannot assign to constant.");
	}

	MirType *dest_type = mir_deref_type(dest->value.type);
	BL_ASSERT(dest_type && "store destination has invalid base type");

	if (analyze_slot(cnt, &analyze_slot_conf_default, &store->src, dest_type) !=
	    ANALYZE_PASSED) {
		return ANALYZE_RESULT(FAILED, 0);
	}

	reduce_instr(cnt, store->dest);

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeResult
analyze_instr_block(Context *cnt, MirInstrBlock *block)
{
	BL_ASSERT(block);

	MirFn *fn = block->owner_fn;
	BL_ASSERT(fn);
	MirInstrFnProto *fn_proto = (MirInstrFnProto *)fn->prototype;

	/* append implicit return for void functions or generate error when last
	 * block is not terminated
	 */
	if (!is_block_terminated(block)) {
		if (fn->type->data.fn.ret_type->kind == MIR_TYPE_VOID) {
			set_current_block(cnt, block);
			append_instr_ret(cnt, NULL, NULL, false);
		} else {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_MISSING_RETURN,
			            fn->decl_node->location,
			            BUILDER_CUR_WORD,
			            "Not every path inside function return value.");
		}
	}

	if (block->base.ref_count == 0 && !fn_proto->first_unrechable_location) {
		MirInstr *first_instr = block->entry_instr;
		if (first_instr && first_instr->node) {
			fn_proto->first_unrechable_location = first_instr->node->location;

			builder_msg(BUILDER_MSG_WARNING,
			            0,
			            fn_proto->first_unrechable_location,
			            BUILDER_CUR_NONE,
			            "Unrechable code detected.");
		}
	}

	return ANALYZE_RESULT(PASSED, 0);
}

AnalyzeState
analyze_slot(Context *cnt, const AnalyzeSlotConfig *conf, MirInstr **input, MirType *slot_type)
{
	AnalyzeStageState state;
	for (s32 i = 0; i < conf->count; ++i) {
		state = conf->stages[i](cnt, input, slot_type);
		switch (state) {
		case ANALYZE_STAGE_BREAK:
			goto DONE;
		case ANALYZE_STAGE_FAILED:
			goto FAILED;
		case ANALYZE_STAGE_CONTINUE:
			break;
		}
	}

DONE:
	reduce_instr(cnt, *input);
	return ANALYZE_PASSED;
FAILED:
	return ANALYZE_FAILED;
}

AnalyzeStageState
analyze_stage_load(Context *cnt, MirInstr **input, MirType *slot_type)
{
	if (is_load_needed(*input)) {
		*input = insert_instr_load(cnt, *input);
	}

	return ANALYZE_STAGE_CONTINUE;
}

AnalyzeStageState
analyze_stage_set_null(Context *cnt, MirInstr **input, MirType *slot_type)
{
	BL_ASSERT(slot_type);
	MirInstr *_input = *input;

	if (_input->kind != MIR_INSTR_CONST) return ANALYZE_STAGE_CONTINUE;
	if (_input->value.type->kind != MIR_TYPE_NULL) return ANALYZE_STAGE_CONTINUE;

	if (slot_type->kind == MIR_TYPE_NULL) {
		_input->value.type = slot_type;
		return ANALYZE_STAGE_BREAK;
	}

	if (mir_is_pointer_type(slot_type)) {
		_input->value.type = create_type_null(cnt, slot_type);
		return ANALYZE_STAGE_BREAK;
	}

	builder_msg(BUILDER_MSG_ERROR,
	            ERR_INVALID_TYPE,
	            _input->node->location,
	            BUILDER_CUR_WORD,
	            "Invalid use of null constant.");

	return ANALYZE_STAGE_FAILED;
}

AnalyzeStageState
analyze_stage_set_auto(Context *cnt, MirInstr **input, MirType *slot_type)
{
	BL_ASSERT(slot_type);
	MirInstr *_input = *input;

	if (_input->kind != MIR_INSTR_CAST) return ANALYZE_STAGE_CONTINUE;
	if (!((MirInstrCast *)_input)->auto_cast) return ANALYZE_STAGE_CONTINUE;

	_input->value.type = slot_type;
	if (analyze_instr_cast(cnt, (MirInstrCast *)_input, true).state != ANALYZE_PASSED) {
		return ANALYZE_STAGE_FAILED;
	}

	return ANALYZE_STAGE_BREAK;
}

AnalyzeStageState
analyze_stage_toany(Context *cnt, MirInstr **input, MirType *slot_type)
{
	BL_ASSERT(slot_type);

	/* check any */
	if (!is_to_any_needed(cnt, *input, slot_type)) return ANALYZE_STAGE_CONTINUE;

	*input = insert_instr_toany(cnt, *input);
	*input = insert_instr_load(cnt, *input);

	return ANALYZE_STAGE_BREAK;
}

AnalyzeStageState
analyze_stage_set_volatile_expr(Context *cnt, MirInstr **input, MirType *slot_type)
{
	BL_ASSERT(slot_type);
	if (slot_type->kind != MIR_TYPE_INT) return ANALYZE_STAGE_CONTINUE;
	if (!is_instr_type_volatile(*input)) return ANALYZE_STAGE_CONTINUE;

	(*input)->value.type = slot_type;
	return ANALYZE_STAGE_BREAK;
}

AnalyzeStageState
analyze_stage_implicit_cast(Context *cnt, MirInstr **input, MirType *slot_type)
{
	if (type_cmp((*input)->value.type, slot_type)) return ANALYZE_STAGE_BREAK;
	if (!can_impl_cast((*input)->value.type, slot_type)) return ANALYZE_STAGE_CONTINUE;

	*input = insert_instr_cast(cnt, *input, slot_type);
	return ANALYZE_STAGE_BREAK;
}

AnalyzeStageState
analyze_stage_report_type_mismatch(Context *cnt, MirInstr **input, MirType *slot_type)
{
	error_types((*input)->value.type, slot_type, (*input)->node, NULL);
	return ANALYZE_STAGE_CONTINUE;
}

AnalyzeResult
analyze_instr(Context *cnt, MirInstr *instr)
{
	if (!instr) return ANALYZE_RESULT(PASSED, 0);

	/* skip already analyzed instructions */
	if (instr->analyzed) return ANALYZE_RESULT(PASSED, 0);
	AnalyzeResult state = ANALYZE_RESULT(PASSED, 0);

	if (instr->owner_block) set_current_block(cnt, instr->owner_block);

	switch (instr->kind) {
	case MIR_INSTR_VARGS:
	case MIR_INSTR_INVALID:
		break;

	case MIR_INSTR_BLOCK:
		state = analyze_instr_block(cnt, (MirInstrBlock *)instr);
		break;
	case MIR_INSTR_FN_PROTO:
		state = analyze_instr_fn_proto(cnt, (MirInstrFnProto *)instr);
		break;
	case MIR_INSTR_DECL_VAR:
		state = analyze_instr_decl_var(cnt, (MirInstrDeclVar *)instr);
		break;
	case MIR_INSTR_DECL_MEMBER:
		state = analyze_instr_decl_member(cnt, (MirInstrDeclMember *)instr);
		break;
	case MIR_INSTR_DECL_VARIANT:
		state = analyze_instr_decl_variant(cnt, (MirInstrDeclVariant *)instr);
		break;
	case MIR_INSTR_DECL_ARG:
		state = analyze_instr_decl_arg(cnt, (MirInstrDeclArg *)instr);
		break;
	case MIR_INSTR_CALL:
		state = analyze_instr_call(cnt, (MirInstrCall *)instr);
		break;
	case MIR_INSTR_CONST:
		state = analyze_instr_const(cnt, (MirInstrConst *)instr);
		break;
	case MIR_INSTR_RET:
		state = analyze_instr_ret(cnt, (MirInstrRet *)instr);
		break;
	case MIR_INSTR_STORE:
		state = analyze_instr_store(cnt, (MirInstrStore *)instr);
		break;
	case MIR_INSTR_DECL_REF:
		state = analyze_instr_decl_ref(cnt, (MirInstrDeclRef *)instr);
		break;
	case MIR_INSTR_BINOP:
		state = analyze_instr_binop(cnt, (MirInstrBinop *)instr);
		break;
	case MIR_INSTR_TYPE_FN:
		state = analyze_instr_type_fn(cnt, (MirInstrTypeFn *)instr);
		break;
	case MIR_INSTR_TYPE_STRUCT:
		state = analyze_instr_type_struct(cnt, (MirInstrTypeStruct *)instr);
		break;
	case MIR_INSTR_TYPE_SLICE:
		state = analyze_instr_type_slice(cnt, (MirInstrTypeSlice *)instr);
		break;
	case MIR_INSTR_TYPE_VARGS:
		state = analyze_instr_type_vargs(cnt, (MirInstrTypeVArgs *)instr);
		break;
	case MIR_INSTR_TYPE_ARRAY:
		state = analyze_instr_type_array(cnt, (MirInstrTypeArray *)instr);
		break;
	case MIR_INSTR_TYPE_PTR:
		state = analyze_instr_type_ptr(cnt, (MirInstrTypePtr *)instr);
		break;
	case MIR_INSTR_TYPE_ENUM:
		state = analyze_instr_type_enum(cnt, (MirInstrTypeEnum *)instr);
		break;
	case MIR_INSTR_LOAD:
		state = analyze_instr_load(cnt, (MirInstrLoad *)instr);
		break;
	case MIR_INSTR_COMPOUND:
		state = analyze_instr_compound(cnt, (MirInstrCompound *)instr);
		break;
	case MIR_INSTR_BR:
		state = analyze_instr_br(cnt, (MirInstrBr *)instr);
		break;
	case MIR_INSTR_COND_BR:
		state = analyze_instr_cond_br(cnt, (MirInstrCondBr *)instr);
		break;
	case MIR_INSTR_UNOP:
		state = analyze_instr_unop(cnt, (MirInstrUnop *)instr);
		break;
	case MIR_INSTR_UNREACHABLE:
		state = analyze_instr_unreachable(cnt, (MirInstrUnreachable *)instr);
		break;
	case MIR_INSTR_ARG:
		state = analyze_instr_arg(cnt, (MirInstrArg *)instr);
		break;
	case MIR_INSTR_ELEM_PTR:
		state = analyze_instr_elem_ptr(cnt, (MirInstrElemPtr *)instr);
		break;
	case MIR_INSTR_MEMBER_PTR:
		state = analyze_instr_member_ptr(cnt, (MirInstrMemberPtr *)instr);
		break;
	case MIR_INSTR_ADDROF:
		state = analyze_instr_addrof(cnt, (MirInstrAddrof *)instr);
		break;
	case MIR_INSTR_CAST:
		state = analyze_instr_cast(cnt, (MirInstrCast *)instr, false);
		break;
	case MIR_INSTR_SIZEOF:
		state = analyze_instr_sizeof(cnt, (MirInstrSizeof *)instr);
		break;
	case MIR_INSTR_ALIGNOF:
		state = analyze_instr_alignof(cnt, (MirInstrAlignof *)instr);
		break;
	case MIR_INSTR_TYPE_INFO:
		state = analyze_instr_type_info(cnt, (MirInstrTypeInfo *)instr);
		break;
	case MIR_INSTR_PHI:
		state = analyze_instr_phi(cnt, (MirInstrPhi *)instr);
		break;
	case MIR_INSTR_TOANY:
		state = analyze_instr_toany(cnt, (MirInstrToAny *)instr);
		break;
	case MIR_INSTR_DECL_DIRECT_REF:
		state = analyze_instr_decl_direct_ref(cnt, (MirInstrDeclDirectRef *)instr);
		break;
	case MIR_INSTR_SWITCH:
		state = analyze_instr_switch(cnt, (MirInstrSwitch *)instr);
		break;
	}

	if (state.state != ANALYZE_PASSED) return state;

	instr->analyzed = true;
	return state;
}

static inline MirInstr *
analyze_try_get_next(MirInstr *instr)
{
	if (!instr) return NULL;
	if (instr->kind == MIR_INSTR_BLOCK) {
		MirInstrBlock *block = (MirInstrBlock *)instr;
		return block->entry_instr;
	}

	/* Instruction can be the last instruction inside block, but block may not
	 * be the last block inside function, we try to get following one. */
	MirInstrBlock *owner_block = instr->owner_block;
	if (owner_block && instr == owner_block->last_instr) {
		if (owner_block->base.next == NULL) {
			/* Instruction is last instruction of the function body, so the
			 * function can be executed in compile time if needed, we need to
			 * set flag with this information here. */
			owner_block->owner_fn->fully_analyzed = true;
#if BL_DEBUG && VERBOSE_ANALYZE
			printf("Analyze: " BLUE("Function '%s' completely analyzed.\n"),
			       owner_block->owner_fn->linkage_name);
#endif
		}

		/* Return following block. */
		return owner_block->base.next;
	}

	return instr->next;
}

void
analyze(Context *cnt)
{
	/******************************************************************************************/
#if BL_DEBUG && VERBOSE_ANALYZE
#define LOG_ANALYZE_PASSED printf("Analyze: [ " GREEN("PASSED") " ] %16s\n", mir_instr_name(ip));

#define LOG_ANALYZE_FAILED printf("Analyze: [ " RED("FAILED") " ] %16s\n", mir_instr_name(ip));

#define LOG_ANALYZE_POSTPONE                                                                       \
	printf("Analyze: [" MAGENTA("POSTPONE") "] %16s\n", mir_instr_name(ip));
#define LOG_ANALYZE_WAITING                                                                        \
	printf("Analyze: [  " YELLOW("WAIT") "  ] %16s is waiting for: '%llu'\n",                  \
	       mir_instr_name(ip),                                                                 \
	       (unsigned long long)result.waiting_for);
#else
#define LOG_ANALYZE_PASSED
#define LOG_ANALYZE_FAILED
#define LOG_ANALYZE_POSTPONE
#define LOG_ANALYZE_WAITING
#endif
	/******************************************************************************************/

	if (cnt->analyze.verbose_pre) {
		MirInstr *instr;
		TArray *  globals = &cnt->assembly->MIR.global_instrs;
		TARRAY_FOREACH(MirInstr *, globals, instr)
		{
			mir_print_instr(instr, stdout);
		}
	}

	/* PERFORMANCE: use array??? */
	/* PERFORMANCE: use array??? */
	/* PERFORMANCE: use array??? */
	TList *       q = &cnt->analyze.queue;
	AnalyzeResult result;
	usize         postpone_loop_count = 0;
	MirInstr *    ip                  = NULL;
	MirInstr *    prev_ip             = NULL;
	bool          skip                = false;

	if (tlist_empty(q)) return;

	while (true) {
		prev_ip = ip;
		ip      = skip ? NULL : analyze_try_get_next(ip);

		if (prev_ip && prev_ip->analyzed) {
			erase_instr_tree(prev_ip);
		}

		if (!ip) {
			if (tlist_empty(q)) break;

			ip = tlist_front(MirInstr *, q);
			tlist_pop_front(q);
			skip = false;
		}

		result = analyze_instr(cnt, ip);

		switch (result.state) {
		case ANALYZE_PASSED:
			LOG_ANALYZE_PASSED
			postpone_loop_count = 0;
			break;

		case ANALYZE_FAILED:
			LOG_ANALYZE_FAILED
			skip                = true;
			postpone_loop_count = 0;
			break;

		case ANALYZE_POSTPONE:
			LOG_ANALYZE_POSTPONE

			skip = true;
			if (postpone_loop_count++ < q->size) tlist_push_back(q, ip);
			break;

		case ANALYZE_WAITING: {
			LOG_ANALYZE_WAITING

			TArray *  wq   = NULL;
			TIterator iter = thtbl_find(&cnt->analyze.waiting, result.waiting_for);
			TIterator end  = thtbl_end(&cnt->analyze.waiting);
			if (TITERATOR_EQUAL(iter, end)) {
				wq = thtbl_insert_empty(&cnt->analyze.waiting, result.waiting_for);
				tarray_init(wq, sizeof(MirInstr *));
				tarray_reserve(wq, 16);
			} else {
				wq = &thtbl_iter_peek_value(TArray, iter);
			}

			BL_ASSERT(wq);
			tarray_push(wq, ip);
			skip                = true;
			postpone_loop_count = 0;
		}
		}
	}

	if (cnt->analyze.verbose_post) {
		MirInstr *instr;
		TArray *  globals = &cnt->assembly->MIR.global_instrs;
		TARRAY_FOREACH(MirInstr *, globals, instr)
		{
			mir_print_instr(instr, stdout);
		}
	}

	/******************************************************************************************/
#undef LOG_ANALYZE_PASSED
#undef LOG_ANALYZE_FAILED
#undef LOG_ANALYZE_POSTPONE
#undef LOG_ANALYZE_WAITING
	/******************************************************************************************/
}

void
analyze_report_unresolved(Context *cnt)
{
	MirInstr *instr;
	TArray *  wq;
	TIterator iter;

	THTBL_FOREACH(&cnt->analyze.waiting, iter)
	{
		wq = &thtbl_iter_peek_value(TArray, iter);
		BL_ASSERT(wq);
		TARRAY_FOREACH(MirInstr *, wq, instr)
		{
			BL_ASSERT(instr);

			builder_msg(BUILDER_MSG_ERROR,
			            ERR_UNKNOWN_SYMBOL,
			            instr->node->location,
			            BUILDER_CUR_WORD,
			            "Unknown symbol.");
		}
	}
}

/*
 * Push RTTI variable to the array of RTTIs, do stack allocation for execution and push
 * current value on the stack.
 */
static inline MirVar *
gen_RTTI_var(Context *cnt, MirType *type, MirConstValueData *value)
{
	const char *name = gen_uq_name(IMPL_RTTI_ENTRY);
	MirVar *    var  = create_var_impl(cnt, name, type, false, true, false);
	var->value.data  = *value;

	vm_create_implicit_global(&cnt->vm, var);

	/* Push into RTTI table */
	tarray_push(&cnt->assembly->MIR.RTTI_var_queue, var);
	return var;
}

MirConstValue *
gen_RTTI_base(Context *cnt, s32 kind, usize size_bytes)
{
	MirType *struct_type = cnt->builtin_types.t_TypeInfo;
	BL_ASSERT(struct_type);

	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	/* .kind */
	MirType *kind_type = cnt->builtin_types.t_TypeKind;
	BL_ASSERT(kind_type);

	/* kind */
	tsa_push_ConstValuePtr(m, init_or_create_const_integer(cnt, NULL, kind_type, (u64)kind));

	/* size_bytes */
	tsa_push_ConstValuePtr(
	    m, init_or_create_const_integer(cnt, NULL, cnt->builtin_types.t_usize, size_bytes));

	return init_or_create_const_struct(cnt, NULL, struct_type, m);
}

MirVar *
gen_RTTI_empty(Context *cnt, MirType *type, MirType *rtti_type)
{
	MirConstValueData rtti_value = {0};

	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);
	/* .base */
	tsa_push_ConstValuePtr(m, gen_RTTI_base(cnt, (s32)type->kind, type->store_size_bytes));

	/* set members */
	rtti_value.v_struct.members = m;

	/* setup type RTTI and push */
	return gen_RTTI_var(cnt, rtti_type, &rtti_value);
}

MirVar *
gen_RTTI_int(Context *cnt, MirType *type)
{
	const s32  bitcount  = type->data.integer.bitcount;
	const bool is_signed = type->data.integer.is_signed;

	MirConstValueData rtti_value = {0};

	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);
	/* .base */
	tsa_push_ConstValuePtr(m, gen_RTTI_base(cnt, (s32)type->kind, type->store_size_bytes));

	/* .bitcount */
	tsa_push_ConstValuePtr(
	    m, init_or_create_const_integer(cnt, NULL, cnt->builtin_types.t_s32, (u64)bitcount));

	/* .is_signed */
	tsa_push_ConstValuePtr(m, init_or_create_const_bool(cnt, NULL, is_signed));

	/* set members */
	rtti_value.v_struct.members = m;

	/* setup type RTTI and push */
	return gen_RTTI_var(cnt, cnt->builtin_types.t_TypeInfoInt, &rtti_value);
}

MirVar *
gen_RTTI_real(Context *cnt, MirType *type)
{
	const s32 bitcount = type->data.integer.bitcount;

	MirConstValueData rtti_value = {0};

	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);
	/* .base */
	tsa_push_ConstValuePtr(m, gen_RTTI_base(cnt, (s32)type->kind, type->store_size_bytes));

	/* .bitcount */
	tsa_push_ConstValuePtr(
	    m, init_or_create_const_integer(cnt, NULL, cnt->builtin_types.t_s32, (u64)bitcount));

	/* set members */
	rtti_value.v_struct.members = m;

	/* setup type RTTI and push */
	return gen_RTTI_var(cnt, cnt->builtin_types.t_TypeInfoReal, &rtti_value);
}

MirVar *
gen_RTTI_ptr(Context *cnt, MirType *type)
{
	MirVar *rtti_pointed = gen_RTTI(cnt, type->data.ptr.expr);

	MirConstValueData rtti_value = {0};

	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);
	/* .base */
	tsa_push_ConstValuePtr(m, gen_RTTI_base(cnt, (s32)type->kind, type->store_size_bytes));

	tsa_push_ConstValuePtr(m,
	                       init_or_create_const_var_ptr(
	                           cnt, NULL, cnt->builtin_types.t_TypeInfo_ptr, rtti_pointed));

	/* set members */
	rtti_value.v_struct.members = m;

	/* setup type RTTI and push */
	return gen_RTTI_var(cnt, cnt->builtin_types.t_TypeInfoPtr, &rtti_value);
}

MirConstValue *
gen_RTTI_enum_variant(Context *cnt, MirVariant *variant)
{
	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	/* .name */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_string(
	        cnt, NULL, variant->id ? variant->id->str : "<implicit_member>"));

	/* .value */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_integer(
	        cnt, NULL, cnt->builtin_types.t_s64, variant->value->data.v_u64));

	return init_or_create_const_struct(cnt, NULL, cnt->builtin_types.t_TypeInfoEnumVariant, m);
}

MirConstValue *
gen_RTTI_slice_of_enum_variants(Context *cnt, TSmallArray_VariantPtr *variants)
{
	/* First build-up an array variable containing pointers to TypeInfo. */
	MirType *array_type =
	    create_type_array(cnt, cnt->builtin_types.t_TypeInfoEnumVariant, (s64)variants->size);

	MirConstValueData          array_value = {0};
	TSmallArray_ConstValuePtr *elems = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	MirVariant *variant;
	TSA_FOREACH(variants, variant)
	{
		tsa_push_ConstValuePtr(elems, gen_RTTI_enum_variant(cnt, variant));
	}

	array_value.v_array.elems = elems;
	MirVar *array_var         = gen_RTTI_var(cnt, array_type, &array_value);

	/* Create slice. */
	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	/* len */
	tsa_push_ConstValuePtr(
	    m, init_or_create_const_integer(cnt, NULL, cnt->builtin_types.t_s64, variants->size));

	/* ptr */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_var_ptr(cnt, NULL, create_type_ptr(cnt, array_type), array_var));

	return init_or_create_const_struct(
	    cnt, NULL, cnt->builtin_types.t_TypeInfoEnumVariants_slice, m);
}

MirVar *
gen_RTTI_enum(Context *cnt, MirType *type)
{
	MirVar *rtti_pointed = gen_RTTI(cnt, type->data.enm.base_type);

	MirConstValueData rtti_value = {0};

	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);
	/* .base */
	tsa_push_ConstValuePtr(m, gen_RTTI_base(cnt, (s32)type->kind, type->store_size_bytes));

	/* .name */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_string(
	        cnt, NULL, type->user_id ? type->user_id->str : "<implicit_enum>"));

	/* .base_type */
	tsa_push_ConstValuePtr(m,
	                       init_or_create_const_var_ptr(
	                           cnt, NULL, cnt->builtin_types.t_TypeInfo_ptr, rtti_pointed));

	/* .variants */
	tsa_push_ConstValuePtr(m, gen_RTTI_slice_of_enum_variants(cnt, type->data.enm.variants));

	/* set members */
	rtti_value.v_struct.members = m;

	/* setup type RTTI and push */
	return gen_RTTI_var(cnt, cnt->builtin_types.t_TypeInfoEnum, &rtti_value);
}

MirVar *
gen_RTTI_array(Context *cnt, MirType *type)
{
	const s64 len          = type->data.array.len;
	MirVar *  rtti_pointed = gen_RTTI(cnt, type->data.array.elem_type);

	MirConstValueData rtti_value = {0};

	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);
	/* .base */
	tsa_push_ConstValuePtr(m, gen_RTTI_base(cnt, (s32)type->kind, type->store_size_bytes));

	/* .name */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_string(
	        cnt, NULL, type->user_id ? type->user_id->str : "<implicit_array>"));

	/* .base_type */
	tsa_push_ConstValuePtr(m,
	                       init_or_create_const_var_ptr(
	                           cnt, NULL, cnt->builtin_types.t_TypeInfo_ptr, rtti_pointed));

	tsa_push_ConstValuePtr(
	    m, init_or_create_const_integer(cnt, NULL, cnt->builtin_types.t_s64, (u64)len));

	/* set members */
	rtti_value.v_struct.members = m;

	/* setup type RTTI and push */
	return gen_RTTI_var(cnt, cnt->builtin_types.t_TypeInfoArray, &rtti_value);
}

MirConstValue *
gen_RTTI_slice_of_TypeInfo_ptr(Context *cnt, TSmallArray_TypePtr *types)
{
	/* First build-up an array variable containing pointers to TypeInfo. */
	MirType *array_type =
	    create_type_array(cnt, cnt->builtin_types.t_TypeInfo_ptr, (s64)types->size);

	MirConstValueData          array_value = {0};
	TSmallArray_ConstValuePtr *elems = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	MirType *type;
	TSA_FOREACH(types, type)
	{
		tsa_push_ConstValuePtr(
		    elems,
		    init_or_create_const_var_ptr(
		        cnt, NULL, cnt->builtin_types.t_TypeInfo_ptr, gen_RTTI(cnt, type)));
	}

	array_value.v_array.elems = elems;
	MirVar *array_var         = gen_RTTI_var(cnt, array_type, &array_value);

	/* Create slice. */
	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	/* len */
	tsa_push_ConstValuePtr(
	    m, init_or_create_const_integer(cnt, NULL, cnt->builtin_types.t_s64, types->size));

	/* ptr */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_var_ptr(cnt, NULL, create_type_ptr(cnt, array_type), array_var));

	return init_or_create_const_struct(cnt, NULL, cnt->builtin_types.t_TypeInfo_slice, m);
}

MirConstValue *
gen_RTTI_struct_member(Context *cnt, MirMember *member)
{
	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	/* .name */
	tsa_push_ConstValuePtr(m,
	                       init_or_create_const_string(
	                           cnt, NULL, member->id ? member->id->str : "<implicit_member>"));

	/* .base_type */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_var_ptr(
	        cnt, NULL, cnt->builtin_types.t_TypeInfo_ptr, gen_RTTI(cnt, member->type)));

	/* .offset_bytes */
	tsa_push_ConstValuePtr(m,
	                       init_or_create_const_integer(
	                           cnt, NULL, cnt->builtin_types.t_s32, (u64)member->offset_bytes));

	/* .index */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_integer(cnt, NULL, cnt->builtin_types.t_s32, (u64)member->index));

	return init_or_create_const_struct(cnt, NULL, cnt->builtin_types.t_TypeInfoStructMember, m);
}

MirConstValue *
gen_RTTI_fn_arg(Context *cnt, MirArg *arg)
{
	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	/* .name */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_string(cnt, NULL, arg->id ? arg->id->str : "<implicit_argument>"));

	/* .base_type */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_var_ptr(
	        cnt, NULL, cnt->builtin_types.t_TypeInfo_ptr, gen_RTTI(cnt, arg->type)));

	return init_or_create_const_struct(cnt, NULL, cnt->builtin_types.t_TypeInfoFnArg, m);
}

MirConstValue *
gen_RTTI_slice_of_fn_args(Context *cnt, TSmallArray_ArgPtr *args)
{
	const usize argc = args ? args->size : 0;

	/* Create slice. */
	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	/* len */
	tsa_push_ConstValuePtr(
	    m, init_or_create_const_integer(cnt, NULL, cnt->builtin_types.t_s64, argc));

	if (argc) {
		MirType *array_type =
		    create_type_array(cnt, cnt->builtin_types.t_TypeInfoFnArg, argc);

		MirConstValueData          array_value = {0};
		TSmallArray_ConstValuePtr *elems =
		    create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

		MirArg *arg;
		TSA_FOREACH(args, arg)
		{
			tsa_push_ConstValuePtr(elems, gen_RTTI_fn_arg(cnt, arg));
		}

		array_value.v_array.elems = elems;
		MirVar *array_var         = gen_RTTI_var(cnt, array_type, &array_value);

		/* ptr */
		tsa_push_ConstValuePtr(m,
		                       init_or_create_const_var_ptr(
		                           cnt, NULL, create_type_ptr(cnt, array_type), array_var));
	} else {
		/* ptr */
		tsa_push_ConstValuePtr(
		    m,
		    init_or_create_const_null(
		        cnt, NULL, create_type_ptr(cnt, cnt->builtin_types.t_TypeInfoFnArg)));
	}

	return init_or_create_const_struct(cnt, NULL, cnt->builtin_types.t_TypeInfoFnArgs_slice, m);
}

MirConstValue *
gen_RTTI_slice_of_struct_members(Context *cnt, TSmallArray_MemberPtr *members)
{
	/* First build-up an array variable containing pointers to TypeInfo. */
	MirType *array_type =
	    create_type_array(cnt, cnt->builtin_types.t_TypeInfoStructMember, (s64)members->size);

	MirConstValueData          array_value = {0};
	TSmallArray_ConstValuePtr *elems = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	MirMember *member;
	TSA_FOREACH(members, member)
	{
		tsa_push_ConstValuePtr(elems, gen_RTTI_struct_member(cnt, member));
	}

	array_value.v_array.elems = elems;
	MirVar *array_var         = gen_RTTI_var(cnt, array_type, &array_value);

	/* Create slice. */
	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);

	/* len */
	tsa_push_ConstValuePtr(
	    m, init_or_create_const_integer(cnt, NULL, cnt->builtin_types.t_s64, members->size));

	/* ptr */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_var_ptr(cnt, NULL, create_type_ptr(cnt, array_type), array_var));

	return init_or_create_const_struct(
	    cnt, NULL, cnt->builtin_types.t_TypeInfoStructMembers_slice, m);
}

MirVar *
gen_RTTI_struct(Context *cnt, MirType *type)
{
	MirConstValueData rtti_value = {0};

	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);
	/* .base */
	tsa_push_ConstValuePtr(
	    m,
	    gen_RTTI_base(
	        cnt, MIR_TYPE_STRUCT, type->store_size_bytes)); /* use same for MIR_TYPE_SLICE!!! */

	/* name */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_string(
	        cnt, NULL, type->user_id ? type->user_id->str : "<implicit_struct>"));

	/* .members */
	tsa_push_ConstValuePtr(m, gen_RTTI_slice_of_struct_members(cnt, type->data.strct.members));

	/* .is_slice */
	tsa_push_ConstValuePtr(
	    m,
	    init_or_create_const_bool(
	        cnt, NULL, type->kind == MIR_TYPE_SLICE || type->kind == MIR_TYPE_VARGS));

	/* set members */
	rtti_value.v_struct.members = m;

	/* setup type RTTI and push */
	return gen_RTTI_var(cnt, cnt->builtin_types.t_TypeInfoStruct, &rtti_value);
}

MirVar *
gen_RTTI_fn(Context *cnt, MirType *type)
{
	MirConstValueData rtti_value = {0};

	TSmallArray_ConstValuePtr *m = create_sarr(TSmallArray_ConstValuePtr, cnt->assembly);
	/* .base */
	tsa_push_ConstValuePtr(m, gen_RTTI_base(cnt, (s32)type->kind, type->store_size_bytes));

	/* .args */
	tsa_push_ConstValuePtr(m, gen_RTTI_slice_of_fn_args(cnt, type->data.fn.args));

	/* .ret */
	tsa_push_ConstValuePtr(m,
	                       init_or_create_const_var_ptr(cnt,
	                                                    NULL,
	                                                    cnt->builtin_types.t_TypeInfo_ptr,
	                                                    gen_RTTI(cnt, type->data.fn.ret_type)));

	/* .is_vargs  */
	tsa_push_ConstValuePtr(m, init_or_create_const_bool(cnt, NULL, type->data.fn.is_vargs));

	/* set members */
	rtti_value.v_struct.members = m;

	/* setup type RTTI and push */
	MirVar *rtti_var = gen_RTTI_var(cnt, cnt->builtin_types.t_TypeInfoFn, &rtti_value);

	return rtti_var;
}

MirVar *
gen_RTTI(Context *cnt, MirType *type)
{
	BL_ASSERT(type);
	if (assembly_has_rtti(cnt->assembly, type->id.hash))
		return assembly_get_rtti(cnt->assembly, type->id.hash);

	MirVar *rtti_var = NULL;

	switch (type->kind) {
	case MIR_TYPE_TYPE:
		rtti_var = gen_RTTI_empty(cnt, type, cnt->builtin_types.t_TypeInfoType);
		break;

	case MIR_TYPE_VOID:
		rtti_var = gen_RTTI_empty(cnt, type, cnt->builtin_types.t_TypeInfoVoid);
		break;

	case MIR_TYPE_BOOL:
		rtti_var = gen_RTTI_empty(cnt, type, cnt->builtin_types.t_TypeInfoBool);
		break;

	case MIR_TYPE_NULL:
		rtti_var = gen_RTTI_empty(cnt, type, cnt->builtin_types.t_TypeInfoNull);
		break;

	case MIR_TYPE_STRING:
		rtti_var = gen_RTTI_empty(cnt, type, cnt->builtin_types.t_TypeInfoString);
		break;

	case MIR_TYPE_INT:
		rtti_var = gen_RTTI_int(cnt, type);
		break;

	case MIR_TYPE_REAL:
		rtti_var = gen_RTTI_real(cnt, type);
		break;

	case MIR_TYPE_PTR:
		rtti_var = gen_RTTI_ptr(cnt, type);
		break;

	case MIR_TYPE_ENUM:
		rtti_var = gen_RTTI_enum(cnt, type);
		break;

	case MIR_TYPE_ARRAY:
		rtti_var = gen_RTTI_array(cnt, type);
		break;

	case MIR_TYPE_SLICE:
	case MIR_TYPE_VARGS:
	case MIR_TYPE_STRUCT:
		rtti_var = gen_RTTI_struct(cnt, type);
		break;

	case MIR_TYPE_FN:
		rtti_var = gen_RTTI_fn(cnt, type);
		break;

	default: {
		char type_name[256];
		mir_type_to_str(type_name, 256, type, true);
		BL_ABORT("missing RTTI generation for type '%s'", type_name);
	}
	}

	BL_ASSERT(rtti_var);
	assembly_add_rtti(cnt->assembly, type->id.hash, rtti_var);
	return rtti_var;
}

/*
 * Generate global type table in data segment of an assembly.
 */
void
gen_RTTI_types(Context *cnt)
{
	/* include prefix t_Type and MIR_BUILTIN_ID_TYPE_ */
	/******************************************************************************************/
#define LOOKUP_TYPE(N, K)                                                                          \
	{                                                                                          \
		cnt->builtin_types.t_Type##N = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_##K);       \
		BL_ASSERT(cnt->builtin_types.t_Type##N && "Builtin type " #N " not found!");       \
	}
	/******************************************************************************************/

	THashTable *table = &cnt->analyze.RTTI_entry_types;
	if (table->size == 0) return;

	LOOKUP_TYPE(Kind, KIND);
	LOOKUP_TYPE(Info, INFO);
	LOOKUP_TYPE(InfoInt, INFO_INT);
	LOOKUP_TYPE(InfoReal, INFO_REAL);
	LOOKUP_TYPE(InfoPtr, INFO_PTR);
	LOOKUP_TYPE(InfoEnum, INFO_ENUM);
	LOOKUP_TYPE(InfoEnumVariant, INFO_ENUM_VARIANT);
	LOOKUP_TYPE(InfoArray, INFO_ARRAY);
	LOOKUP_TYPE(InfoStruct, INFO_STRUCT);
	LOOKUP_TYPE(InfoStructMember, INFO_STRUCT_MEMBER);
	LOOKUP_TYPE(InfoFn, INFO_FN);
	LOOKUP_TYPE(InfoFnArg, INFO_FN_ARG);
	LOOKUP_TYPE(InfoType, INFO_TYPE);
	LOOKUP_TYPE(InfoVoid, INFO_VOID);
	LOOKUP_TYPE(InfoBool, INFO_BOOL);
	LOOKUP_TYPE(InfoNull, INFO_NULL);
	LOOKUP_TYPE(InfoString, INFO_STRING);

	cnt->builtin_types.t_TypeInfo_ptr = create_type_ptr(cnt, cnt->builtin_types.t_TypeInfo);

	cnt->builtin_types.t_TypeInfo_slice = create_type_struct_special(
	    cnt, MIR_TYPE_SLICE, NULL, cnt->builtin_types.t_TypeInfo_ptr);

	cnt->builtin_types.t_TypeInfoStructMembers_slice = create_type_struct_special(
	    cnt,
	    MIR_TYPE_SLICE,
	    NULL,
	    create_type_ptr(cnt, cnt->builtin_types.t_TypeInfoStructMember));

	cnt->builtin_types.t_TypeInfoEnumVariants_slice = create_type_struct_special(
	    cnt,
	    MIR_TYPE_SLICE,
	    NULL,
	    create_type_ptr(cnt, cnt->builtin_types.t_TypeInfoEnumVariant));

	cnt->builtin_types.t_TypeInfoFnArgs_slice = create_type_struct_special(
	    cnt, MIR_TYPE_SLICE, NULL, create_type_ptr(cnt, cnt->builtin_types.t_TypeInfoFnArg));

	TIterator it;
	MirType * type;
	THTBL_FOREACH(table, it)
	{
		type = (MirType *)thtbl_iter_peek_key(it);
		gen_RTTI(cnt, type);
	}

#undef LOOKUP_TYPE
}

/* MIR builting */
void
ast_defer_block(Context *cnt, Ast *block, bool whole_tree)
{
	TSmallArray_DeferStack *stack = &cnt->ast.defer_stack;
	Ast *                   defer;

	for (usize i = stack->size; i-- > 0;) {
		defer = stack->data[i];

		if (defer->owner_scope == block->owner_scope) {
			tsa_pop_DeferStack(stack);
		} else if (!whole_tree) {
			break;
		}

		ast(cnt, defer->data.stmt_defer.expr);
	}
}

void
ast_ublock(Context *cnt, Ast *ublock)
{
	Ast *tmp;
	TARRAY_FOREACH(Ast *, ublock->data.ublock.nodes, tmp) ast(cnt, tmp);
}

void
ast_block(Context *cnt, Ast *block)
{
	if (cnt->debug_mode) init_llvm_DI_scope(cnt, block->owner_scope);

	Ast *tmp;
	TARRAY_FOREACH(Ast *, block->data.block.nodes, tmp) ast(cnt, tmp);

	if (!block->data.block.has_return) ast_defer_block(cnt, block, false);
}

void
ast_test_case(Context *cnt, Ast *test)
{
	/* build test function */
	Ast *ast_block = test->data.test_case.block;
	BL_ASSERT(ast_block);

	MirInstrFnProto *fn_proto =
	    (MirInstrFnProto *)append_instr_fn_proto(cnt, test, NULL, NULL, true);

	fn_proto->base.value.type = cnt->builtin_types.t_test_case_fn;

	const bool  emit_llvm    = builder.options.force_test_llvm;
	const char *linkage_name = gen_uq_name(TEST_CASE_FN_NAME);
	const bool  is_in_gscope =
	    test->owner_scope->kind == SCOPE_GLOBAL || test->owner_scope->kind == SCOPE_PRIVATE;
	MirFn *fn =
	    create_fn(cnt, test, NULL, linkage_name, FLAG_TEST, fn_proto, emit_llvm, is_in_gscope);

	BL_ASSERT(test->data.test_case.desc);
	fn->test_case_desc = test->data.test_case.desc;
	mir_set_const_ptr(&fn_proto->base.value.data.v_ptr, fn, MIR_CP_FN);

	tarray_push(&cnt->test_cases, fn);

	MirInstrBlock *entry_block = append_block(cnt, fn, "entry");

	cnt->ast.exit_block = append_block(cnt, fn, "exit");

	set_current_block(cnt, cnt->ast.exit_block);
	append_instr_ret(cnt, NULL, NULL, false);

	set_current_block(cnt, entry_block);

	/* generate body instructions */
	ast(cnt, ast_block);
}

void
ast_unrecheable(Context *cnt, Ast *unr)
{
	append_instr_unrecheable(cnt, unr);
}

void
ast_stmt_if(Context *cnt, Ast *stmt_if)
{
	Ast *ast_cond = stmt_if->data.stmt_if.test;
	Ast *ast_then = stmt_if->data.stmt_if.true_stmt;
	Ast *ast_else = stmt_if->data.stmt_if.false_stmt;
	BL_ASSERT(ast_cond && ast_then);

	MirFn *fn = get_current_fn(cnt);
	BL_ASSERT(fn);

	MirInstrBlock *tmp_block  = NULL;
	MirInstrBlock *then_block = append_block(cnt, fn, "if_then");
	MirInstrBlock *else_block = append_block(cnt, fn, "if_else");
	MirInstrBlock *cont_block = append_block(cnt, fn, "if_cont");

	MirInstr *cond = ast(cnt, ast_cond);
	append_instr_cond_br(cnt, stmt_if, cond, then_block, else_block);

	/* then block */
	set_current_block(cnt, then_block);
	ast(cnt, ast_then);

	tmp_block = get_current_block(cnt);
	if (!get_block_terminator(tmp_block)) {
		/* block has not been terminated -> add terminator */
		append_instr_br(cnt, NULL, cont_block);
	}

	/* else if */
	if (ast_else) {
		set_current_block(cnt, else_block);
		ast(cnt, ast_else);

		tmp_block = get_current_block(cnt);
		if (!is_block_terminated(tmp_block)) append_instr_br(cnt, NULL, cont_block);
	}

	if (!is_block_terminated(else_block)) {
		/* block has not been terminated -> add terminator */
		set_current_block(cnt, else_block);
		append_instr_br(cnt, NULL, cont_block);
	}

	set_current_block(cnt, cont_block);
}

void
ast_stmt_loop(Context *cnt, Ast *loop)
{
	Ast *ast_block     = loop->data.stmt_loop.block;
	Ast *ast_cond      = loop->data.stmt_loop.condition;
	Ast *ast_increment = loop->data.stmt_loop.increment;
	Ast *ast_init      = loop->data.stmt_loop.init;
	BL_ASSERT(ast_block);

	MirFn *fn = get_current_fn(cnt);
	BL_ASSERT(fn);

	/* prepare all blocks */
	MirInstrBlock *tmp_block = NULL;
	MirInstrBlock *increment_block =
	    ast_increment ? append_block(cnt, fn, "loop_increment") : NULL;
	MirInstrBlock *decide_block = append_block(cnt, fn, "loop_decide");
	MirInstrBlock *body_block   = append_block(cnt, fn, "loop_body");
	MirInstrBlock *cont_block   = append_block(cnt, fn, "loop_continue");

	MirInstrBlock *prev_break_block    = cnt->ast.break_block;
	MirInstrBlock *prev_continue_block = cnt->ast.continue_block;
	cnt->ast.break_block               = cont_block;
	cnt->ast.continue_block            = ast_increment ? increment_block : decide_block;

	/* generate initialization if there is one */
	if (ast_init) {
		ast(cnt, ast_init);
	}

	/* decide block */
	append_instr_br(cnt, NULL, decide_block);
	set_current_block(cnt, decide_block);

	MirInstr *cond = ast_cond ? ast(cnt, ast_cond) : append_instr_const_bool(cnt, NULL, true);

	append_instr_cond_br(cnt, ast_cond, cond, body_block, cont_block);

	/* loop body */
	set_current_block(cnt, body_block);
	ast(cnt, ast_block);

	tmp_block = get_current_block(cnt);
	if (!is_block_terminated(tmp_block)) {
		append_instr_br(cnt, NULL, ast_increment ? increment_block : decide_block);
	}

	/* increment if there is one */
	if (ast_increment) {
		set_current_block(cnt, increment_block);
		ast(cnt, ast_increment);
		append_instr_br(cnt, NULL, decide_block);
	}

	cnt->ast.break_block    = prev_break_block;
	cnt->ast.continue_block = prev_continue_block;
	set_current_block(cnt, cont_block);
}

void
ast_stmt_break(Context *cnt, Ast *br)
{
	BL_ASSERT(cnt->ast.break_block && "break statement outside the loop");
	append_instr_br(cnt, br, cnt->ast.break_block);
}

void
ast_stmt_continue(Context *cnt, Ast *cont)
{
	BL_ASSERT(cnt->ast.continue_block && "break statement outside the loop");
	append_instr_br(cnt, cont, cnt->ast.continue_block);
}

void
ast_stmt_switch(Context *cnt, Ast *stmt_switch)
{
	TSmallArray_AstPtr *ast_cases = stmt_switch->data.stmt_switch.cases;
	BL_ASSERT(ast_cases);

	TSmallArray_SwitchCase *cases = create_sarr(TSmallArray_SwitchCase, cnt->assembly);

	MirFn *fn = get_current_fn(cnt);
	BL_ASSERT(fn);

	MirInstrBlock *src_block            = get_current_block(cnt);
	MirInstrBlock *cont_block           = append_block(cnt, fn, "switch_continue");
	MirInstrBlock *default_block        = cont_block;
	bool           user_defined_default = false;

	for (usize i = ast_cases->size; i-- > 0;) {
		Ast *      ast_case   = ast_cases->data[i];
		const bool is_default = ast_case->data.stmt_case.is_default;

		MirInstrBlock *case_block = NULL;

		if (ast_case->data.stmt_case.block) {
			case_block =
			    append_block(cnt, fn, is_default ? "switch_default" : "switch_case");
			set_current_block(cnt, case_block);
			ast(cnt, ast_case->data.stmt_case.block);
			append_instr_br(cnt, ast_case, cont_block);
		} else {
			/* Handle empty cases. */
			case_block = cont_block;
		}

		if (is_default) {
			default_block        = case_block;
			user_defined_default = true;
			continue;
		}

		TSmallArray_AstPtr *ast_exprs = ast_case->data.stmt_case.exprs;

		for (usize i = ast_exprs->size; i-- > 0;) {
			Ast *ast_expr = ast_exprs->data[i];

			set_current_block(cnt, src_block);
			MirSwitchCase c = {.on_value = ast(cnt, ast_expr), .block = case_block};
			tsa_push_SwitchCase(cases, c);
		}
	}

	/* Generate instructions for switch value and create switch itself. */
	set_current_block(cnt, src_block);

	MirInstr *value = ast(cnt, stmt_switch->data.stmt_switch.expr);
	append_instr_switch(cnt, stmt_switch, value, default_block, user_defined_default, cases);

	set_current_block(cnt, cont_block);
}

void
ast_stmt_return(Context *cnt, Ast *ret)
{
	/* Return statement produce only setup of .ret temporary and break into the exit
	 * block of the function. */
	MirInstr *value = ast(cnt, ret->data.stmt_return.expr);

	if (!is_current_block_terminated(cnt)) {
		MirFn *fn = get_current_fn(cnt);
		BL_ASSERT(fn);

		if (fn->ret_tmp) {
			if (!value) {
				builder_msg(BUILDER_MSG_ERROR,
				            ERR_EXPECTED_EXPR,
				            ret->location,
				            BUILDER_CUR_AFTER,
				            "Expected return value.");
			}

			MirInstr *ref = append_instr_decl_direct_ref(cnt, fn->ret_tmp);
			append_instr_store(cnt, ret, value, ref);
		} else if (value) {
			builder_msg(BUILDER_MSG_ERROR,
			            ERR_UNEXPECTED_EXPR,
			            value->node->location,
			            BUILDER_CUR_WORD,
			            "Unexpected return value.");
		}

		ast_defer_block(cnt, ret->data.stmt_return.owner_block, true);
	}

	BL_ASSERT(cnt->ast.exit_block);
	append_instr_br(cnt, ret, cnt->ast.exit_block);
}

void
ast_stmt_defer(Context *cnt, Ast *defer)
{
	/* push new defer record */
	tsa_push_DeferStack(&cnt->ast.defer_stack, defer);
}

MirInstr *
ast_expr_compound(Context *cnt, Ast *cmp)
{
	TSmallArray_AstPtr *ast_values = cmp->data.expr_compound.values;
	Ast *               ast_type   = cmp->data.expr_compound.type;
	MirInstr *          type       = ast(cnt, ast_type);
	BL_ASSERT(type);

	if (!ast_values) {
		return append_instr_compound(cnt, cmp, type, NULL);
	}

	const usize valc = ast_values->size;

	BL_ASSERT(ast_type);

	TSmallArray_InstrPtr *values = create_sarr(TSmallArray_InstrPtr, cnt->assembly);
	tsa_resize_InstrPtr(values, valc);

	Ast *     ast_value;
	MirInstr *value;

	/* Values must be appended in reverse order. */
	for (usize i = valc; i-- > 0;) {
		ast_value = ast_values->data[i];
		value     = ast(cnt, ast_value);
		BL_ASSERT(value);
		values->data[i] = value;
	}

	return append_instr_compound(cnt, cmp, type, values);
}

MirInstr *
ast_expr_line(Context *cnt, Ast *line)
{
	const s32 l = line->data.expr_line.line;
	return append_instr_const_int(cnt, line, cnt->builtin_types.t_s32, (u64)l);
};

MirInstr *
ast_expr_file(Context *cnt, Ast *file)
{
	const char *f = file->data.expr_file.filename;
	return append_instr_const_string(cnt, file, f);
}

MirInstr *
ast_expr_addrof(Context *cnt, Ast *addrof)
{
	MirInstr *src = ast(cnt, addrof->data.expr_addrof.next);
	BL_ASSERT(src);

	return append_instr_addrof(cnt, addrof, src);
}

MirInstr *
ast_expr_cast(Context *cnt, Ast *cast)
{
	const bool auto_cast = cast->data.expr_cast.auto_cast;
	Ast *      ast_type  = cast->data.expr_cast.type;
	Ast *      ast_next  = cast->data.expr_cast.next;
	BL_ASSERT(ast_next);

	// INCOMPLETE: const type!!!
	MirInstr *type = NULL;

	if (!auto_cast) {
		BL_ASSERT(ast_type);
		type = CREATE_TYPE_RESOLVER_CALL(ast_type);
	}

	MirInstr *next = ast(cnt, ast_next);

	return append_instr_cast(cnt, cast, type, next);
}

MirInstr *
ast_expr_sizeof(Context *cnt, Ast *szof)
{
	Ast *ast_node = szof->data.expr_sizeof.node;
	BL_ASSERT(ast_node);

	MirInstr *expr = ast(cnt, ast_node);
	return append_instr_sizeof(cnt, szof, expr);
}

MirInstr *
ast_expr_type_info(Context *cnt, Ast *type_info)
{
	Ast *ast_node = type_info->data.expr_type_info.node;
	BL_ASSERT(ast_node);

	MirInstr *expr = ast(cnt, ast_node);
	return append_instr_type_info(cnt, type_info, expr);
}

MirInstr *
ast_expr_alignof(Context *cnt, Ast *szof)
{
	Ast *ast_node = szof->data.expr_alignof.node;
	BL_ASSERT(ast_node);

	MirInstr *expr = ast(cnt, ast_node);
	return append_instr_alignof(cnt, szof, expr);
}

MirInstr *
ast_expr_deref(Context *cnt, Ast *deref)
{
	MirInstr *next = ast(cnt, deref->data.expr_deref.next);
	BL_ASSERT(next);
	return append_instr_load(cnt, deref, next);
}

MirInstr *
ast_expr_lit_int(Context *cnt, Ast *expr)
{
	u64 val = expr->data.expr_integer.val;

	if (expr->data.expr_integer.overflow) {
		builder_msg(
		    BUILDER_MSG_ERROR,
		    ERR_NUM_LIT_OVERFLOW,
		    expr->location,
		    BUILDER_CUR_WORD,
		    "Integer literal is too big and cannot be represented as any integer type.");
	}

	MirType * type         = NULL;
	const int desired_bits = count_bits(val);

	/* Here we choose best type for const integer literal: s32, s64 or u64. When u64 is
	 * selected, this number cannot be negative. */
	if (desired_bits < 32) {
		type = cnt->builtin_types.t_s32;
	} else if (desired_bits < 64) {
		type = cnt->builtin_types.t_s64;
	} else {
		type = cnt->builtin_types.t_u64;
	}

	return append_instr_const_int(cnt, expr, type, val);
}

MirInstr *
ast_expr_lit_float(Context *cnt, Ast *expr)
{
	if (expr->data.expr_float.overflow) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_NUM_LIT_OVERFLOW,
		            expr->location,
		            BUILDER_CUR_WORD,
		            "Float literal is too big and cannot be represented as f32.");
	}

	return append_instr_const_float(cnt, expr, expr->data.expr_float.val);
}

MirInstr *
ast_expr_lit_double(Context *cnt, Ast *expr)
{
	if (expr->data.expr_double.overflow) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_NUM_LIT_OVERFLOW,
		            expr->location,
		            BUILDER_CUR_WORD,
		            "Double literal is too big and cannot be represented as f64.");
	}

	return append_instr_const_double(cnt, expr, expr->data.expr_double.val);
}

MirInstr *
ast_expr_lit_bool(Context *cnt, Ast *expr)
{
	return append_instr_const_bool(cnt, expr, expr->data.expr_boolean.val);
}

MirInstr *
ast_expr_lit_char(Context *cnt, Ast *expr)
{
	return append_instr_const_char(cnt, expr, (s8)expr->data.expr_character.val);
}

MirInstr *
ast_expr_null(Context *cnt, Ast *nl)
{
	return append_instr_const_null(cnt, nl);
}

MirInstr *
ast_expr_call(Context *cnt, Ast *call)
{
	Ast *               ast_callee = call->data.expr_call.ref;
	TSmallArray_AstPtr *ast_args   = call->data.expr_call.args;
	BL_ASSERT(ast_callee);

	TSmallArray_InstrPtr *args = create_sarr(TSmallArray_InstrPtr, cnt->assembly);

	/* arguments need to be generated into reverse order due to bytecode call
	 * conventions */
	if (ast_args) {
		const usize argc = ast_args->size;
		tsa_resize_InstrPtr(args, argc);
		MirInstr *arg;
		Ast *     ast_arg;
		for (usize i = argc; i-- > 0;) {
			ast_arg = ast_args->data[i];
			arg     = ast(cnt, ast_arg);

			args->data[i] = arg;
		}
	}

	MirInstr *callee = ast(cnt, ast_callee);

	return append_instr_call(cnt, call, callee, args);
}

MirInstr *
ast_expr_ref(Context *cnt, Ast *ref)
{
	Ast *ident = ref->data.expr_ref.ident;
	BL_ASSERT(ident);
	BL_ASSERT(ident->kind == AST_IDENT);

	Scope *scope = ident->owner_scope;
	Unit * unit  = ident->location->unit;
	BL_ASSERT(unit);
	BL_ASSERT(scope);

	return append_instr_decl_ref(cnt, ref, unit, &ident->data.ident.id, scope, NULL);
}

MirInstr *
ast_expr_elem(Context *cnt, Ast *elem)
{
	Ast *ast_arr   = elem->data.expr_elem.next;
	Ast *ast_index = elem->data.expr_elem.index;
	BL_ASSERT(ast_arr && ast_index);

	MirInstr *arr_ptr = ast(cnt, ast_arr);
	MirInstr *index   = ast(cnt, ast_index);

	return append_instr_elem_ptr(cnt, elem, arr_ptr, index, false);
}

MirInstr *
ast_expr_member(Context *cnt, Ast *member)
{
	Ast *ast_next = member->data.expr_member.next;
	// BL_ASSERT(ast_next);

	MirInstr *target = ast(cnt, ast_next);
	// BL_ASSERT(target);

	return append_instr_member_ptr(
	    cnt, member, target, member->data.expr_member.ident, NULL, MIR_BUILTIN_ID_NONE);
}

MirInstr *
ast_expr_lit_fn(Context *cnt, Ast *lit_fn, Ast *decl_node, bool is_in_gscope, u32 flags)
{
	/* creates function prototype */
	Ast *ast_block   = lit_fn->data.expr_fn.block;
	Ast *ast_fn_type = lit_fn->data.expr_fn.type;

	MirInstrFnProto *fn_proto =
	    (MirInstrFnProto *)append_instr_fn_proto(cnt, lit_fn, NULL, NULL, true);

	/* Generate type resolver for function type. */
	fn_proto->type = CREATE_TYPE_RESOLVER_CALL(ast_fn_type);
	BL_ASSERT(fn_proto->type);

	MirInstrBlock *prev_block      = get_current_block(cnt);
	MirInstrBlock *prev_exit_block = cnt->ast.exit_block;

	MirFn *fn = create_fn(cnt,
	                      decl_node ? decl_node : lit_fn,
	                      decl_node ? &decl_node->data.ident.id : NULL,
	                      NULL,
	                      (u32)flags,
	                      fn_proto,
	                      true,
	                      is_in_gscope);

	mir_set_const_ptr(&fn_proto->base.value.data.v_ptr, fn, MIR_CP_FN);

	/* function body */
	/* external functions has no body */
	if (IS_FLAG(flags, FLAG_EXTERN)) {
		return &fn_proto->base;
	}

	if (!ast_block) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_EXPECTED_BODY,
		            decl_node ? decl_node->location : lit_fn->location,
		            BUILDER_CUR_WORD,
		            "Missing function body.");
	}

	/* Set body scope for DI. */
	fn->body_scope = ast_block->owner_scope;

	/* create block for initialization locals and arguments */
	MirInstrBlock *init_block = append_block(cnt, fn, "entry");

	/* Every user generated function must contain exit block; this block is invoked last
	 * in every function a eventually can return .ret value stored in temporary storage.
	 * When ast parser hit user defined 'return' statement it sets up .ret temporary if
	 * there is one and produce break into exit block. This approach is needed due to
	 * defer statement, because we need to call defer blocks after return value
	 * evaluation and before terminal instruction of the function. Last defer block
	 * always breaks into the exit block. */
	cnt->ast.exit_block = append_block(cnt, fn, "exit");

	if (ast_fn_type->data.type_fn.ret_type) {
		set_current_block(cnt, init_block);
		fn->ret_tmp = append_instr_decl_var_impl(
		    cnt, gen_uq_name(IMPL_RET_TMP), NULL, NULL, true, false, -1, 0);

		set_current_block(cnt, cnt->ast.exit_block);
		MirInstr *ret_init = append_instr_decl_direct_ref(cnt, fn->ret_tmp);

		append_instr_ret(cnt, NULL, ret_init, false);
	} else {
		set_current_block(cnt, cnt->ast.exit_block);
		append_instr_ret(cnt, NULL, NULL, false);
	}

	set_current_block(cnt, init_block);

	/* build MIR for fn arguments */
	TSmallArray_AstPtr *ast_args = ast_fn_type->data.type_fn.args;
	if (ast_args) {
		Ast *ast_arg;
		Ast *ast_arg_name;

		const usize argc = ast_args->size;
		for (usize i = argc; i-- > 0;) {
			ast_arg = ast_args->data[i];
			BL_ASSERT(ast_arg->kind == AST_DECL_ARG);
			ast_arg_name = ast_arg->data.decl.name;
			BL_ASSERT(ast_arg_name);

			/* create tmp declaration for arg variable */
			MirInstr *arg = append_instr_arg(cnt, NULL, (u32)i);
			append_instr_decl_var(cnt, ast_arg_name, NULL, arg, true, false, (s32)i, 0);

			register_symbol(cnt,
			                ast_arg_name,
			                &ast_arg_name->data.ident.id,
			                ast_arg_name->owner_scope,
			                false,
			                false);
		}
	}

	/* generate body instructions */
	ast(cnt, ast_block);

	if (!is_block_terminated(get_current_block(cnt)))
		append_instr_br(cnt, NULL, cnt->ast.exit_block);

	cnt->ast.exit_block = prev_exit_block;
	set_current_block(cnt, prev_block);
	return &fn_proto->base;
}

MirInstr *
ast_expr_lit_string(Context *cnt, Ast *lit_string)
{
	const char *cstr = lit_string->data.expr_string.val;
	BL_ASSERT(cstr);
	return append_instr_const_string(cnt, lit_string, cstr);
}

MirInstr *
ast_expr_binop(Context *cnt, Ast *binop)
{
	Ast *ast_lhs = binop->data.expr_binop.lhs;
	Ast *ast_rhs = binop->data.expr_binop.rhs;
	BL_ASSERT(ast_lhs && ast_rhs);

	const BinopKind op = binop->data.expr_binop.kind;

	switch (op) {
	case BINOP_ASSIGN: {
		MirInstr *rhs = ast(cnt, ast_rhs);
		MirInstr *lhs = ast(cnt, ast_lhs);

		return append_instr_store(cnt, binop, rhs, lhs);
	}

	case BINOP_ADD_ASSIGN: {
		MirInstr *rhs = ast(cnt, ast_rhs);
		MirInstr *lhs = ast(cnt, ast_lhs);
		MirInstr *tmp = append_instr_binop(cnt, binop, lhs, rhs, BINOP_ADD);
		lhs           = ast(cnt, ast_lhs);

		return append_instr_store(cnt, binop, tmp, lhs);
	}

	case BINOP_SUB_ASSIGN: {
		MirInstr *rhs = ast(cnt, ast_rhs);
		MirInstr *lhs = ast(cnt, ast_lhs);
		MirInstr *tmp = append_instr_binop(cnt, binop, lhs, rhs, BINOP_SUB);
		lhs           = ast(cnt, ast_lhs);

		return append_instr_store(cnt, binop, tmp, lhs);
	}

	case BINOP_MUL_ASSIGN: {
		MirInstr *rhs = ast(cnt, ast_rhs);
		MirInstr *lhs = ast(cnt, ast_lhs);
		MirInstr *tmp = append_instr_binop(cnt, binop, lhs, rhs, BINOP_MUL);
		lhs           = ast(cnt, ast_lhs);

		return append_instr_store(cnt, binop, tmp, lhs);
	}

	case BINOP_DIV_ASSIGN: {
		MirInstr *rhs = ast(cnt, ast_rhs);
		MirInstr *lhs = ast(cnt, ast_lhs);
		MirInstr *tmp = append_instr_binop(cnt, binop, lhs, rhs, BINOP_DIV);
		lhs           = ast(cnt, ast_lhs);

		return append_instr_store(cnt, binop, tmp, lhs);
	}

	case BINOP_MOD_ASSIGN: {
		MirInstr *rhs = ast(cnt, ast_rhs);
		MirInstr *lhs = ast(cnt, ast_lhs);
		MirInstr *tmp = append_instr_binop(cnt, binop, lhs, rhs, BINOP_MOD);
		lhs           = ast(cnt, ast_lhs);

		return append_instr_store(cnt, binop, tmp, lhs);
	}

	case BINOP_LOGIC_AND: {
		MirFn *        fn        = get_current_fn(cnt);
		MirInstrBlock *rhs_block = append_block(cnt, fn, "rhs_block");
		MirInstrBlock *end_block = append_block(cnt, fn, "end_block");

		MirInstr *lhs = ast(cnt, ast_lhs);
		append_instr_cond_br(cnt, NULL, lhs, rhs_block, end_block);

		set_current_block(cnt, rhs_block);
		MirInstr *rhs = ast(cnt, ast_rhs);
		append_instr_br(cnt, NULL, end_block);

		set_current_block(cnt, end_block);
		MirInstr *   const_false = append_instr_const_bool(cnt, NULL, false);
		MirInstrPhi *phi         = (MirInstrPhi *)append_instr_phi(cnt, binop);
		phi_add_income(phi, const_false, lhs->owner_block);
		phi_add_income(phi, rhs, rhs_block);

		return &phi->base;
	}

	case BINOP_LOGIC_OR: {
		MirFn *        fn        = get_current_fn(cnt);
		MirInstrBlock *rhs_block = append_block(cnt, fn, "rhs_block");
		MirInstrBlock *end_block = append_block(cnt, fn, "end_block");

		MirInstr *lhs = ast(cnt, ast_lhs);
		append_instr_cond_br(cnt, NULL, lhs, end_block, rhs_block);

		set_current_block(cnt, rhs_block);
		MirInstr *rhs = ast(cnt, ast_rhs);
		append_instr_br(cnt, NULL, end_block);

		set_current_block(cnt, end_block);
		MirInstr *   const_true = append_instr_const_bool(cnt, NULL, true);
		MirInstrPhi *phi        = (MirInstrPhi *)append_instr_phi(cnt, binop);
		phi_add_income(phi, const_true, lhs->owner_block);
		phi_add_income(phi, rhs, rhs_block);

		return &phi->base;
	}

	default: {
		MirInstr *rhs = ast(cnt, ast_rhs);
		MirInstr *lhs = ast(cnt, ast_lhs);
		return append_instr_binop(cnt, binop, lhs, rhs, op);
	}
	}
}

MirInstr *
ast_expr_unary(Context *cnt, Ast *unop)
{
	Ast *ast_next = unop->data.expr_unary.next;
	BL_ASSERT(ast_next);

	MirInstr *next = ast(cnt, ast_next);
	BL_ASSERT(next);

	return append_instr_unop(cnt, unop, next, unop->data.expr_unary.kind);
}

MirInstr *
ast_expr_type(Context *cnt, Ast *type)
{
	Ast *next_type = type->data.expr_type.type;
	BL_ASSERT(next_type);

	return ast(cnt, next_type);
}

MirInstr *
ast_decl_entity(Context *cnt, Ast *entity)
{
	MirInstr * result         = NULL;
	Ast *      ast_name       = entity->data.decl.name;
	Ast *      ast_type       = entity->data.decl.type;
	Ast *      ast_value      = entity->data.decl_entity.value;
	const bool is_fn_decl     = ast_value && ast_value->kind == AST_EXPR_LIT_FN;
	const bool is_struct_decl = ast_value && ast_value->kind == AST_EXPR_TYPE &&
	                            ast_value->data.expr_type.type->kind == AST_TYPE_STRUCT;
	const bool is_mutable    = entity->data.decl_entity.mut;
	const bool is_in_gscope  = entity->data.decl_entity.in_gscope;
	const bool is_compiler   = IS_FLAG(entity->data.decl_entity.flags, FLAG_COMPILER);
	bool       enable_groups = false;

	BL_ASSERT(ast_name && "Missing entity name.");
	BL_ASSERT(ast_name->kind == AST_IDENT && "Expected identificator.");

	Scope *scope = ast_name->owner_scope;
	ID *   id    = &ast_name->data.ident.id;

	if (is_fn_decl) {
		/* recognised named function declaraton */
		const s32 flags = entity->data.decl_entity.flags;
		MirInstr *value = ast_expr_lit_fn(cnt, ast_value, ast_name, is_in_gscope, flags);
		enable_groups   = true;

		if (ast_type) {
			((MirInstrFnProto *)value)->user_type = CREATE_TYPE_RESOLVER_CALL(ast_type);
		}

		/* check main */
		if (is_builtin(ast_name, MIR_BUILTIN_ID_MAIN)) {
			BL_ASSERT(!cnt->entry_fn);
			cnt->entry_fn = value->value.data.v_ptr.data.fn;
			ref_instr(cnt->entry_fn->prototype); /* main must be generated into LLVM */
		}
	} else {
		/* other declaration types */
		MirInstr *type = ast_type ? CREATE_TYPE_RESOLVER_CALL(ast_type) : NULL;

		cnt->ast.current_entity_id = &ast_name->data.ident.id;
		/* initialize value */
		MirInstr *value = NULL;
		if (!ast_value) goto NO_VALUE;
		if (is_struct_decl) {
			// Set to const type fwd decl
			MirType *fwd_decl_type =
			    create_type_struct_incomplete(cnt, cnt->ast.current_entity_id);
			value = create_instr_const_type(cnt, ast_value, fwd_decl_type);
			analyze_instr_rq(cnt, value);

			// Set current fwd decl
			cnt->ast.current_fwd_struct_decl = value;

			// Enable incomplete types for decl_ref instructions.
			cnt->ast.enable_incomplete_decl_refs = true;

			// Generate value resolver
			CREATE_VALUE_RESOLVER_CALL(ast_value, true);

			cnt->ast.enable_incomplete_decl_refs = false;
			cnt->ast.current_fwd_struct_decl     = NULL;
		} else if (is_in_gscope) {
			/* Initialization of global variables must be done in
			 * implicit initializer function executed in compile
			 * time. Every initialization function must be able to
			 * be executed in compile time. */
			value = CREATE_VALUE_RESOLVER_CALL(ast_value, false);
		} else {
			value = ast(cnt, ast_value);
		}

	NO_VALUE:
		append_instr_decl_var(cnt,
		                      ast_name,
		                      type,
		                      value,
		                      is_mutable,
		                      is_in_gscope,
		                      -1,
		                      entity->data.decl_entity.flags);

		cnt->ast.current_entity_id = NULL;
	}

	if (!is_fn_decl && is_builtin(ast_name, MIR_BUILTIN_ID_MAIN)) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_EXPECTED_FUNC,
		            ast_name->location,
		            BUILDER_CUR_WORD,
		            "Main is expected to be a function.");
	}

	register_symbol(cnt, ast_name, id, scope, is_compiler, enable_groups);
	return result;
}

MirInstr *
ast_decl_arg(Context *cnt, Ast *arg)
{
	Ast *ast_name = arg->data.decl.name;
	Ast *ast_type = arg->data.decl.type;

	BL_ASSERT(ast_type);
	MirInstr *type = ast(cnt, ast_type);

	return append_instr_decl_arg(cnt, ast_name, type);
}

MirInstr *
ast_decl_member(Context *cnt, Ast *arg)
{
	Ast *ast_type = arg->data.decl.type;
	Ast *ast_name = arg->data.decl.name;

	BL_ASSERT(ast_type);
	MirInstr *result = ast(cnt, ast_type);

	/* named member? */
	if (ast_name) {
		BL_ASSERT(ast_name->kind == AST_IDENT);
		result = append_instr_decl_member(cnt, ast_name, result);

		register_symbol(
		    cnt, ast_name, &ast_name->data.ident.id, ast_name->owner_scope, false, false);
	}

	BL_ASSERT(result);
	return result;
}

MirInstr *
ast_decl_variant(Context *cnt, Ast *variant)
{
	Ast *ast_name  = variant->data.decl.name;
	Ast *ast_value = variant->data.decl_variant.value;
	BL_ASSERT(ast_name && "Missing enum variant name!");

	MirInstr *value = ast(cnt, ast_value);

	register_symbol(
	    cnt, ast_name, &ast_name->data.ident.id, ast_name->owner_scope, false, false);

	return append_instr_decl_variant(cnt, ast_name, value);
}

MirInstr *
ast_type_ref(Context *cnt, Ast *type_ref)
{
	Ast *ident = type_ref->data.type_ref.ident;
	BL_ASSERT(ident);

	Scope *scope = ident->owner_scope;
	Unit * unit  = ident->location->unit;
	BL_ASSERT(unit);
	BL_ASSERT(scope);

	MirInstr *ref =
	    append_instr_decl_ref(cnt, type_ref, unit, &ident->data.ident.id, scope, NULL);
	return ref;
}

MirInstr *
ast_type_fn(Context *cnt, Ast *type_fn)
{
	Ast *               ast_ret_type  = type_fn->data.type_fn.ret_type;
	TSmallArray_AstPtr *ast_arg_types = type_fn->data.type_fn.args;

	/* return type */
	MirInstr *ret_type = NULL;
	if (ast_ret_type) {
		ret_type = ast(cnt, ast_ret_type);
		ref_instr(ret_type);
	}

	TSmallArray_InstrPtr *args = NULL;
	if (ast_arg_types && ast_arg_types->size) {
		const usize c = ast_arg_types->size;
		args          = create_sarr(TSmallArray_InstrPtr, cnt->assembly);
		tsa_resize_InstrPtr(args, c);

		Ast *     ast_arg_type;
		MirInstr *arg;
		for (usize i = c; i-- > 0;) {
			ast_arg_type = ast_arg_types->data[i];
			arg          = ast(cnt, ast_arg_type);
			ref_instr(arg);
			args->data[i] = arg;
		}
	}

	return append_instr_type_fn(cnt, type_fn, ret_type, args);
}

MirInstr *
ast_type_arr(Context *cnt, Ast *type_arr)
{
	Ast *ast_elem_type = type_arr->data.type_arr.elem_type;
	Ast *ast_len       = type_arr->data.type_arr.len;
	BL_ASSERT(ast_elem_type && ast_len);

	MirInstr *len       = ast(cnt, ast_len);
	MirInstr *elem_type = ast(cnt, ast_elem_type);
	return append_instr_type_array(cnt, type_arr, elem_type, len);
}

MirInstr *
ast_type_slice(Context *cnt, Ast *type_slice)
{
	Ast *ast_elem_type = type_slice->data.type_arr.elem_type;
	BL_ASSERT(ast_elem_type);

	MirInstr *elem_type = ast(cnt, ast_elem_type);
	return append_instr_type_slice(cnt, type_slice, elem_type);
}

MirInstr *
ast_type_ptr(Context *cnt, Ast *type_ptr)
{
	Ast *ast_type = type_ptr->data.type_ptr.type;
	BL_ASSERT(ast_type && "invalid pointee type");
	MirInstr *type = ast(cnt, ast_type);
	BL_ASSERT(type);

	if (cnt->ast.enable_incomplete_decl_refs && type->kind == MIR_INSTR_DECL_REF) {
		/* Enable incomplete types for pointers to declarations. */
		((MirInstrDeclRef *)type)->accept_incomplete_type = true;
	}

	return append_instr_type_ptr(cnt, type_ptr, type);
}

MirInstr *
ast_type_vargs(Context *cnt, Ast *type_vargs)
{
	/* type is optional (Any will be used when no type was specified) */
	Ast *     ast_type = type_vargs->data.type_vargs.type;
	MirInstr *type     = ast(cnt, ast_type);
	return append_instr_type_vargs(cnt, type_vargs, type);
}

MirInstr *
ast_type_enum(Context *cnt, Ast *type_enum)
{
	TSmallArray_AstPtr *ast_variants  = type_enum->data.type_enm.variants;
	Ast *               ast_base_type = type_enum->data.type_enm.type;
	BL_ASSERT(ast_variants);

	const usize varc = ast_variants->size;
	if (varc == 0) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_EMPTY_ENUM,
		            type_enum->location,
		            BUILDER_CUR_WORD,
		            "Empty enumerator.");
		return NULL;
	}

	MirInstr *base_type = ast(cnt, ast_base_type);

	Scope *scope = type_enum->data.type_enm.scope;
	BL_ASSERT(scope);
	if (cnt->debug_mode) init_llvm_DI_scope(cnt, scope);

	TSmallArray_InstrPtr *variants = create_sarr(TSmallArray_InstrPtr, cnt->assembly);

	/* Build variant instructions */
	MirInstr *variant;
	Ast *     ast_variant;
	TSA_FOREACH(ast_variants, ast_variant)
	{
		variant = ast(cnt, ast_variant);
		BL_ASSERT(variant);
		tsa_push_InstrPtr(variants, variant);
	}

	/* Consume declaration identificator. */
	ID *id                     = cnt->ast.current_entity_id;
	cnt->ast.current_entity_id = NULL;

	return append_instr_type_enum(cnt, type_enum, id, scope, variants, base_type);
}

MirInstr *
ast_type_struct(Context *cnt, Ast *type_struct)
{
	/* Consume declaration identificator. */
	ID *id                     = cnt->ast.current_entity_id;
	cnt->ast.current_entity_id = NULL;

	/* Consume current struct fwd decl. */
	MirInstr *fwd_decl               = cnt->ast.current_fwd_struct_decl;
	cnt->ast.current_fwd_struct_decl = NULL;

	TSmallArray_AstPtr *ast_members = type_struct->data.type_strct.members;
	const bool          is_raw      = type_struct->data.type_strct.raw;
	if (is_raw) {
		BL_ABORT_ISSUE(31);
	}

	BL_ASSERT(ast_members);

	Ast *       ast_base_type = type_struct->data.type_strct.base_type;
	const usize memc          = ast_members->size;
	if (!memc && !ast_base_type) {
		builder_msg(BUILDER_MSG_ERROR,
		            ERR_EMPTY_STRUCT,
		            type_struct->location,
		            BUILDER_CUR_WORD,
		            "Empty structure.");
		return NULL;
	}

	TSmallArray_InstrPtr *members = create_sarr(TSmallArray_InstrPtr, cnt->assembly);
	Scope *               scope   = type_struct->data.type_strct.scope;
	BL_ASSERT(scope);

	if (ast_base_type) {
		/* Structure has base type, in such case we generate implicit first member 'base'.
		 */
		MirInstr *base_type = ast(cnt, ast_base_type);
		ID *      id        = &builtin_ids[MIR_BUILTIN_ID_STRUCT_BASE];
		base_type = append_instr_decl_member_impl(cnt, ast_base_type, id, base_type);

		MirMember *base_member = ((MirInstrDeclMember *)base_type)->member;
		base_member->is_base   = true;
		provide_builtin_member(cnt, scope, base_member);

		tsa_push_InstrPtr(members, base_type);
	}

	MirInstr *tmp = NULL;
	Ast *     ast_member;
	TSA_FOREACH(ast_members, ast_member)
	{
		tmp = ast(cnt, ast_member);
		BL_ASSERT(tmp);
		tsa_push_InstrPtr(members, tmp);
	}

	if (cnt->debug_mode) init_llvm_DI_scope(cnt, scope);

	return append_instr_type_struct(cnt, type_struct, id, fwd_decl, scope, members, false);
}

MirInstr *
ast_create_impl_fn_call(Context *   cnt,
                        Ast *       node,
                        const char *fn_name,
                        MirType *   fn_type,
                        bool        schedule_analyze)
{
	if (!node) return NULL;

	/* Sometimes we need to have implicit function return type based on
	 * resulting type of the AST expression, in such case we must allow return
	 * instruction to change function return
	 * type and create dummy type for the function. */
	MirType *final_fn_type  = fn_type;
	bool     infer_ret_type = false;

	if (!final_fn_type) {
		final_fn_type  = create_type_fn(cnt, NULL, NULL, NULL, false);
		infer_ret_type = true;
	}

	MirInstrBlock *prev_block = get_current_block(cnt);
	MirInstr *     fn_proto   = append_instr_fn_proto(cnt, NULL, NULL, NULL, schedule_analyze);
	fn_proto->value.type      = final_fn_type;

	MirFn *fn =
	    create_fn(cnt, NULL, NULL, fn_name, 0, (MirInstrFnProto *)fn_proto, false, true);
	mir_set_const_ptr(&fn_proto->value.data.v_ptr, fn, MIR_CP_FN);

	fn->type = final_fn_type;

	MirInstrBlock *entry = append_block(cnt, fn, "entry");
	set_current_block(cnt, entry);

	MirInstr *result = ast(cnt, node);
	/* Guess return type here when it is based on expression result... */
	append_instr_ret(cnt, NULL, result, infer_ret_type);

	set_current_block(cnt, prev_block);
	return create_instr_call_comptime(cnt, node, fn_proto);
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
	case AST_TEST_CASE:
		ast_test_case(cnt, node);
		break;
	case AST_UNREACHABLE:
		ast_unrecheable(cnt, node);
		break;
	case AST_STMT_DEFER:
		ast_stmt_defer(cnt, node);
		break;
	case AST_STMT_RETURN:
		ast_stmt_return(cnt, node);
		break;
	case AST_STMT_LOOP:
		ast_stmt_loop(cnt, node);
		break;
	case AST_STMT_BREAK:
		ast_stmt_break(cnt, node);
		break;
	case AST_STMT_CONTINUE:
		ast_stmt_continue(cnt, node);
		break;
	case AST_STMT_IF:
		ast_stmt_if(cnt, node);
		break;
	case AST_STMT_SWITCH:
		ast_stmt_switch(cnt, node);
		break;
	case AST_DECL_ENTITY:
		return ast_decl_entity(cnt, node);
	case AST_DECL_ARG:
		return ast_decl_arg(cnt, node);
	case AST_DECL_MEMBER:
		return ast_decl_member(cnt, node);
	case AST_DECL_VARIANT:
		return ast_decl_variant(cnt, node);
	case AST_TYPE_REF:
		return ast_type_ref(cnt, node);
	case AST_TYPE_STRUCT:
		return ast_type_struct(cnt, node);
	case AST_TYPE_FN:
		return ast_type_fn(cnt, node);
	case AST_TYPE_ARR:
		return ast_type_arr(cnt, node);
	case AST_TYPE_SLICE:
		return ast_type_slice(cnt, node);
	case AST_TYPE_PTR:
		return ast_type_ptr(cnt, node);
	case AST_TYPE_VARGS:
		return ast_type_vargs(cnt, node);
	case AST_TYPE_ENUM:
		return ast_type_enum(cnt, node);
	case AST_EXPR_FILE:
		return ast_expr_file(cnt, node);
	case AST_EXPR_LINE:
		return ast_expr_line(cnt, node);
	case AST_EXPR_ADDROF:
		return ast_expr_addrof(cnt, node);
	case AST_EXPR_CAST:
		return ast_expr_cast(cnt, node);
	case AST_EXPR_SIZEOF:
		return ast_expr_sizeof(cnt, node);
	case AST_EXPR_ALIGNOF:
		return ast_expr_alignof(cnt, node);
	case AST_EXPR_DEREF:
		return ast_expr_deref(cnt, node);
	case AST_EXPR_LIT_INT:
		return ast_expr_lit_int(cnt, node);
	case AST_EXPR_LIT_FLOAT:
		return ast_expr_lit_float(cnt, node);
	case AST_EXPR_LIT_DOUBLE:
		return ast_expr_lit_double(cnt, node);
	case AST_EXPR_LIT_BOOL:
		return ast_expr_lit_bool(cnt, node);
	case AST_EXPR_LIT_FN:
		return ast_expr_lit_fn(cnt, node, NULL, false, 0);
	case AST_EXPR_LIT_STRING:
		return ast_expr_lit_string(cnt, node);
	case AST_EXPR_LIT_CHAR:
		return ast_expr_lit_char(cnt, node);
	case AST_EXPR_BINOP:
		return ast_expr_binop(cnt, node);
	case AST_EXPR_UNARY:
		return ast_expr_unary(cnt, node);
	case AST_EXPR_REF:
		return ast_expr_ref(cnt, node);
	case AST_EXPR_CALL:
		return ast_expr_call(cnt, node);
	case AST_EXPR_ELEM:
		return ast_expr_elem(cnt, node);
	case AST_EXPR_NULL:
		return ast_expr_null(cnt, node);
	case AST_EXPR_MEMBER:
		return ast_expr_member(cnt, node);
	case AST_EXPR_TYPE:
		return ast_expr_type(cnt, node);
	case AST_EXPR_COMPOUND:
		return ast_expr_compound(cnt, node);
	case AST_EXPR_TYPE_INFO:
		return ast_expr_type_info(cnt, node);

	case AST_LOAD:
	case AST_LINK:
	case AST_PRIVATE:
		break;
	default:
		BL_ABORT("invalid node %s", ast_get_name(node));
	}

	return NULL;
}

const char *
mir_instr_name(MirInstr *instr)
{
	if (!instr) return "unknown";
	switch (instr->kind) {
	case MIR_INSTR_INVALID:
		return "InstrInvalid";
	case MIR_INSTR_BLOCK:
		return "InstrBlock";
	case MIR_INSTR_DECL_VAR:
		return "InstrDeclVar";
	case MIR_INSTR_DECL_MEMBER:
		return "InstrDeclMember";
	case MIR_INSTR_DECL_ARG:
		return "InstrDeclArg";
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
	case MIR_INSTR_FN_PROTO:
		return "InstrFnProto";
	case MIR_INSTR_CALL:
		return "InstrCall";
	case MIR_INSTR_DECL_REF:
		return "InstrDeclRef";
	case MIR_INSTR_DECL_DIRECT_REF:
		return "InstrDeclDirectRef";
	case MIR_INSTR_UNREACHABLE:
		return "InstrUnreachable";
	case MIR_INSTR_TYPE_FN:
		return "InstrTypeFn";
	case MIR_INSTR_TYPE_STRUCT:
		return "InstrTypeStruct";
	case MIR_INSTR_TYPE_ARRAY:
		return "InstrTypeArray";
	case MIR_INSTR_TYPE_SLICE:
		return "InstrTypeSlice";
	case MIR_INSTR_TYPE_VARGS:
		return "InstrTypeVArgs";
	case MIR_INSTR_COND_BR:
		return "InstrCondBr";
	case MIR_INSTR_BR:
		return "InstrBr";
	case MIR_INSTR_UNOP:
		return "InstrUnop";
	case MIR_INSTR_ARG:
		return "InstrArg";
	case MIR_INSTR_ELEM_PTR:
		return "InstrElemPtr";
	case MIR_INSTR_TYPE_PTR:
		return "InstrTypePtr";
	case MIR_INSTR_ADDROF:
		return "InstrAddrOf";
	case MIR_INSTR_MEMBER_PTR:
		return "InstrMemberPtr";
	case MIR_INSTR_CAST:
		return "InstrCast";
	case MIR_INSTR_SIZEOF:
		return "InstrSizeof";
	case MIR_INSTR_ALIGNOF:
		return "InstrAlignof";
	case MIR_INSTR_COMPOUND:
		return "InstrCompound";
	case MIR_INSTR_VARGS:
		return "InstrVArgs";
	case MIR_INSTR_TYPE_INFO:
		return "InstrTypeInfo";
	case MIR_INSTR_PHI:
		return "InstrPhi";
	case MIR_INSTR_TYPE_ENUM:
		return "InstrTypeEnum";
	case MIR_INSTR_DECL_VARIANT:
		return "InstrDeclVariant";
	case MIR_INSTR_TOANY:
		return "InstrToAny";
	case MIR_INSTR_SWITCH:
		return "InstrSwitch";
	}

	return "UNKNOWN";
}

/* public */
static void
_type_to_str(char *buf, usize len, MirType *type, bool prefer_name)
{
	/******************************************************************************************/
#define append_buf(buf, len, str)                                                                  \
	{                                                                                          \
		const usize filled = strlen(buf);                                                  \
		snprintf((buf) + filled, (len)-filled, "%s", str);                                 \
	}
	/******************************************************************************************/

	if (!buf) return;
	if (!type) {
		append_buf(buf, len, "<unknown>");
		return;
	}

	if (type->user_id && prefer_name) {
		append_buf(buf, len, type->user_id->str);
		return;
	}

	switch (type->kind) {
	case MIR_TYPE_TYPE:
		append_buf(buf, len, "type");
		break;

	case MIR_TYPE_SLICE: {
		const bool has_members = (bool)type->data.strct.members;
		append_buf(buf, len, "[]");

		if (has_members) {
			MirType *tmp = mir_get_struct_elem_type(type, MIR_SLICE_PTR_INDEX);
			tmp          = mir_deref_type(tmp);
			_type_to_str(buf, len, tmp, true);
		}
		break;
	}

	case MIR_TYPE_VARGS: {
		const bool has_members = (bool)type->data.strct.members;
		append_buf(buf, len, "...");

		if (has_members) {
			MirType *tmp = mir_get_struct_elem_type(type, MIR_SLICE_PTR_INDEX);
			tmp          = mir_deref_type(tmp);
			_type_to_str(buf, len, tmp, true);
		}
		break;
	}

	case MIR_TYPE_STRUCT: {
		TSmallArray_MemberPtr *members = type->data.strct.members;
		MirMember *            tmp;

		append_buf(buf, len, "struct{");
		if (members) {
			TSA_FOREACH(members, tmp)
			{
				_type_to_str(buf, len, tmp->type, true);
				if (i < members->size - 1) append_buf(buf, len, ", ");
			}
		}
		append_buf(buf, len, "}");

		break;
	}

	case MIR_TYPE_ENUM: {
		TSmallArray_VariantPtr *variants = type->data.enm.variants;
		append_buf(buf, len, "enum{");

		if (variants) {
			MirVariant *variant;
			TSA_FOREACH(variants, variant)
			{
				append_buf(buf, len, variant->id->str);
				append_buf(buf, len, " :: ");

				if (variant->value) {
					char value_str[35];
					snprintf(value_str,
					         TARRAY_SIZE(value_str),
					         "%lld",
					         (long long)variant->value->data.v_s64);
					append_buf(buf, len, value_str);
				} else {
					append_buf(buf, len, "<invalid>");
				}

				if (i < variants->size - 1) append_buf(buf, len, ", ");
			}
		}
		append_buf(buf, len, "}");
		break;
	}

	case MIR_TYPE_FN: {
		append_buf(buf, len, "fn(");

		MirArg *            it;
		TSmallArray_ArgPtr *args = type->data.fn.args;
		if (args) {
			TSA_FOREACH(args, it)
			{
				_type_to_str(buf, len, it->type, true);
				if (i < args->size - 1) append_buf(buf, len, ", ");
			}
		}

		append_buf(buf, len, ") ");

		_type_to_str(buf, len, type->data.fn.ret_type, true);
		break;
	}

	case MIR_TYPE_PTR: {
		append_buf(buf, len, "*");
		_type_to_str(buf, len, mir_deref_type(type), prefer_name);
		break;
	}

	case MIR_TYPE_ARRAY: {
		char str[35];
		sprintf(str, "[%llu]", (unsigned long long)type->data.array.len);
		append_buf(buf, len, str);

		_type_to_str(buf, len, type->data.array.elem_type, true);
		break;
	}

	default:
		if (type->user_id) {
			append_buf(buf, len, type->user_id->str);
		} else {
			append_buf(buf, len, "<invalid>");
		}
	}
#undef append_buf
}

void
mir_type_to_str(char *buf, usize len, MirType *type, bool prefer_name)
{
	if (!buf || !len) return;
	buf[0] = '\0';
	_type_to_str(buf, len, type, prefer_name);
}

void
execute_entry_fn(Context *cnt)
{
	msg_log("\nExecuting 'main' in compile time...");
	if (!cnt->entry_fn) {
		msg_error("Assembly '%s' has no entry function!", cnt->assembly->name);
		return;
	}

	MirType *fn_type = cnt->entry_fn->type;
	BL_ASSERT(fn_type && fn_type->kind == MIR_TYPE_FN);

	/* INCOMPLETE: support passing of arguments. */
	if (fn_type->data.fn.args) {
		msg_error("Main function expects arguments, this is not supported yet!");
		return;
	}

	/* tmp return value storage */
	VMStackPtr ret_ptr = NULL;
	if (vm_execute_fn(&cnt->vm, cnt->entry_fn, &ret_ptr)) {
		if (ret_ptr) {
			MirConstValue tmp = {.type = fn_type->data.fn.ret_type};
			vm_read_stack_value(&tmp, ret_ptr);
			msg_log("Execution finished with state: %lld\n", (long long)tmp.data.v_s64);
		} else {
			msg_log("Execution finished without errors");
		}
	} else {
		msg_log("Execution finished with errors");
	}
}

void
execute_test_cases(Context *cnt)
{
	msg_log("\nExecuting test cases...");

	const usize c      = cnt->test_cases.size;
	s32         failed = 0;
	MirFn *     test_fn;
	s32         line;
	const char *file;

	TARRAY_FOREACH(MirFn *, &cnt->test_cases, test_fn)
	{
		BL_ASSERT(IS_FLAG(test_fn->flags, FLAG_TEST));
		const bool passed = vm_execute_fn(&cnt->vm, test_fn, NULL);

		line = test_fn->decl_node ? test_fn->decl_node->location->line : -1;
		file = test_fn->decl_node ? test_fn->decl_node->location->unit->filepath : "?";

		msg_log("[ %s ] (%llu/%llu) %s:%d '%s'",
		        passed ? GREEN("PASSED") : RED("FAILED"),
		        (unsigned long long)i + 1,
		        (unsigned long long)c,
		        file,
		        line,
		        test_fn->test_case_desc);

		if (!passed) ++failed;
	}

	{
		s32 perc = c > 0 ? (s32)((f32)(c - failed) / (c * 0.01f)) : 100;

		msg_log("------------------------------------------------------------------"
		        "--------"
		        "------");
		if (perc == 100) {
			msg_log("Testing done, %d of %zu failed. Completed: " GREEN("%d%%"),
			        failed,
			        c,
			        perc);
		} else {
			msg_log("Testing done, %d of %zu failed. Completed: " RED("%d%%"),
			        failed,
			        c,
			        perc);
		}
		msg_log("------------------------------------------------------------------"
		        "--------"
		        "------");
	}
}

void
init_builtins(Context *cnt)
{
#define PROVIDE(N) provide_builtin_type(cnt, bt->t_##N)

	// initialize all hashes once
	for (s32 i = 0; i < _MIR_BUILTIN_ID_COUNT; ++i) {
		builtin_ids[i].hash = thash_from_str(builtin_ids[i].str);
	}

	struct BuiltinTypes *bt = &cnt->builtin_types;

	bt->t_type   = create_type_type(cnt);
	bt->t_void   = create_type_void(cnt);
	bt->t_s8     = create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_S8], 8, true);
	bt->t_s16    = create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_S16], 16, true);
	bt->t_s32    = create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_S32], 32, true);
	bt->t_s64    = create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_S64], 64, true);
	bt->t_u8     = create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_U8], 8, false);
	bt->t_u16    = create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_U16], 16, false);
	bt->t_u32    = create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_U32], 32, false);
	bt->t_u64    = create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_U64], 64, false);
	bt->t_usize  = create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_USIZE], 64, false);
	bt->t_bool   = create_type_bool(cnt);
	bt->t_f32    = create_type_real(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_F32], 32);
	bt->t_f64    = create_type_real(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_F64], 64);
	bt->t_u8_ptr = create_type_ptr(cnt, bt->t_u8);
	bt->t_string = create_type_struct_special(
	    cnt, MIR_TYPE_STRING, &builtin_ids[MIR_BUILTIN_ID_TYPE_STRING], bt->t_u8_ptr);

	bt->t_string_ptr = create_type_ptr(cnt, bt->t_string);

	bt->t_string_slice =
	    create_type_struct_special(cnt, MIR_TYPE_SLICE, NULL, bt->t_string_ptr);

	bt->t_resolve_type_fn = create_type_fn(cnt, NULL, bt->t_type, NULL, false);
	bt->t_test_case_fn    = create_type_fn(cnt, NULL, bt->t_void, NULL, false);

	/* Provide types into global scope */
	PROVIDE(type);
	PROVIDE(s8);
	PROVIDE(s16);
	PROVIDE(s32);
	PROVIDE(s64);
	PROVIDE(u8);
	PROVIDE(u16);
	PROVIDE(u32);
	PROVIDE(u64);
	PROVIDE(usize);
	PROVIDE(bool);
	PROVIDE(f32);
	PROVIDE(f64);
	PROVIDE(string);

#undef PROVIDE
}

ptrdiff_t
mir_get_struct_elem_offest(Assembly *assembly, MirType *type, u32 i)
{
	BL_ASSERT(mir_is_composit_type(type) && "Expected structure type");
	return (ptrdiff_t)LLVMOffsetOfElement(assembly->llvm.TD, type->llvm_type, i);
}

ptrdiff_t
mir_get_array_elem_offset(MirType *type, u32 i)
{
	BL_ASSERT(type->kind == MIR_TYPE_ARRAY && "Expected array type");
	MirType *elem_type = type->data.array.elem_type;
	BL_ASSERT(elem_type);
	return (ptrdiff_t)elem_type->store_size_bytes * i;
}

void
mir_arenas_init(MirArenas *arenas)
{
	arena_init(&arenas->instr, SIZEOF_MIR_INSTR, ARENA_CHUNK_COUNT, NULL);
	arena_init(&arenas->type, sizeof(MirType), ARENA_CHUNK_COUNT, NULL);
	arena_init(&arenas->var, sizeof(MirVar), ARENA_CHUNK_COUNT, NULL);
	arena_init(&arenas->fn, sizeof(MirFn), ARENA_CHUNK_COUNT, (ArenaElemDtor)&fn_dtor);
	arena_init(&arenas->member, sizeof(MirMember), ARENA_CHUNK_COUNT, NULL);
	arena_init(&arenas->variant, sizeof(MirVariant), ARENA_CHUNK_COUNT, NULL);
	arena_init(&arenas->value, sizeof(MirConstValue), ARENA_CHUNK_COUNT / 2, NULL);
	arena_init(&arenas->arg, sizeof(MirArg), ARENA_CHUNK_COUNT / 2, NULL);
}

void
mir_arenas_terminate(MirArenas *arenas)
{
	arena_terminate(&arenas->fn);
	arena_terminate(&arenas->instr);
	arena_terminate(&arenas->member);
	arena_terminate(&arenas->type);
	arena_terminate(&arenas->value);
	arena_terminate(&arenas->var);
	arena_terminate(&arenas->variant);
	arena_terminate(&arenas->arg);
}

void
mir_run(Assembly *assembly)
{
	Context cnt;
	memset(&cnt, 0, sizeof(Context));
	cnt.assembly                = assembly;
	cnt.debug_mode              = builder.options.debug_build;
	cnt.analyze.verbose_pre     = false;
	cnt.analyze.verbose_post    = false;
	cnt.analyze.llvm_di_builder = assembly->llvm.di_builder;

	thtbl_init(&cnt.analyze.waiting, sizeof(TArray), ANALYZE_TABLE_SIZE);
	thtbl_init(&cnt.analyze.RTTI_entry_types, 0, 1024);
	tlist_init(&cnt.analyze.queue, sizeof(MirInstr *));
	tstring_init(&cnt.tmp_sh);
	tarray_init(&cnt.test_cases, sizeof(MirFn *));
	vm_init(&cnt.vm, assembly, VM_STACK_SIZE);

	tsa_init(&cnt.ast.defer_stack);

	/* initialize all builtin types */
	init_builtins(&cnt);

	/* Gen MIR from AST pass */
	Unit *unit;
	TARRAY_FOREACH(Unit *, &assembly->units, unit) ast(&cnt, unit->ast);

	if (builder.errorc) goto SKIP;

	/* Skip analyze if no_analyze is set by user. */
	if (builder.options.no_analyze) goto SKIP;

	/* Analyze pass */
	analyze(&cnt);
	analyze_report_unresolved(&cnt);

	if (builder.errorc) goto SKIP;

	gen_RTTI_types(&cnt);

	/* Move those as separate stages! */
	if (builder.options.run_tests) execute_test_cases(&cnt);
	if (builder.options.run) execute_entry_fn(&cnt);

SKIP:
	tlist_terminate(&cnt.analyze.queue);
	thtbl_terminate(&cnt.analyze.waiting);
	thtbl_terminate(&cnt.analyze.RTTI_entry_types);
	tarray_terminate(&cnt.test_cases);
	tstring_terminate(&cnt.tmp_sh);

	tsa_terminate(&cnt.ast.defer_stack);

	vm_terminate(&cnt.vm);
}
