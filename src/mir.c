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
#define DEFAULT_EXEC_FRAME_STACK_SIZE   2097152 // 2MB
#define DEFAULT_EXEC_CALL_STACK_NESTING 10000
#define MAX_ALIGNMENT                   8
#define NO_REF_COUNTING                 -1
#define VERBOSE_EXEC                    false 
#define VERBOSE_ANALYZE                 false
#define CHCK_STACK                      true
// clang-format on

// Debug helpers
#if BL_DEBUG && VERBOSE_EXEC
#define _log_push_ra                                                                               \
	{                                                                                          \
		if (instr) {                                                                       \
			fprintf(stdout,                                                            \
			        "%6llu %20s  PUSH RA\n",                                           \
			        cnt->exec.stack->pc->id,                                           \
			        mir_instr_name(cnt->exec.stack->pc));                              \
		} else {                                                                           \
			fprintf(stdout, "     - %20s  PUSH RA\n", "Terminal");                     \
		}                                                                                  \
	}

#define _log_pop_ra                                                                                \
	{                                                                                          \
		fprintf(stdout,                                                                    \
		        "%6llu %20s  POP RA\n",                                                    \
		        cnt->exec.stack->pc->id,                                                   \
		        mir_instr_name(cnt->exec.stack->pc));                                      \
	}

#define _log_push_stack                                                                            \
	{                                                                                          \
		char type_name[256];                                                               \
		mir_type_to_str(type_name, 256, type, true);                                       \
		if (cnt->exec.stack->pc) {                                                         \
			fprintf(stdout,                                                            \
			        "%6llu %20s  PUSH    (%luB, %p) %s\n",                             \
			        (unsigned long long)cnt->exec.stack->pc->id,                       \
			        mir_instr_name(cnt->exec.stack->pc),                               \
			        size,                                                              \
			        tmp,                                                               \
			        type_name);                                                        \
		} else {                                                                           \
			fprintf(stdout,                                                            \
			        "     -                       PUSH    (%luB, %p) %s\n",            \
			        size,                                                              \
			        tmp,                                                               \
			        type_name);                                                        \
		}                                                                                  \
	}

#define _log_pop_stack                                                                             \
	{                                                                                          \
		char type_name[256];                                                               \
		mir_type_to_str(type_name, 256, type, true);                                       \
		fprintf(stdout,                                                                    \
		        "%6llu %20s  POP     (%luB, %p) %s\n",                                     \
		        cnt->exec.stack->pc->id,                                                   \
		        mir_instr_name(cnt->exec.stack->pc),                                       \
		        size,                                                                      \
		        cnt->exec.stack->top_ptr - size,                                           \
		        type_name);                                                                \
	}

#else
#define _log_push_ra
#define _log_pop_ra
#define _log_push_stack
#define _log_pop_stack
#endif

#if BL_DEBUG && CHCK_STACK
#define _chck_size() sizeof(void *)
#define _chck_write(_ptr, _data_size) memcpy((_ptr) + (_data_size), &(_ptr), _chck_size())
#define _chck_validate(_ptr, _data_size)                                                           \
	if ((*(intptr_t *)((_ptr) + (_data_size))) != (intptr_t)(_ptr)) {                          \
		bl_abort("Stack memory malformed!");                                               \
	}
#else
#define _chck_size() 0
#define _chck_write(_ptr, _data_size)                                                              \
	while (0) {                                                                                \
	}

#define _chck_validate(_ptr, _data_size)                                                           \
	while (0) {                                                                                \
	}
#endif

union _MirInstr {
	MirInstrBlock         block;
	MirInstrDeclVar       var;
	MirInstrDeclMember    member;
	MirInstrDeclVariant   variant;
	MirInstrConst         cnst;
	MirInstrLoad          load;
	MirInstrStore         store;
	MirInstrRet           ret;
	MirInstrBinop         binop;
	MirInstrFnProto       fn_proto;
	MirInstrDeclRef       decl_ref;
	MirInstrDeclDirectRef decl_direct_ref;
	MirInstrCall          call;
	MirInstrUnreachable   unreachable;
	MirInstrCondBr        cond_br;
	MirInstrBr            br;
	MirInstrUnop          unop;
	MirInstrArg           arg;
	MirInstrElemPtr       elem_ptr;
	MirInstrMemberPtr     member_ptr;
	MirInstrAddrOf        addrof;
	MirInstrTypeArray     type_array;
	MirInstrTypeSlice     type_slice;
	MirInstrTypeVArgs     type_vargs;
	MirInstrTypePtr       type_ptr;
	MirInstrTypeStruct    type_struct;
	MirInstrTypeFn        type_fn;
	MirInstrTypeEnum      type_enum;
	MirInstrCast          cast;
	MirInstrSizeof        szof;
	MirInstrAlignof       alof;
	MirInstrCompound      init;
	MirInstrVArgs         vargs;
	MirInstrTypeInfo      type_info;
	MirInstrPhi           phi;
	MirInstrToAny         toany;
};

typedef struct MirFrame {
	struct MirFrame *prev;
	MirInstr *       callee;
} MirFrame;

typedef struct MirStack {
	MirStackPtr    top_ptr;         /* pointer to top of the stack */
	size_t         used_bytes;      /* size of the used stack in bytes */
	size_t         allocated_bytes; /* total allocated size of the stack in bytes */
	MirFrame *     ra;              /* current frame beginning (return address)*/
	MirInstr *     pc;              /* currently executed instruction */
	MirInstrBlock *prev_block;      /* used by phi instruction */
	bool           aborted;         /* true when execution was aborted */
} MirStack;

SmallArrayType(LLVMType, LLVMTypeRef, 8);
SmallArrayType(LLVMMetadata, LLVMMetadataRef, 16);
SmallArrayType(DeferStack, Ast *, 64);
SmallArrayType(Instr64, MirInstr *, 64);

typedef struct {
	Builder *   builder;
	Assembly *  assembly;
	BArray *    test_cases;
	BString *   tmp_sh;
	BHashTable *type_table;
	MirFn *     entry_fn;
	bool        debug_mode;

	/* AST -> MIR generation */
	struct {
		SmallArray_DeferStack defer_stack;
		MirInstrBlock *       current_block;
		MirInstrBlock *       break_block;
		MirInstrBlock *       exit_block;
		MirInstrBlock *       continue_block;
		ID *                  current_entity_id; /* Sometimes used for named structures */
	} ast;

	/* Analyze MIR generated from AST */
	struct {
		/* Instructions waiting for analyze. */
		BList *queue;

		/* Hash table of arrays. Hash is ID of symbol and array contains queue
		 * of waiting instructions (DeclRefs). */
		BHashTable *waiting;
		bool        verbose_pre;
		bool        verbose_post;

		BHashTable *     RTTI_entry_types;
		LLVMDIBuilderRef llvm_di_builder;
	} analyze;

	/* MIR compile time execution. */
	struct {
		/* stack header is also allocated on the stack :) */
		MirStack *stack;
	} exec;

	/* Builtins */
	struct BuiltinTypes {
		/* PROVIDED BEGIN */
		/* TODO: replace those entries with lookup_buildin call... */
		/* TODO: replace those entries with lookup_buildin call... */
		/* TODO: replace those entries with lookup_buildin call... */
		/* TODO: replace those entries with lookup_buildin call... */
		MirType *entry_type;
		MirType *entry_s8;
		MirType *entry_s16;
		MirType *entry_s32;
		MirType *entry_s64;
		MirType *entry_u8;
		MirType *entry_u16;
		MirType *entry_u32;
		MirType *entry_u64;
		MirType *entry_usize;
		MirType *entry_bool;
		MirType *entry_f32;
		MirType *entry_f64;
		MirType *entry_string;
		/* PROVIDED END */

		/* OTHER BEGIN */
		MirType *entry_void;
		MirType *entry_u8_ptr;
		MirType *entry_resolve_type_fn;
		MirType *entry_test_case_fn;
		MirType *entry_TypeInfo_ptr;
		MirType *entry_TypeInfo_slice;
		/* OTHER END */

		/* Cache scope containing '#compiler' flagged symbols.  */
		Scope *cache;
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
	ANALYZE_POSTPONE = 2
} AnalyzeResult;

/* Ids of builtin symbols, hash is calculated inside init_builtins function
 * later. */
// clang-format off
static ID builtin_ids[_MIR_BUILTIN_ID_COUNT] = {
    {.str = "type",           .hash = 0},
    {.str = "s8",             .hash = 0},
    {.str = "s16",            .hash = 0},
    {.str = "s32",            .hash = 0},
    {.str = "s64",            .hash = 0},
    {.str = "u8",             .hash = 0},
    {.str = "u16",            .hash = 0},
    {.str = "u32",            .hash = 0},
    {.str = "u64",            .hash = 0},
    {.str = "usize",          .hash = 0},
    {.str = "bool",           .hash = 0},
    {.str = "f32",            .hash = 0},
    {.str = "f64",            .hash = 0},
    {.str = "void",           .hash = 0},
    {.str = "string",         .hash = 0},
    {.str = "null_t",         .hash = 0},
    {.str = "main",           .hash = 0},
    {.str = "len",            .hash = 0},
    {.str = "ptr",            .hash = 0},
    {.str = "Any",            .hash = 0},
    {.str = "TypeKind",       .hash = 0},
    {.str = "TypeInfo",       .hash = 0},
    {.str = "TypeInfoType",   .hash = 0},
    {.str = "TypeInfoVoid",   .hash = 0},
    {.str = "TypeInfoInt",    .hash = 0},
    {.str = "TypeInfoReal",   .hash = 0},
    {.str = "TypeInfoFn",     .hash = 0},
    {.str = "TypeInfoPtr",    .hash = 0},
    {.str = "TypeInfoBool",   .hash = 0},
    {.str = "TypeInfoArray",  .hash = 0},
    {.str = "TypeInfoStruct", .hash = 0},
    {.str = "TypeInfoEnum",   .hash = 0},
    {.str = "TypeInfoNull",   .hash = 0},
    {.str = "TypeInfoString", .hash = 0},
    {.str = "TypeInfoSlice",  .hash = 0},
    {.str = "TypeInfoVArgs",  .hash = 0},
};
// clang-format on

/* FW decls */
static void
init_builtins(Context *cnt);

static void
execute_entry_fn(Context *cnt);

static void
execute_test_cases(Context *cnt);

static ScopeEntry *
register_symbol(Context *cnt, Ast *node, ID *id, Scope *scope, bool is_builtin, bool enable_groups);

static void
cache_builtin(Context *cnt, ScopeEntry *entry);

static MirType *
lookup_builtin(Context *cnt, MirBuiltinIdKind kind);

/* ctors */
static bool
create_type(Context *cnt, MirType **out_type, const char *sh);

static MirType *
create_type_type(Context *cnt);

static MirType *
create_type_null(Context *cnt, MirType *base_type);

static MirType *
create_type_void(Context *cnt);

static MirType *
create_type_bool(Context *cnt);

static MirType *
create_type_int(Context *cnt, ID *id, int32_t bitcount, bool is_signed);

static MirType *
create_type_real(Context *cnt, ID *id, int32_t bitcount);

static MirType *
create_type_ptr(Context *cnt, MirType *src_type);

static MirType *
create_type_fn(Context *cnt, MirType *ret_type, SmallArray_Type *arg_types, bool is_vargs);

static MirType *
create_type_array(Context *cnt, MirType *elem_type, size_t len);

static MirType *
create_type_struct(Context *          cnt,
                   MirTypeKind        kind,
                   ID *               id,
                   Scope *            scope,
                   SmallArray_Member *members, /* MirMember */
                   bool               is_packed);

static MirType *
create_type_enum(Context *           cnt,
                 ID *                id,
                 Scope *             scope,
                 MirType *           base_type,
                 SmallArray_Variant *variants);

MirType *
create_type_struct_special(Context *cnt, MirTypeKind kind, ID *id, MirType *elem_ptr_type);

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
           int32_t  order, /* pass -1 if none */
           uint32_t flags);

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
          const char *     llvm_name,
          int32_t          flags,
          MirInstrFnProto *prototype,
          bool             emit_llvm);

static MirMember *
create_member(Context *cnt, Ast *node, ID *id, Scope *scope, int64_t index, MirType *type);

static MirVariant *
create_variant(Context *cnt, Ast *node, ID *id, Scope *scope, MirConstValue *value);

static MirConstValue *
create_const_value(Context *cnt, MirType *type);

static MirInstrBlock *
append_block(Context *cnt, MirFn *fn, const char *name);

/* instructions */
static void
maybe_mark_as_unrechable(MirInstrBlock *block, MirInstr *instr);

static void
append_current_block(Context *cnt, MirInstr *instr);

static MirInstr *
insert_instr_load(Context *cnt, MirInstr *src);

static MirInstr *
insert_instr_cast(Context *cnt, MirInstr *src, MirType *to_type);

static MirInstr *
insert_instr_toany(Context *cnt, MirInstr *expr);

static MirCastOp
get_cast_op(MirType *from, MirType *to);

#define create_instr(_cnt, _kind, _node, _t) ((_t)_create_instr((_cnt), (_kind), (_node)))

static MirInstr *
_create_instr(Context *cnt, MirInstrKind kind, Ast *node);

static MirInstr *
create_instr_call_comptime(Context *cnt, Ast *node, MirInstr *fn);

static MirInstr *
append_instr_arg(Context *cnt, Ast *node, unsigned i);

static MirInstr *
append_instr_phi(Context *cnt, Ast *node);

static MirInstr *
append_instr_compound(Context *cnt, Ast *node, MirInstr *type, SmallArray_Instr *values);

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
append_instr_load(Context *cnt, Ast *node, MirInstr *src);

static MirInstr *
append_instr_type_fn(Context *cnt, Ast *node, MirInstr *ret_type, SmallArray_Instr *arg_types);

static MirInstr *
append_instr_type_struct(Context *         cnt,
                         Ast *             node,
                         ID *              id,
                         Scope *           scope,
                         SmallArray_Instr *members,
                         bool              is_packed);

static MirInstr *
append_instr_type_enum(Context *         cnt,
                       Ast *             node,
                       ID *              id,
                       Scope *           scope,
                       SmallArray_Instr *variants,
                       MirInstr *        base_type);

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
append_instr_fn_proto(Context *cnt, Ast *node, MirInstr *type, MirInstr *user_type);

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
append_instr_call(Context *cnt, Ast *node, MirInstr *callee, SmallArray_Instr *args);

static MirInstr *
append_instr_decl_var(Context * cnt,
                      Ast *     node,
                      MirInstr *type,
                      MirInstr *init,
                      bool      is_mutable,
                      bool      is_in_gscope,
                      int32_t   order, /* -1 of none */
                      uint32_t  flags);

static MirInstr *
append_instr_decl_var_impl(Context *   cnt,
                           const char *name,
                           MirInstr *  type,
                           MirInstr *  init,
                           bool        is_mutable,
                           bool        is_in_gscope,
                           int32_t     order, /* -1 of none */
                           uint32_t    flags);

static MirInstr *
append_instr_decl_member(Context *cnt, Ast *node, MirInstr *type);

static MirInstr *
append_instr_decl_variant(Context *cnt, Ast *node, MirInstr *value);

static MirInstr *
create_instr_const_usize(Context *cnt, Ast *node, uint64_t val);

static MirInstr *
append_instr_const_int(Context *cnt, Ast *node, uint64_t val);

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
append_instr_ret(Context *cnt, Ast *node, MirInstr *value, bool allow_fn_ret_type_override);

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
create_instr_vargs_impl(Context *cnt, MirType *type, SmallArray_Instr *values);

/* ast */
static MirInstr *
ast_create_impl_fn_call(Context *cnt, Ast *node, const char *fn_name, MirType *fn_type);

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
ast_expr_lit_fn(Context *cnt, Ast *lit_fn);

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
static void
reduce_instr(Context *cnt, MirInstr *instr);

static uint64_t
analyze_instr(Context *cnt, MirInstr *instr);

MirInstr *
analyze_slot_input(Context * cnt,
                   bool *    out_valid,
                   MirInstr *input,
                   MirType * slot_type,
                   bool      enable_special_cast);

static uint64_t
analyze_instr_compound(Context *cnt, MirInstrCompound *init);

static uint64_t
analyze_instr_phi(Context *cnt, MirInstrPhi *phi);

static uint64_t
analyze_instr_toany(Context *cnt, MirInstrToAny *toany);

static uint64_t
analyze_instr_vargs(Context *cnt, MirInstrVArgs *vargs);

static uint64_t
analyze_instr_elem_ptr(Context *cnt, MirInstrElemPtr *elem_ptr);

static uint64_t
analyze_instr_member_ptr(Context *cnt, MirInstrMemberPtr *member_ptr);

static uint64_t
analyze_instr_addrof(Context *cnt, MirInstrAddrOf *addrof);

static uint64_t
analyze_instr_block(Context *cnt, MirInstrBlock *block);

static uint64_t
analyze_instr_ret(Context *cnt, MirInstrRet *ret);

static uint64_t
analyze_instr_arg(Context *cnt, MirInstrArg *arg);

static uint64_t
analyze_instr_unop(Context *cnt, MirInstrUnop *unop);

static uint64_t
analyze_instr_unreachable(Context *cnt, MirInstrUnreachable *unr);

static uint64_t
analyze_instr_cond_br(Context *cnt, MirInstrCondBr *br);

static uint64_t
analyze_instr_br(Context *cnt, MirInstrBr *br);

static uint64_t
analyze_instr_load(Context *cnt, MirInstrLoad *load);

static uint64_t
analyze_instr_store(Context *cnt, MirInstrStore *store);

static uint64_t
analyze_instr_fn_proto(Context *cnt, MirInstrFnProto *fn_proto);

static uint64_t
analyze_instr_type_fn(Context *cnt, MirInstrTypeFn *type_fn);

static uint64_t
analyze_instr_type_struct(Context *cnt, MirInstrTypeStruct *type_struct);

static uint64_t
analyze_instr_type_slice(Context *cnt, MirInstrTypeSlice *type_slice);

static uint64_t
analyze_instr_type_vargs(Context *cnt, MirInstrTypeVArgs *type_vargs);

static uint64_t
analyze_instr_type_ptr(Context *cnt, MirInstrTypePtr *type_ptr);

static uint64_t
analyze_instr_type_array(Context *cnt, MirInstrTypeArray *type_arr);

static uint64_t
analyze_instr_type_enum(Context *cnt, MirInstrTypeEnum *type_enum);

static uint64_t
analyze_instr_decl_var(Context *cnt, MirInstrDeclVar *var);

static uint64_t
analyze_instr_decl_member(Context *cnt, MirInstrDeclMember *decl);

static uint64_t
analyze_instr_decl_variant(Context *cnt, MirInstrDeclVariant *variant_instr);

static uint64_t
analyze_instr_decl_ref(Context *cnt, MirInstrDeclRef *ref);

static uint64_t
analyze_instr_decl_direct_ref(Context *cnt, MirInstrDeclDirectRef *ref);

static uint64_t
analyze_instr_const(Context *cnt, MirInstrConst *cnst);

static uint64_t
analyze_instr_call(Context *cnt, MirInstrCall *call);

static uint64_t
analyze_instr_cast(Context *cnt, MirInstrCast *cast);

static uint64_t
analyze_instr_sizeof(Context *cnt, MirInstrSizeof *szof);

static uint64_t
analyze_instr_type_info(Context *cnt, MirInstrTypeInfo *type_info);

static uint64_t
analyze_instr_alignof(Context *cnt, MirInstrAlignof *alof);

static uint64_t
analyze_instr_binop(Context *cnt, MirInstrBinop *binop);

static void
analyze(Context *cnt);

static void
analyze_report_unresolved(Context *cnt);

/* execute */
static void
exec_copy_comptime_to_stack(Context *cnt, MirStackPtr dest_ptr, MirConstValue *src_value);

static MirVar *
exec_gen_type_RTTI(Context *cnt, MirType *type);

static void
exec_gen_RTTI_types(Context *cnt);

static void
exec_instr(Context *cnt, MirInstr *instr);

static void
exec_instr_toany(Context *cnt, MirInstrToAny *toany);

static void
exec_instr_unreachable(Context *cnt, MirInstrUnreachable *unr);

static void
exec_instr_phi(Context *cnt, MirInstrPhi *phi);

static void
exec_instr_type_info(Context *cnt, MirInstrTypeInfo *type_info);

static void
exec_instr_cast(Context *cnt, MirInstrCast *cast);

static void
exec_instr_addrof(Context *cnt, MirInstrAddrOf *addrof);

static void
exec_instr_br(Context *cnt, MirInstrBr *br);

static void
exec_instr_elem_ptr(Context *cnt, MirInstrElemPtr *elem_ptr);

static void
exec_instr_member_ptr(Context *cnt, MirInstrMemberPtr *member_ptr);

static void
exec_instr_arg(Context *cnt, MirInstrArg *arg);

static void
exec_instr_cond_br(Context *cnt, MirInstrCondBr *br);

static void
exec_instr_load(Context *cnt, MirInstrLoad *load);

static void
exec_instr_store(Context *cnt, MirInstrStore *store);

static void
exec_instr_binop(Context *cnt, MirInstrBinop *binop);

static void
exec_instr_unop(Context *cnt, MirInstrUnop *unop);

static void
exec_instr_call(Context *cnt, MirInstrCall *call);

static MirConstValue *
exec_call_top_lvl(Context *cnt, MirInstrCall *call);

static void
exec_instr_type_slice(Context *cnt, MirInstrTypeSlice *type_slice);

static void
exec_instr_ret(Context *cnt, MirInstrRet *ret);

static void
exec_instr_compound(Context *cnt, MirStackPtr tmp_ptr, MirInstrCompound *init);

static void
exec_instr_vargs(Context *cnt, MirInstrVArgs *vargs);

static void
exec_instr_decl_var(Context *cnt, MirInstrDeclVar *var);

static void
exec_instr_decl_ref(Context *cnt, MirInstrDeclRef *ref);

static void
exec_instr_decl_direct_ref(Context *cnt, MirInstrDeclDirectRef *ref);

static bool
exec_fn(Context *cnt, MirFn *fn, SmallArray_Instr *args, MirConstValueData *out_value);

static MirConstValue *
exec_call_top_lvl(Context *cnt, MirInstrCall *call);

/* zero max nesting = unlimited nesting */
static void
exec_print_call_stack(Context *cnt, size_t max_nesting);

static MirStack *
exec_new_stack(size_t size);

static void
exec_delete_stack(MirStack *stack);

static void
exec_reset_stack(MirStack *stack);

static void
exec_copy_comptime_to_stack(Context *cnt, MirStackPtr dest_ptr, MirConstValue *src_value);

/* INLINES */
static inline bool
type_cmp(MirType *first, MirType *second)
{
	assert(first && second);
	return first->id.hash == second->id.hash;
}

static inline MirInstrBlock *
get_current_block(Context *cnt)
{
	return cnt->ast.current_block;
}

static inline MirFn *
get_current_fn(Context *cnt)
{
	return cnt->ast.current_block ? cnt->ast.current_block->owner_fn : NULL;
}

static inline void
terminate_block(MirInstrBlock *block, MirInstr *terminator)
{
	assert(block);
	if (block->terminal) bl_abort("basic block '%s' already terminated!", block->name);
	block->terminal = terminator;
}

static inline bool
is_block_terminated(MirInstrBlock *block)
{
	return block->terminal;
}

static inline bool
is_current_block_terminated(Context *cnt)
{
	return get_current_block(cnt)->terminal;
}

static inline ptrdiff_t
get_struct_elem_offest(Context *cnt, MirType *type, uint32_t i)
{
	assert(mir_is_composit_type(type) && "Expected structure type");
	return LLVMOffsetOfElement(cnt->assembly->llvm.TD, type->llvm_type, (unsigned long)i);
}

static inline ptrdiff_t
get_array_elem_offset(MirType *type, uint32_t i)
{
	assert(type->kind == MIR_TYPE_ARRAY && "Expected array type");
	MirType *elem_type = type->data.array.elem_type;
	assert(elem_type);
	return elem_type->store_size_bytes * i;
}

static inline void
schedule_RTTI_generation(Context *cnt, MirType *type)
{
	if (!bo_htbl_has_key(cnt->analyze.RTTI_entry_types, (uint64_t)type))
		bo_htbl_insert_empty(cnt->analyze.RTTI_entry_types, (uint64_t)type);
}

static inline bool
setup_instr_const_null(Context *cnt, MirInstr *instr, MirType *type)
{
	if (type->kind == MIR_TYPE_NULL) {
		instr->value.type = type;
		return true;
	}

	if (mir_is_pointer_type(type)) {
		instr->value.type = create_type_null(cnt, type);
		return true;
	}

	builder_msg(cnt->builder,
	            BUILDER_MSG_ERROR,
	            ERR_INVALID_TYPE,
	            instr->node->location,
	            BUILDER_CUR_WORD,
	            "Invalid use of null constant.");

	return false;
}

static inline bool
is_allocated_object(MirInstr *instr)
{
	if (instr->kind == MIR_INSTR_DECL_DIRECT_REF) return true;
	if (instr->kind == MIR_INSTR_DECL_REF) return true;
	if (instr->kind == MIR_INSTR_ELEM_PTR) return true;
	if (instr->kind == MIR_INSTR_MEMBER_PTR) return true;
	if (instr->kind == MIR_INSTR_FN_PROTO) return true;
	if (instr->kind == MIR_INSTR_COMPOUND) return true;

	return false;
}

static inline void
set_const_ptr(MirConstPtr *value, void *ptr, MirConstPtrKind kind)
{
	value->data.any = ptr;
	value->kind     = kind;
}

static inline MirInstr *
mutate_instr(MirInstr *instr, MirInstrKind kind)
{
	assert(instr);
	instr->kind = kind;
	return instr;
}

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

static inline void
insert_instr_after(MirInstr *after, MirInstr *instr)
{
	assert(after && instr);

	MirInstrBlock *block = after->owner_block;
	instr->unrechable    = after->unrechable;

	instr->next = after->next;
	instr->prev = after;
	if (after->next) after->next->prev = instr;
	after->next = instr;

	instr->owner_block = block;
	if (block->last_instr == after) instr->owner_block->last_instr = instr;
}

static inline void
insert_instr_before(MirInstr *before, MirInstr *instr)
{
	assert(before && instr);

	MirInstrBlock *block = before->owner_block;
	instr->unrechable    = before->unrechable;

	instr->next = before;
	instr->prev = before->prev;
	if (before->prev) before->prev->next = instr;
	before->prev = instr;

	instr->owner_block = block;
	if (block->entry_instr == before) instr->owner_block->entry_instr = instr;
}

static inline void
push_into_gscope(Context *cnt, MirInstr *instr)
{
	assert(instr);
	instr->id = bo_array_size(cnt->assembly->MIR.global_instrs);
	bo_array_push_back(cnt->assembly->MIR.global_instrs, instr);
};

static inline void
analyze_push_back(Context *cnt, MirInstr *instr)
{
	assert(instr);
	bo_list_push_back(cnt->analyze.queue, instr);
}

static inline void
analyze_push_front(Context *cnt, MirInstr *instr)
{
	assert(instr);
	bo_list_push_front(cnt->analyze.queue, instr);
}

static inline void
analyze_notify_provided(Context *cnt, uint64_t hash)
{
	bo_iterator_t iter = bo_htbl_find(cnt->analyze.waiting, hash);
	bo_iterator_t end  = bo_htbl_end(cnt->analyze.waiting);
	if (bo_iterator_equal(&iter, &end)) return; /* No one is waiting for this... */

#if BL_DEBUG && VERBOSE_ANALYZE
	printf("Analyze: Notify '%llu'.\n", (unsigned long long)hash);
#endif

	BArray *wq = bo_htbl_iter_peek_value(cnt->analyze.waiting, &iter, BArray *);
	assert(wq);

	MirInstr *instr;
	barray_foreach(wq, instr)
	{
		analyze_push_back(cnt, instr);
	}

	bo_htbl_erase(cnt->analyze.waiting, &iter);
}

static inline void
analyze_instr_rq(Context *cnt, MirInstr *instr)
{
	if (analyze_instr(cnt, instr) != ANALYZE_PASSED)
		bl_warning("invalid analyze of compiler-generated instruction: %s",
		           mir_instr_name(instr));
}

static inline const char *
gen_uq_name(Context *cnt, const char *prefix)
{
	static int32_t ui = 0;
	BString *      s  = builder_create_cached_str(cnt->builder);

	bo_string_append(s, prefix);
	char ui_str[22];
	sprintf(ui_str, ".%i", ui++);
	bo_string_append(s, ui_str);
	return bo_string_get(s);
}

/* Global variables are allocated in static data segment, so there is no need to
 * use relative pointer. When we set ignore to true original pointer is returned
 * as absolute pointer to the stack.  */
static inline MirStackPtr
exec_read_stack_ptr(Context *cnt, MirRelativeStackPtr rel_ptr, bool ignore)
{
	if (ignore) return (MirStackPtr)rel_ptr;
	assert(rel_ptr);

	MirStackPtr base = (MirStackPtr)cnt->exec.stack->ra;
	assert(base);
	return base + rel_ptr;
}

static inline void *
exec_read_value(MirConstValueData *dest, MirStackPtr src, MirType *type)
{
	assert(dest && src && type);
	const size_t size = type->store_size_bytes;
	return memcpy(dest, src, size);
}

static inline void
exec_abort(Context *cnt, int32_t report_stack_nesting)
{
	exec_print_call_stack(cnt, report_stack_nesting);
	cnt->exec.stack->aborted = true;
}

static inline size_t
exec_stack_alloc_size(size_t size)
{
	assert(size != 0);
	size += _chck_size();
	return size + (MAX_ALIGNMENT - (size % MAX_ALIGNMENT));
}

/* allocate memory on frame stack, size is in bits!!! */
static inline MirStackPtr
exec_stack_alloc(Context *cnt, size_t size)
{
	assert(size && "trying to allocate 0 bits on stack");

#if BL_DEBUG && CHCK_STACK
	const size_t orig_size = size;
#endif
	size = exec_stack_alloc_size(size);
	cnt->exec.stack->used_bytes += size;
	if (cnt->exec.stack->used_bytes > cnt->exec.stack->allocated_bytes) {
		msg_error("Stack overflow!!!");
		exec_abort(cnt, 10);
	}

	MirStackPtr mem          = (MirStackPtr)cnt->exec.stack->top_ptr;
	cnt->exec.stack->top_ptr = cnt->exec.stack->top_ptr + size;

	if (!is_aligned(mem, MAX_ALIGNMENT)) {
		bl_warning("BAD ALIGNMENT %p, %d bytes", mem, size);
	}

	_chck_write(mem, orig_size);

	return mem;
}

/* shift stack top by the size in bytes */
static inline MirStackPtr
exec_stack_free(Context *cnt, size_t size)
{
#if BL_DEBUG && CHCK_STACK
	const size_t orig_size = size;
#endif

	size                = exec_stack_alloc_size(size);
	MirStackPtr new_top = cnt->exec.stack->top_ptr - size;
	if (new_top < (uint8_t *)(cnt->exec.stack->ra + 1)) bl_abort("Stack underflow!!!");
	cnt->exec.stack->top_ptr = new_top;
	cnt->exec.stack->used_bytes -= size;

	_chck_validate(new_top, orig_size);

	return new_top;
}

static inline void
exec_push_ra(Context *cnt, MirInstr *instr)
{
	MirFrame *prev      = cnt->exec.stack->ra;
	MirFrame *tmp       = (MirFrame *)exec_stack_alloc(cnt, sizeof(MirFrame));
	tmp->callee         = instr;
	tmp->prev           = prev;
	cnt->exec.stack->ra = tmp;
	_log_push_ra;
}

static inline MirInstr *
exec_pop_ra(Context *cnt)
{
	if (!cnt->exec.stack->ra) return NULL;
	MirInstr *callee = cnt->exec.stack->ra->callee;

	_log_pop_ra;

	/* rollback */
	MirStackPtr new_top_ptr     = (MirStackPtr)cnt->exec.stack->ra;
	cnt->exec.stack->used_bytes = cnt->exec.stack->top_ptr - new_top_ptr;
	cnt->exec.stack->top_ptr    = new_top_ptr;
	cnt->exec.stack->ra         = cnt->exec.stack->ra->prev;
	return callee;
}

static inline MirStackPtr
exec_push_stack_empty(Context *cnt, MirType *type)
{
	assert(type);
	const size_t size = type->store_size_bytes;
	assert(size && "pushing zero sized data on stack");
	MirStackPtr tmp = exec_stack_alloc(cnt, size);

	_log_push_stack;
	return tmp;
}

static inline MirStackPtr
exec_push_stack(Context *cnt, void *value, MirType *type)
{
	assert(value && "try to push NULL value");
	MirStackPtr  tmp  = exec_push_stack_empty(cnt, type);
	const size_t size = type->store_size_bytes;
	memcpy(tmp, value, size);
	/* pointer relative to frame top */
	return tmp;
}

static inline MirStackPtr
exec_pop_stack(Context *cnt, MirType *type)
{
	assert(type);
	const size_t size = type->store_size_bytes;
	assert(size && "popping zero sized data on stack");

	_log_pop_stack;

	return exec_stack_free(cnt, size);
}

#define exec_pop_stack_as(cnt, type, T) ((T)exec_pop_stack((cnt), (type)))

static inline void
exec_stack_alloc_var(Context *cnt, MirVar *var)
{
	assert(var);
	assert(!var->comptime && "cannot allocate compile time constant");
	/* allocate memory for variable on stack */

	MirStackPtr tmp    = exec_push_stack_empty(cnt, var->value.type);
	var->rel_stack_ptr = tmp - (MirStackPtr)cnt->exec.stack->ra;
}

static inline void
exec_stack_alloc_local_vars(Context *cnt, MirFn *fn)
{
	assert(fn);
	/* Init all stack variables. */
	BArray *vars = fn->variables;
	MirVar *var;
	barray_foreach(vars, var)
	{
		if (var->comptime) continue;
		exec_stack_alloc_var(cnt, var);
	}
}

/* Return pointer to value evaluated from src instruction. Source can be compile
 * time constant or allocated on the stack.*/
static inline MirStackPtr
exec_fetch_value(Context *cnt, MirInstr *src)
{
	if (src->comptime || src->kind == MIR_INSTR_DECL_REF ||
	    src->kind == MIR_INSTR_DECL_DIRECT_REF) {
		return (MirStackPtr)&src->value.data;
	}

	return exec_pop_stack(cnt, src->value.type);
}

static inline MirInstr *
exec_get_pc(Context *cnt)
{
	return cnt->exec.stack->pc;
}

static inline MirFrame *
exec_get_ra(Context *cnt)
{
	return cnt->exec.stack->ra;
}

static inline void
exec_set_pc(Context *cnt, MirInstr *instr)
{
	cnt->exec.stack->pc = instr;
}
/* execute end */

static inline bool
is_builtin(Ast *ident, MirBuiltinIdKind kind)
{
	if (!ident) return false;
	assert(ident->kind == AST_IDENT);
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
error_types(Context *cnt, MirType *from, MirType *to, Ast *loc, const char *msg)
{
	assert(from && to);
	if (!msg) msg = "No implicit cast for type '%s' and '%s'.";

	char tmp_from[256];
	char tmp_to[256];
	mir_type_to_str(tmp_from, 256, from, true);
	mir_type_to_str(tmp_to, 256, to, true);

	builder_msg(cnt->builder,
	            BUILDER_MSG_ERROR,
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
	assert(id);

	ScopeEntry *entry = scope_lookup(fn->decl_node->owner_scope, id, true, false);
	assert(entry && "cannot commit unregistred function");

	entry->kind    = SCOPE_ENTRY_FN;
	entry->data.fn = fn;

	analyze_notify_provided(cnt, id->hash);
}

static inline void
commit_variant(Context *cnt, MirVariant *v)
{
	ID *id = v->id;
	assert(id);

	ScopeEntry *entry = scope_lookup(v->decl_scope, id, false, true);
	assert(entry && "cannot commit unregistred variant");

	entry->kind         = SCOPE_ENTRY_VARIANT;
	entry->data.variant = v;
}

static inline void
commit_member(Context *cnt, MirMember *member)
{
	ID *id = member->id;
	assert(id);

	ScopeEntry *entry = scope_lookup(member->decl_scope, id, false, true);
	assert(entry && "cannot commit unregistred member");

	entry->kind        = SCOPE_ENTRY_MEMBER;
	entry->data.member = member;
}

static inline void
commit_var(Context *cnt, MirVar *var)
{
	ID *id = var->id;
	assert(id);

	ScopeEntry *entry = scope_lookup(var->decl_scope, id, true, false);
	assert(entry && "cannot commit unregistred var");

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
	assert(phi && value && block);
	ref_instr(value);
	ref_instr(&block->base);

	sa_push_Instr(phi->incoming_values, value);
	sa_push_Instr(phi->incoming_blocks, &block->base);
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
	assert(base_type->id.str);
	BString *tmp = cnt->tmp_sh;
	bo_string_clear(tmp);
	bo_string_append(tmp, "n.");
	bo_string_append(tmp, base_type->id.str);
	return bo_string_get(tmp);
}

static inline const char *
sh_type_ptr(Context *cnt, MirType *src_type)
{
	assert(src_type->id.str);
	BString *tmp = cnt->tmp_sh;
	bo_string_clear(tmp);
	bo_string_append(tmp, "p.");
	bo_string_append(tmp, src_type->id.str);
	return bo_string_get(tmp);
}

static inline const char *
sh_type_fn(Context *cnt, MirType *ret_type, SmallArray_Type *arg_types, bool is_vargs)
{
	// assert(src_type->id.str);
	BString *tmp = cnt->tmp_sh;
	bo_string_clear(tmp);

	bo_string_append(tmp, "f.(");

	/* append all arg types isd */
	if (arg_types) {
		MirType *arg_type;
		sarray_foreach(arg_types, arg_type)
		{
			assert(arg_type->id.str);
			bo_string_append(tmp, arg_type->id.str);

			if (i != arg_types->size - 1) bo_string_append(tmp, ",");
		}
	}

	bo_string_append(tmp, ")");

	if (ret_type) {
		assert(ret_type->id.str);
		bo_string_append(tmp, ret_type->id.str);
	} else {
		/* implicit return void */
		bo_string_append(tmp, cnt->builtin_types.entry_void->id.str);
	}

	return bo_string_get(tmp);
}

static inline const char *
sh_type_arr(Context *cnt, MirType *elem_type, size_t len)
{
	assert(elem_type->id.str);
	BString *tmp = cnt->tmp_sh;
	bo_string_clear(tmp);

	char ui_str[21];
	sprintf(ui_str, "%llu", (unsigned long long)len);

	bo_string_append(tmp, ui_str);
	bo_string_append(tmp, ".");
	bo_string_append(tmp, elem_type->id.str);
	return bo_string_get(tmp);
}

static inline const char *
sh_type_struct(Context *cnt, MirTypeKind kind, ID *id, SmallArray_Member *members, bool is_packed)
{
	assert(!is_packed);
	BString *tmp = cnt->tmp_sh;
	bo_string_clear(tmp);

	switch (kind) {
	case MIR_TYPE_STRUCT:
		bo_string_append(tmp, "s.");
		break;
	case MIR_TYPE_SLICE:
		bo_string_append(tmp, "sl.");
		break;
	case MIR_TYPE_STRING:
		bo_string_append(tmp, "ss.");
		break;
	case MIR_TYPE_VARGS:
		bo_string_append(tmp, "sv.");
		break;
	default:
		bl_abort("Expected struct base type.");
	}

	if (id) {
		bo_string_append(tmp, id->str);
	}

	bo_string_append(tmp, "{");
	if (members) {
		MirMember *member;
		sarray_foreach(members, member)
		{
			assert(member->type->id.str);
			bo_string_append(tmp, member->type->id.str);

			if (i != members->size - 1) bo_string_append(tmp, ",");
		}
	}

	bo_string_append(tmp, "}");
	return bo_string_get(tmp);
}

static inline const char *
sh_type_enum(Context *cnt, ID *id, MirType *base_type, SmallArray_Variant *variants)
{
	assert(base_type->id.str);
	BString *tmp = cnt->tmp_sh;
	bo_string_clear(tmp);

	bo_string_append(tmp, "e.");

	if (id) bo_string_append(tmp, id->str);

	bo_string_append(tmp, "(");
	bo_string_append(tmp, base_type->id.str);
	bo_string_append(tmp, ")");

	bo_string_append(tmp, "{");
	if (variants) {
		MirVariant *variant;
		sarray_foreach(variants, variant)
		{
			assert(variant->value);

			char value_str[35];
			snprintf(value_str,
			         array_size(value_str),
			         "%lld",
			         (long long)variant->value->data.v_s64);
			bo_string_append(tmp, value_str);

			if (i != variants->size - 1) bo_string_append(tmp, ",");
		}
	}
	bo_string_append(tmp, "}");
	return bo_string_get(tmp);
}

/* impl */
/* Fetch type, when type with same sh has been already created and can be
 * reused, this function return false and set out_type to already created type
 * from cache. When new type instance was created function will return true and
 * set out_type to new instance of type, new instance will be stored in cache
 * for later use also.
 *
 * Hashing rules:
 *
 * | Type       | Rules                           |
 * |------------+---------------------------------|
 * | Null       | n.<type>                        |
 * | Pointer    | p.<type>                        |
 * | Function   | f.(<arg1,...>)<return type>     |
 * | Array      | <len>.<type>                    |
 * | Structures | <s|sl|sv|ss>.<name>{<m1,...>}   |
 * | Enumerator | <e>.<name>(<type>){<1,2,...>}   |
 */
bool
create_type(Context *cnt, MirType **out_type, const char *sh)
{
	assert(out_type);
	assert(sh);
	uint64_t hash = bo_hash_from_str(sh);

	bo_iterator_t found = bo_htbl_find(cnt->type_table, hash);
	bo_iterator_t end   = bo_htbl_end(cnt->type_table);
	if (!bo_iterator_equal(&found, &end)) {
		*out_type = bo_htbl_iter_peek_value(cnt->type_table, &found, MirType *);
		assert(*out_type);
		return false;
	} else {
		MirType *tmp = arena_alloc(&cnt->assembly->arenas.mir.type);

		BString *copy = builder_create_cached_str(cnt->builder);
		bo_string_append(copy, sh);

		tmp->id.str  = bo_string_get(copy);
		tmp->id.hash = hash;

		// bl_log("new type: '%s' (%llu)", tmp->id.str, tmp->id.hash);
		bo_htbl_insert(cnt->type_table, tmp->id.hash, tmp);
		*out_type = tmp;

		return true;
	}

	bl_abort("should not happend");
}

ScopeEntry *
register_symbol(Context *cnt, Ast *node, ID *id, Scope *scope, bool is_builtin, bool enable_groups)
{
	assert(id && "Missing symbol ID.");
	assert(scope && "Missing entry scope.");

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
	if (is_builtin) cache_builtin(cnt, entry);
	return entry;

COLLIDE : {
	char *err_msg = collision->is_buildin || is_builtin
	                    ? "Symbol name colision with compiler builtin '%s'."
	                    : "Duplicate symbol";

	builder_msg(cnt->builder,
	            BUILDER_MSG_ERROR,
	            ERR_DUPLICATE_SYMBOL,
	            node ? node->location : NULL,
	            BUILDER_CUR_WORD,
	            err_msg,
	            id->str);

	if (collision->node) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_NOTE,
		            0,
		            collision->node->location,
		            BUILDER_CUR_WORD,
		            "Previous declaration found here.");
	}

	return NULL;
}
}

void
cache_builtin(Context *cnt, ScopeEntry *entry)
{
	assert(entry);
	ScopeEntry *collision = scope_lookup(cnt->builtin_types.cache, entry->id, true, false);
	if (collision) {
		bl_abort("Duplicate compiler internal '%s'.", entry->id->str);
	}

	scope_insert(cnt->builtin_types.cache, entry);
}

MirType *
lookup_builtin(Context *cnt, MirBuiltinIdKind kind)
{
	ID *        id    = &builtin_ids[kind];
	Scope *     scope = cnt->builtin_types.cache;
	ScopeEntry *found = scope_lookup(scope, id, true, false);

	if (!found) bl_abort("Missing compiler internal symbol '%s'", id->str);
	if (found->kind == SCOPE_ENTRY_INCOMPLETE) return NULL;

	assert(found->kind == SCOPE_ENTRY_VAR);

	MirVar *var = found->data.var;

	if (!is_flag(var->flags, FLAG_COMPILER))
		bl_abort("Internally used symbol '%s' declared without '#compiler' flag!",
		         var->llvm_name);

	assert(var);
	assert(var->comptime && var->value.type->kind == MIR_TYPE_TYPE);
	assert(var->value.data.v_ptr.data.type);

	return var->value.data.v_ptr.data.type;
}

MirType *
create_type_type(Context *cnt)
{
	MirType *tmp = NULL;
	if (create_type(cnt, &tmp, builtin_ids[MIR_BUILTIN_ID_TYPE_TYPE].str)) {
		/* NOTE: TypeType has no LLVM representation */
		tmp->kind             = MIR_TYPE_TYPE;
		tmp->user_id          = &builtin_ids[MIR_BUILTIN_ID_TYPE_TYPE];
		tmp->alignment        = __alignof(MirType *);
		tmp->size_bits        = sizeof(MirType *) * 8;
		tmp->store_size_bytes = sizeof(MirType *);
	}
	return tmp;
}

MirType *
create_type_null(Context *cnt, MirType *base_type)
{
	assert(base_type);
	MirType *tmp = NULL;
	if (create_type(cnt, &tmp, sh_type_null(cnt, base_type))) {
		tmp->kind                = MIR_TYPE_NULL;
		tmp->user_id             = &builtin_ids[MIR_BUILTIN_ID_NULL];
		tmp->data.null.base_type = base_type;

		init_llvm_type_null(cnt, tmp);
	}
	return tmp;
}

MirType *
create_type_void(Context *cnt)
{
	MirType *tmp = NULL;
	if (create_type(cnt, &tmp, builtin_ids[MIR_BUILTIN_ID_TYPE_VOID].str)) {
		tmp->kind    = MIR_TYPE_VOID;
		tmp->user_id = &builtin_ids[MIR_BUILTIN_ID_TYPE_VOID];

		init_llvm_type_void(cnt, tmp);
	}
	return tmp;
}

MirType *
create_type_bool(Context *cnt)
{
	MirType *tmp = NULL;
	if (create_type(cnt, &tmp, builtin_ids[MIR_BUILTIN_ID_TYPE_BOOL].str)) {
		tmp->kind    = MIR_TYPE_BOOL;
		tmp->user_id = &builtin_ids[MIR_BUILTIN_ID_TYPE_BOOL];

		init_llvm_type_bool(cnt, tmp);
	}
	return tmp;
}

MirType *
create_type_int(Context *cnt, ID *id, int32_t bitcount, bool is_signed)
{
	assert(id);
	assert(bitcount > 0);
	MirType *tmp = NULL;
	if (create_type(cnt, &tmp, id->str)) {
		tmp->kind                   = MIR_TYPE_INT;
		tmp->user_id                = id;
		tmp->data.integer.bitcount  = bitcount;
		tmp->data.integer.is_signed = is_signed;

		init_llvm_type_int(cnt, tmp);
	}
	return tmp;
}

MirType *
create_type_real(Context *cnt, ID *id, int32_t bitcount)
{
	assert(bitcount > 0);
	MirType *tmp = NULL;
	if (create_type(cnt, &tmp, id->str)) {
		tmp->kind               = MIR_TYPE_REAL;
		tmp->user_id            = id;
		tmp->data.real.bitcount = bitcount;

		init_llvm_type_real(cnt, tmp);
	}
	return tmp;
}

MirType *
create_type_ptr(Context *cnt, MirType *src_type)
{
	MirType *tmp = NULL;
	if (create_type(cnt, &tmp, sh_type_ptr(cnt, src_type))) {
		tmp->kind          = MIR_TYPE_PTR;
		tmp->data.ptr.expr = src_type;

		init_llvm_type_ptr(cnt, tmp);
	}

	return tmp;
}

MirType *
create_type_fn(Context *cnt, MirType *ret_type, SmallArray_Type *arg_types, bool is_vargs)
{
	MirType *tmp = NULL;
	if (create_type(cnt, &tmp, sh_type_fn(cnt, ret_type, arg_types, is_vargs))) {
		tmp->kind              = MIR_TYPE_FN;
		tmp->data.fn.arg_types = arg_types;
		tmp->data.fn.is_vargs  = is_vargs;
		tmp->data.fn.ret_type  = ret_type ? ret_type : cnt->builtin_types.entry_void;

		init_llvm_type_fn(cnt, tmp);
	}

	return tmp;
}

MirType *
create_type_array(Context *cnt, MirType *elem_type, size_t len)
{
	MirType *tmp = NULL;
	if (create_type(cnt, &tmp, sh_type_arr(cnt, elem_type, len))) {
		tmp->kind                 = MIR_TYPE_ARRAY;
		tmp->data.array.elem_type = elem_type;
		tmp->data.array.len       = len;

		init_llvm_type_array(cnt, tmp);
	}

	return tmp;
}

MirType *
create_type_struct(Context *          cnt,
                   MirTypeKind        kind,
                   ID *               id,
                   Scope *            scope,
                   SmallArray_Member *members, /* MirMember */
                   bool               is_packed)
{
	MirType *tmp = NULL;

	if (create_type(cnt, &tmp, sh_type_struct(cnt, kind, id, members, is_packed))) {
		tmp->kind                 = kind;
		tmp->data.strct.members   = members;
		tmp->data.strct.scope     = scope;
		tmp->data.strct.is_packed = is_packed;
		tmp->user_id              = id;

		init_llvm_type_struct(cnt, tmp);
	}

	return tmp;
}

MirType *
create_type_struct_special(Context *cnt, MirTypeKind kind, ID *id, MirType *elem_ptr_type)
{
	assert(mir_is_pointer_type(elem_ptr_type));
	assert(kind == MIR_TYPE_STRING || kind == MIR_TYPE_VARGS || kind == MIR_TYPE_SLICE);

	/* PERFORMANCE: due to reusing of the types we can create members and scope which will
	 * not be later used because same type already exists. */
	SmallArray_Member *members = create_sarr(SmallArray_Member, cnt->assembly);

	/* Slice layout struct { usize, *T } */
	Scope *body_scope = scope_create(
	    &cnt->assembly->arenas.scope, SCOPE_TYPE_STRUCT, cnt->assembly->gscope, 2, NULL);

	MirMember *tmp;
	tmp = create_member(cnt,
	                    NULL,
	                    &builtin_ids[MIR_BUILTIN_ID_ARR_LEN],
	                    body_scope,
	                    0,
	                    cnt->builtin_types.entry_usize);

	sa_push_Member(members, tmp);
	provide_builtin_member(cnt, body_scope, tmp);

	tmp = create_member(
	    cnt, NULL, &builtin_ids[MIR_BUILTIN_ID_ARR_PTR], body_scope, 1, elem_ptr_type);

	sa_push_Member(members, tmp);
	provide_builtin_member(cnt, body_scope, tmp);

	return create_type_struct(cnt, kind, id, body_scope, members, false);
}

MirType *
create_type_enum(Context *           cnt,
                 ID *                id,
                 Scope *             scope,
                 MirType *           base_type,
                 SmallArray_Variant *variants)
{
	assert(base_type);
	MirType *tmp = NULL;
	if (create_type(cnt, &tmp, sh_type_enum(cnt, id, base_type, variants))) {
		tmp->kind               = MIR_TYPE_ENUM;
		tmp->data.enm.scope     = scope;
		tmp->data.enm.base_type = base_type;
		tmp->data.enm.variants  = variants;
		tmp->user_id            = id;

		init_llvm_type_enum(cnt, tmp);
	}

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
		bl_abort("invalid floating point type");

	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment        = LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	/*** DI ***/
	if (!cnt->debug_mode) return;

	const char *name = type->user_id ? type->user_id->str : type->id.str;

	type->llvm_meta = llvm_di_create_basic_type(
	    cnt->analyze.llvm_di_builder, name, type->size_bits, DW_ATE_float);
}

void
init_llvm_type_ptr(Context *cnt, MirType *type)
{
	MirType *tmp = mir_deref_type(type);
	/* Pointer to Type has no LLVM representation and cannot not be generated into IR.*/
	if (tmp->kind == MIR_TYPE_TYPE) return;

	assert(tmp);
	assert(tmp->llvm_type);
	type->llvm_type        = LLVMPointerType(tmp->llvm_type, 0);
	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment        = LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	/*** DI ***/
	if (!cnt->debug_mode) return;

	const char *name = type->user_id ? type->user_id->str : type->id.str;
	type->llvm_meta  = llvm_di_create_pointer_type(cnt->analyze.llvm_di_builder,
                                                      tmp->llvm_meta,
                                                      type->size_bits,
                                                      type->alignment * 8,
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
	assert(tmp);
	assert(tmp->llvm_type);
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
	type->alignment        = LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	/*** DI ***/
	if (!cnt->debug_mode) return;

	const char *name = type->user_id ? type->user_id->str : type->id.str;
	type->llvm_meta =
	    llvm_di_create_basic_type(cnt->analyze.llvm_di_builder, name, 8, DW_ATE_boolean);
}

void
init_llvm_type_fn(Context *cnt, MirType *type)
{
	MirType *tmp_ret = type->data.fn.ret_type;
	if (tmp_ret->kind == MIR_TYPE_TYPE) {
		return;
	}

	SmallArray_Type *arg_types = type->data.fn.arg_types;
	size_t           argc      = arg_types ? arg_types->size : 0;

	SmallArray_LLVMType llvm_args;
	sa_init(&llvm_args);
	LLVMTypeRef llvm_ret = NULL;

	if (argc) {
		MirType *tmp_arg;
		sarray_foreach(arg_types, tmp_arg)
		{
			assert(tmp_arg->llvm_type);
			sa_push_LLVMType(&llvm_args, tmp_arg->llvm_type);
		}
	}

	llvm_ret = tmp_ret ? tmp_ret->llvm_type : LLVMVoidTypeInContext(cnt->assembly->llvm.cnt);
	assert(llvm_ret);

	type->llvm_type = LLVMFunctionType(llvm_ret, llvm_args.data, (unsigned int)argc, false);
	type->alignment = __alignof(MirFn *);
	type->size_bits = sizeof(MirFn *) * 8;
	type->store_size_bytes = sizeof(MirFn *);

	sa_terminate(&llvm_args);

	/*** DI ***/
	if (!cnt->debug_mode) return;
	SmallArray_LLVMMetadata params;
	sa_init(&params);

	/* return type is first */
	sa_push_LLVMMetadata(&params, type->data.fn.ret_type->llvm_meta);

	if (type->data.fn.arg_types) {
		MirType *it;
		sarray_foreach(type->data.fn.arg_types, it)
		    sa_push_LLVMMetadata(&params, it->llvm_meta);
	}

	type->llvm_meta =
	    llvm_di_create_function_type(cnt->analyze.llvm_di_builder, params.data, params.size);

	sa_terminate(&params);
}

void
init_llvm_type_array(Context *cnt, MirType *type)
{
	LLVMTypeRef llvm_elem_type = type->data.array.elem_type->llvm_type;
	assert(llvm_elem_type);
	const unsigned int len = (const unsigned int)type->data.array.len;

	type->llvm_type        = LLVMArrayType(llvm_elem_type, len);
	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment        = LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	/*** DI ***/
	if (!cnt->debug_mode) return;
	type->llvm_meta = llvm_di_create_array_type(cnt->analyze.llvm_di_builder,
	                                            type->size_bits,
	                                            type->alignment * 8,
	                                            type->data.array.elem_type->llvm_meta,
	                                            type->data.array.len);
}

void
init_llvm_type_struct(Context *cnt, MirType *type)
{
	SmallArray_Member *members = type->data.strct.members;
	assert(members);

	const bool   is_packed = type->data.strct.is_packed;
	const size_t memc      = members->size;
	assert(memc > 0);
	SmallArray_LLVMType llvm_members;
	sa_init(&llvm_members);

	{
		MirMember *tmp;
		sarray_foreach(members, tmp)
		{
			assert(tmp->type->llvm_type);
			sa_push_LLVMType(&llvm_members, tmp->type->llvm_type);
		}
	}

	/* named structure type */
	if (type->user_id) {
		type->llvm_type =
		    LLVMStructCreateNamed(cnt->assembly->llvm.cnt, type->user_id->str);
		LLVMStructSetBody(
		    type->llvm_type, llvm_members.data, (unsigned long)memc, is_packed);
	} else {
		type->llvm_type = LLVMStructTypeInContext(
		    cnt->assembly->llvm.cnt, llvm_members.data, (unsigned long)memc, is_packed);
	}

	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment        = LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	sa_terminate(&llvm_members);

	/*** DI ***/
	if (!cnt->debug_mode) return;

	assert(type->data.strct.scope);
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
		struct_line        = location->line;
	}

	LLVMMetadataRef llvm_scope = type->data.strct.scope->llvm_di_meta;
	const char *    struct_name;
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
			bl_abort("cannot get struct name for DI");
		}
	}

	SmallArray_LLVMMetadata llvm_elems;
	sa_init(&llvm_elems);

	MirMember *elem;
	sarray_foreach(type->data.strct.members, elem)
	{
		unsigned        elem_line = elem->decl_node ? elem->decl_node->location->line : 0;
		LLVMMetadataRef llvm_elem =
		    llvm_di_create_member_type(cnt->analyze.llvm_di_builder,
		                               llvm_scope,
		                               elem->id->str,
		                               llvm_file,
		                               elem_line,
		                               elem->type->size_bits,
		                               elem->type->alignment * 8,
		                               get_struct_elem_offest(cnt, type, i) * 8,
		                               elem->type->llvm_meta);

		sa_push_LLVMMetadata(&llvm_elems, llvm_elem);
	}

	LLVMMetadataRef llvm_struct =
	    llvm_di_create_struct_type(cnt->analyze.llvm_di_builder,
	                               type->data.strct.scope->parent->llvm_di_meta,
	                               struct_name,
	                               llvm_file,
	                               struct_line,
	                               type->size_bits,
	                               type->alignment * 8,
	                               llvm_elems.data,
	                               llvm_elems.size);

	type->llvm_meta = llvm_di_replace_temporary(
	    cnt->analyze.llvm_di_builder, type->data.strct.scope->llvm_di_meta, llvm_struct);

	sa_terminate(&llvm_elems);
	return;
}

void
init_llvm_type_enum(Context *cnt, MirType *type)
{
	MirType *base_type = type->data.enm.base_type;
	assert(base_type->kind == MIR_TYPE_INT);
	LLVMTypeRef llvm_base_type = base_type->llvm_type;
	assert(llvm_base_type);

	type->llvm_type        = llvm_base_type;
	type->size_bits        = LLVMSizeOfTypeInBits(cnt->assembly->llvm.TD, type->llvm_type);
	type->store_size_bytes = LLVMStoreSizeOfType(cnt->assembly->llvm.TD, type->llvm_type);
	type->alignment        = LLVMABIAlignmentOfType(cnt->assembly->llvm.TD, type->llvm_type);

	/*** DI ***/
	if (!cnt->debug_mode) return;
	const char *name = type->user_id ? type->user_id->str : "enum";

	SmallArray_LLVMMetadata llvm_elems;
	sa_init(&llvm_elems);

	MirVariant *variant;
	sarray_foreach(type->data.enm.variants, variant)
	{
		LLVMMetadataRef llvm_variant =
		    llvm_di_create_enum_variant(cnt->analyze.llvm_di_builder,
		                                variant->id->str,
		                                variant->value->data.v_u64,
		                                !base_type->data.integer.is_signed);

		sa_push_LLVMMetadata(&llvm_elems, llvm_variant);
	}

	LLVMMetadataRef llvm_type =
	    llvm_di_create_enum_type(cnt->analyze.llvm_di_builder,
	                             type->data.enm.scope->parent->llvm_di_meta,
	                             name,
	                             type->data.enm.scope->location->unit->llvm_file_meta,
	                             type->data.enm.scope->location->line,
	                             type->size_bits,
	                             type->alignment * 8,
	                             llvm_elems.data,
	                             llvm_elems.size,
	                             base_type->llvm_meta);

	type->llvm_meta = llvm_di_replace_temporary(
	    cnt->analyze.llvm_di_builder, type->data.enm.scope->llvm_di_meta, llvm_type);
	sa_terminate(&llvm_elems);
}

void
init_llvm_DI_scope(Context *cnt, Scope *scope)
{
	switch (scope->kind) {
	case SCOPE_LEXICAL: {
		assert(scope->location);
		LLVMMetadataRef llvm_parent_scope = scope->parent->llvm_di_meta;
		LLVMMetadataRef llvm_unit         = scope->location->unit->llvm_file_meta;

		assert(llvm_parent_scope);
		assert(llvm_unit);

		scope->llvm_di_meta = llvm_di_create_lexical_scope(cnt->analyze.llvm_di_builder,
		                                                   llvm_parent_scope,
		                                                   llvm_unit,
		                                                   scope->location->line,
		                                                   scope->location->col);
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
		bl_abort("unsuported scope type for DI generation");
	}
}

static inline void
push_var(Context *cnt, MirVar *var)
{
	assert(var);

	if (var->is_in_gscope) return;

	MirFn *fn = get_current_fn(cnt);
	assert(fn);
	bo_array_push_back(fn->variables, var);
}

MirVar *
create_var(Context *cnt,
           Ast *    decl_node,
           Scope *  scope,
           ID *     id,
           MirType *alloc_type,
           bool     is_mutable,
           bool     is_in_gscope,
           int32_t  order, /* pass -1 if none */
           uint32_t flags)
{
	assert(id);
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
	tmp->is_arg_tmp   = order > -1;
	tmp->order        = order;

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
	assert(name);
	MirVar *tmp       = arena_alloc(&cnt->assembly->arenas.mir.var);
	tmp->value.type   = alloc_type;
	tmp->is_mutable   = is_mutable;
	tmp->is_in_gscope = is_in_gscope;
	tmp->llvm_name    = name;
	tmp->is_implicit  = true;
	tmp->gen_llvm     = true;
	tmp->comptime     = comptime;
	tmp->order        = -1;

	push_var(cnt, tmp);
	return tmp;
}

MirFn *
create_fn(Context *        cnt,
          Ast *            node,
          ID *             id,
          const char *     llvm_name,
          int32_t          flags,
          MirInstrFnProto *prototype,
          bool             emit_llvm)
{
	MirFn *tmp     = arena_alloc(&cnt->assembly->arenas.mir.fn);
	tmp->variables = create_arr(cnt->assembly, sizeof(MirVar *));
	tmp->llvm_name = llvm_name;
	tmp->id        = id;
	tmp->flags     = flags;
	tmp->decl_node = node;
	tmp->prototype = &prototype->base;
	tmp->emit_llvm = emit_llvm;
	return tmp;
}

MirMember *
create_member(Context *cnt, Ast *node, ID *id, Scope *scope, int64_t index, MirType *type)
{
	MirMember *tmp  = arena_alloc(&cnt->assembly->arenas.mir.member);
	tmp->decl_node  = node;
	tmp->id         = id;
	tmp->index      = index;
	tmp->type       = type;
	tmp->decl_scope = scope;
	return tmp;
}

MirVariant *
create_variant(Context *cnt, Ast *node, ID *id, Scope *scope, MirConstValue *value)
{
	MirVariant *tmp = arena_alloc(&cnt->assembly->arenas.mir.variant);
	tmp->decl_node  = node;
	tmp->id         = id;
	tmp->decl_scope = scope;
	tmp->value      = value;
	return tmp;
}

MirConstValue *
create_const_value(Context *cnt, MirType *type)
{
	assert(type);
	MirConstValue *tmp = arena_alloc(&cnt->assembly->arenas.mir.value);
	tmp->type          = type;
	tmp->addr_mode     = MIR_VAM_LVALUE_CONST;
	return tmp;
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
	assert(instr);
	MirInstrBlock *block = get_current_block(cnt);
	assert(block);

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
	MirInstrCast *cast    = create_instr(cnt, MIR_INSTR_CAST, src->node, MirInstrCast *);
	cast->base.value.type = to_type;
	cast->expr            = src;
	ref_instr(&cast->base);

	insert_instr_after(src, &cast->base);
	analyze_instr_rq(cnt, &cast->base);
	return &cast->base;
}

MirInstr *
insert_instr_toany(Context *cnt, MirInstr *expr)
{
	MirInstrToAny *toany   = create_instr(cnt, MIR_INSTR_TOANY, expr->node, MirInstrToAny *);
	toany->base.value.type = create_type_ptr(cnt, lookup_builtin(cnt, MIR_BUILTIN_ID_ANY));
	toany->expr            = expr;
	ref_instr(&toany->base);

	insert_instr_after(expr, &toany->base);
	analyze_instr_rq(cnt, &toany->base);
	return &toany->base;
}

MirInstr *
insert_instr_load(Context *cnt, MirInstr *src)
{
	assert(src);
	assert(src->value.type);
	assert(src->value.type->kind == MIR_TYPE_PTR);
	MirInstrLoad *tmp  = create_instr(cnt, MIR_INSTR_LOAD, src->node, MirInstrLoad *);
	tmp->src           = src;
	tmp->base.analyzed = true;

	ref_instr(&tmp->base);
	insert_instr_after(src, &tmp->base);
	analyze_instr_load(cnt, tmp);

	return &tmp->base;
}

MirCastOp
get_cast_op(MirType *from, MirType *to)
{
	assert(from);
	assert(to);
	const size_t fsize = from->size_bits;
	const size_t tsize = to->size_bits;

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
		break;
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
		break;
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
		break;
	}

	default:
		return MIR_CAST_INVALID;
	}

	return MIR_CAST_INVALID;
}

static uint64_t _id_counter = 0;

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
	assert(fn && name);
	MirInstrBlock *tmp = create_instr(cnt, MIR_INSTR_BLOCK, NULL, MirInstrBlock *);
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
	assert(fn && fn->kind == MIR_INSTR_FN_PROTO);
	MirInstrCall *tmp  = create_instr(cnt, MIR_INSTR_CALL, node, MirInstrCall *);
	tmp->base.comptime = true;
	tmp->callee        = fn;

	ref_instr(fn);
	return &tmp->base;
}

MirInstr *
append_instr_type_fn(Context *cnt, Ast *node, MirInstr *ret_type, SmallArray_Instr *arg_types)
{
	MirInstrTypeFn *tmp  = create_instr(cnt, MIR_INSTR_TYPE_FN, node, MirInstrTypeFn *);
	tmp->base.value.type = cnt->builtin_types.entry_type;
	tmp->base.comptime   = true;
	tmp->ret_type        = ret_type;
	tmp->arg_types       = arg_types;

	if (arg_types) {
		MirInstr *it;
		sarray_foreach(arg_types, it)
		{
			ref_instr(it);
		}
	}

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_type_struct(Context *         cnt,
                         Ast *             node,
                         ID *              id,
                         Scope *           scope,
                         SmallArray_Instr *members,
                         bool              is_packed)
{
	MirInstrTypeStruct *tmp =
	    create_instr(cnt, MIR_INSTR_TYPE_STRUCT, node, MirInstrTypeStruct *);
	tmp->base.value.type = cnt->builtin_types.entry_type;
	tmp->base.comptime   = true;
	tmp->members         = members;
	tmp->scope           = scope;
	tmp->is_packed       = is_packed;
	tmp->id              = id;

	if (members) {
		MirInstr *it;
		sarray_foreach(members, it)
		{
			ref_instr(it);
		}
	}

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_type_enum(Context *         cnt,
                       Ast *             node,
                       ID *              id,
                       Scope *           scope,
                       SmallArray_Instr *variants,
                       MirInstr *        base_type)
{
	MirInstrTypeEnum *tmp = create_instr(cnt, MIR_INSTR_TYPE_ENUM, node, MirInstrTypeEnum *);
	tmp->base.value.type  = cnt->builtin_types.entry_type;
	tmp->base.comptime    = true;
	tmp->variants         = variants;
	tmp->scope            = scope;
	tmp->id               = id;
	tmp->base_type        = base_type;

	if (variants) {
		MirInstr *it;
		sarray_foreach(variants, it)
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
	MirInstrTypePtr *tmp = create_instr(cnt, MIR_INSTR_TYPE_PTR, node, MirInstrTypePtr *);
	tmp->base.value.type = cnt->builtin_types.entry_type;
	tmp->base.comptime   = true;
	tmp->type            = type;

	ref_instr(type);
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_type_array(Context *cnt, Ast *node, MirInstr *elem_type, MirInstr *len)
{
	MirInstrTypeArray *tmp = create_instr(cnt, MIR_INSTR_TYPE_ARRAY, node, MirInstrTypeArray *);
	tmp->base.value.type   = cnt->builtin_types.entry_type;
	tmp->base.comptime     = true;
	tmp->elem_type         = elem_type;
	tmp->len               = len;

	ref_instr(elem_type);
	ref_instr(len);
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_type_slice(Context *cnt, Ast *node, MirInstr *elem_type)
{
	MirInstrTypeSlice *tmp = create_instr(cnt, MIR_INSTR_TYPE_SLICE, node, MirInstrTypeSlice *);
	tmp->base.value.type   = cnt->builtin_types.entry_type;
	tmp->base.comptime     = true;
	tmp->elem_type         = elem_type;

	ref_instr(elem_type);
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_type_vargs(Context *cnt, Ast *node, MirInstr *elem_type)
{
	MirInstrTypeVArgs *tmp = create_instr(cnt, MIR_INSTR_TYPE_VARGS, node, MirInstrTypeVArgs *);
	tmp->base.value.type   = cnt->builtin_types.entry_type;
	tmp->base.comptime     = true;
	tmp->elem_type         = elem_type;

	ref_instr(elem_type);
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_arg(Context *cnt, Ast *node, unsigned i)
{
	MirInstrArg *tmp = create_instr(cnt, MIR_INSTR_ARG, node, MirInstrArg *);
	tmp->i           = i;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_phi(Context *cnt, Ast *node)
{
	MirInstrPhi *tmp     = create_instr(cnt, MIR_INSTR_PHI, node, MirInstrPhi *);
	tmp->incoming_values = create_sarr(SmallArray_Instr, cnt->assembly);
	tmp->incoming_blocks = create_sarr(SmallArray_Instr, cnt->assembly);
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_compound(Context *cnt, Ast *node, MirInstr *type, SmallArray_Instr *values)
{
	if (values) {
		MirInstr *value;
		sarray_foreach(values, value) ref_instr(value);
	}
	ref_instr(type);

	MirInstrCompound *tmp = create_instr(cnt, MIR_INSTR_COMPOUND, node, MirInstrCompound *);
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
	MirInstrCast *tmp = create_instr(cnt, MIR_INSTR_CAST, node, MirInstrCast *);
	tmp->type         = type;
	tmp->expr         = next;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_sizeof(Context *cnt, Ast *node, MirInstr *expr)
{
	ref_instr(expr);
	MirInstrSizeof *tmp  = create_instr(cnt, MIR_INSTR_SIZEOF, node, MirInstrSizeof *);
	tmp->base.value.type = cnt->builtin_types.entry_usize;
	tmp->base.comptime   = true;
	tmp->expr            = expr;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
create_instr_type_info(Context *cnt, Ast *node, MirInstr *expr)
{
	ref_instr(expr);
	MirInstrTypeInfo *tmp = create_instr(cnt, MIR_INSTR_TYPE_INFO, node, MirInstrTypeInfo *);
	tmp->expr             = expr;
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
	MirInstrAlignof *tmp = create_instr(cnt, MIR_INSTR_ALIGNOF, node, MirInstrAlignof *);
	tmp->base.value.type = cnt->builtin_types.entry_usize;
	tmp->base.comptime   = true;
	tmp->expr            = expr;

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
	assert(cond && then_block && else_block);
	ref_instr(cond);
	ref_instr(&then_block->base);
	ref_instr(&else_block->base);
	MirInstrCondBr *tmp  = create_instr(cnt, MIR_INSTR_COND_BR, node, MirInstrCondBr *);
	tmp->base.ref_count  = NO_REF_COUNTING;
	tmp->base.value.type = cnt->builtin_types.entry_void;
	tmp->cond            = cond;
	tmp->then_block      = then_block;
	tmp->else_block      = else_block;

	MirInstrBlock *block = get_current_block(cnt);

	append_current_block(cnt, &tmp->base);
	if (!is_block_terminated(block)) terminate_block(block, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_br(Context *cnt, Ast *node, MirInstrBlock *then_block)
{
	assert(then_block);
	ref_instr(&then_block->base);
	MirInstrBr *tmp      = create_instr(cnt, MIR_INSTR_BR, node, MirInstrBr *);
	tmp->base.ref_count  = NO_REF_COUNTING;
	tmp->base.value.type = cnt->builtin_types.entry_void;
	tmp->then_block      = then_block;

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
	assert(arr_ptr && index);
	ref_instr(arr_ptr);
	ref_instr(index);
	MirInstrElemPtr *tmp = create_instr(cnt, MIR_INSTR_ELEM_PTR, node, MirInstrElemPtr *);
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
	MirInstrMemberPtr *tmp = create_instr(cnt, MIR_INSTR_MEMBER_PTR, node, MirInstrMemberPtr *);
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
	MirInstrLoad *tmp = create_instr(cnt, MIR_INSTR_LOAD, node, MirInstrLoad *);
	tmp->src          = src;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
create_instr_addrof(Context *cnt, Ast *node, MirInstr *src)
{
	ref_instr(src);
	MirInstrAddrOf *tmp = create_instr(cnt, MIR_INSTR_ADDROF, node, MirInstrAddrOf *);
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
	    create_instr(cnt, MIR_INSTR_UNREACHABLE, node, MirInstrUnreachable *);
	tmp->base.value.type = cnt->builtin_types.entry_void;
	tmp->base.ref_count  = NO_REF_COUNTING;
	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_fn_proto(Context *cnt, Ast *node, MirInstr *type, MirInstr *user_type)
{
	MirInstrFnProto *tmp = create_instr(cnt, MIR_INSTR_FN_PROTO, node, MirInstrFnProto *);
	tmp->type            = type;
	tmp->user_type       = user_type;
	tmp->base.comptime   = true;
	tmp->base.ref_count  = NO_REF_COUNTING;

	push_into_gscope(cnt, &tmp->base);
	analyze_push_back(cnt, &tmp->base);
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
	assert(scope && rid);
	MirInstrDeclRef *tmp = create_instr(cnt, MIR_INSTR_DECL_REF, node, MirInstrDeclRef *);
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
	assert(ref);
	ref_instr(ref);
	MirInstrDeclDirectRef *tmp =
	    create_instr(cnt, MIR_INSTR_DECL_DIRECT_REF, NULL, MirInstrDeclDirectRef *);
	tmp->ref = ref;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_call(Context *cnt, Ast *node, MirInstr *callee, SmallArray_Instr *args)
{
	assert(callee);
	MirInstrCall *tmp = create_instr(cnt, MIR_INSTR_CALL, node, MirInstrCall *);
	tmp->args         = args;
	tmp->callee       = callee;

	ref_instr(&tmp->base);

	/* Callee must be referenced even if we call no-ref counted fn_proto instructions, because
	 * sometimes callee is declaration reference poining to variable containing pointer to some
	 * function. */
	ref_instr(callee);

	/* reference all arguments */
	if (args) {
		MirInstr *instr;
		sarray_foreach(args, instr) ref_instr(instr);
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
                      int32_t   order,
                      uint32_t  flags)
{
	ref_instr(type);
	ref_instr(init);
	MirInstrDeclVar *tmp = create_instr(cnt, MIR_INSTR_DECL_VAR, node, MirInstrDeclVar *);
	tmp->base.ref_count  = NO_REF_COUNTING;
	tmp->base.value.type = cnt->builtin_types.entry_void;
	tmp->type            = type;
	tmp->init            = init;

	tmp->var = create_var(cnt,
	                      node,
	                      node->owner_scope,
	                      &node->data.ident.id,
	                      NULL,
	                      is_mutable,
	                      is_in_gscope,
	                      order,
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
                           int32_t     order,
                           uint32_t    flags)
{
	ref_instr(type);
	ref_instr(init);
	MirInstrDeclVar *tmp = create_instr(cnt, MIR_INSTR_DECL_VAR, NULL, MirInstrDeclVar *);
	tmp->base.ref_count  = NO_REF_COUNTING;
	tmp->base.value.type = cnt->builtin_types.entry_void;
	tmp->type            = type;
	tmp->init            = init;

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
	ref_instr(type);
	MirInstrDeclMember *tmp =
	    create_instr(cnt, MIR_INSTR_DECL_MEMBER, node, MirInstrDeclMember *);
	tmp->base.ref_count  = NO_REF_COUNTING;
	tmp->base.comptime   = true;
	tmp->base.value.type = cnt->builtin_types.entry_void;
	tmp->type            = type;

	ID *id      = node ? &node->data.ident.id : NULL;
	tmp->member = create_member(cnt, node, id, NULL, -1, NULL);

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

static MirInstr *
append_instr_decl_variant(Context *cnt, Ast *node, MirInstr *value)
{
	MirInstrDeclVariant *tmp =
	    create_instr(cnt, MIR_INSTR_DECL_VARIANT, node, MirInstrDeclVariant *);

	tmp->base.ref_count  = NO_REF_COUNTING;
	tmp->base.comptime   = true;
	tmp->base.value.type = cnt->builtin_types.entry_void;
	tmp->value           = value;

	assert(node && node->kind == AST_IDENT);
	ID *   id    = &node->data.ident.id;
	Scope *scope = node->owner_scope;
	tmp->variant = create_variant(cnt, node, id, scope, NULL);

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

static MirInstr *
create_instr_const_usize(Context *cnt, Ast *node, uint64_t val)
{
	MirInstr *tmp         = create_instr(cnt, MIR_INSTR_CONST, node, MirInstr *);
	tmp->comptime         = true;
	tmp->value.type       = cnt->builtin_types.entry_usize;
	tmp->value.data.v_u64 = val;

	return tmp;
}

MirInstr *
append_instr_const_int(Context *cnt, Ast *node, uint64_t val)
{
	MirInstr *tmp         = create_instr(cnt, MIR_INSTR_CONST, node, MirInstr *);
	tmp->comptime         = true;
	tmp->value.type       = cnt->builtin_types.entry_s32;
	tmp->value.data.v_s64 = (int64_t)val;

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_float(Context *cnt, Ast *node, float val)
{
	MirInstr *tmp   = create_instr(cnt, MIR_INSTR_CONST, node, MirInstr *);
	tmp->comptime   = true;
	tmp->value.type = cnt->builtin_types.entry_f32;
	// memcpy(&tmp->const_value.data, &val, sizeof(float));
	tmp->value.data.v_f32 = val;

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_double(Context *cnt, Ast *node, double val)
{
	MirInstr *tmp         = create_instr(cnt, MIR_INSTR_CONST, node, MirInstr *);
	tmp->comptime         = true;
	tmp->value.type       = cnt->builtin_types.entry_f64;
	tmp->value.data.v_f64 = val;

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_bool(Context *cnt, Ast *node, bool val)
{
	MirInstr *tmp          = create_instr(cnt, MIR_INSTR_CONST, node, MirInstr *);
	tmp->comptime          = true;
	tmp->value.type        = cnt->builtin_types.entry_bool;
	tmp->value.data.v_bool = val;

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_string(Context *cnt, Ast *node, const char *str)
{
	MirInstr *tmp        = create_instr(cnt, MIR_INSTR_CONST, node, MirInstr *);
	tmp->comptime        = true;
	tmp->value.type      = cnt->builtin_types.entry_string;
	tmp->value.addr_mode = MIR_VAM_LVALUE_CONST;

	{ /* initialize constant slice */
		SmallArray_ConstValue *members = create_sarr(SmallArray_ConstValue, cnt->assembly);
		MirConstValue *        value;

		/* string slice len */
		value = create_const_value(
		    cnt,
		    mir_get_struct_elem_type(cnt->builtin_types.entry_string, MIR_SLICE_LEN_INDEX));

		value->data.v_u64 = strlen(str);
		sa_push_ConstValue(members, value);

		/* string slice ptr */
		value = create_const_value(
		    cnt,
		    mir_get_struct_elem_type(cnt->builtin_types.entry_string, MIR_SLICE_PTR_INDEX));

		MirConstPtr *const_ptr = &value->data.v_ptr;
		set_const_ptr(const_ptr, (void *)str, MIR_CP_STR);
		sa_push_ConstValue(members, value);

		tmp->value.data.v_struct.members = members;
	}

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_char(Context *cnt, Ast *node, char c)
{
	MirInstr *tmp          = create_instr(cnt, MIR_INSTR_CONST, node, MirInstr *);
	tmp->comptime          = true;
	tmp->value.type        = cnt->builtin_types.entry_u8;
	tmp->value.data.v_char = c;

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_const_null(Context *cnt, Ast *node)
{
	MirInstr *tmp   = create_instr(cnt, MIR_INSTR_CONST, node, MirInstr *);
	tmp->comptime   = true;
	tmp->value.type = create_type_null(cnt, cnt->builtin_types.entry_u8_ptr);

	set_const_ptr(&tmp->value.data.v_ptr, NULL, MIR_CP_VALUE);

	append_current_block(cnt, tmp);
	return tmp;
}

MirInstr *
append_instr_ret(Context *cnt, Ast *node, MirInstr *value, bool allow_fn_ret_type_override)
{
	if (value) ref_instr(value);

	MirInstrRet *tmp                = create_instr(cnt, MIR_INSTR_RET, node, MirInstrRet *);
	tmp->base.value.type            = cnt->builtin_types.entry_void;
	tmp->base.ref_count             = NO_REF_COUNTING;
	tmp->value                      = value;
	tmp->allow_fn_ret_type_override = allow_fn_ret_type_override;

	MirInstrBlock *block = get_current_block(cnt);

	append_current_block(cnt, &tmp->base);
	if (!is_block_terminated(block)) terminate_block(block, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_store(Context *cnt, Ast *node, MirInstr *src, MirInstr *dest)
{
	assert(src && dest);
	ref_instr(src);
	ref_instr(dest);

	MirInstrStore *tmp   = create_instr(cnt, MIR_INSTR_STORE, node, MirInstrStore *);
	tmp->base.value.type = cnt->builtin_types.entry_void;
	tmp->base.ref_count  = NO_REF_COUNTING;
	tmp->src             = src;
	tmp->dest            = dest;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_binop(Context *cnt, Ast *node, MirInstr *lhs, MirInstr *rhs, BinopKind op)
{
	assert(lhs && rhs);
	ref_instr(lhs);
	ref_instr(rhs);
	MirInstrBinop *tmp = create_instr(cnt, MIR_INSTR_BINOP, node, MirInstrBinop *);
	tmp->lhs           = lhs;
	tmp->rhs           = rhs;
	tmp->op            = op;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
append_instr_unop(Context *cnt, Ast *node, MirInstr *instr, UnopKind op)
{
	assert(instr);
	ref_instr(instr);
	MirInstrUnop *tmp = create_instr(cnt, MIR_INSTR_UNOP, node, MirInstrUnop *);
	tmp->expr         = instr;
	tmp->op           = op;

	append_current_block(cnt, &tmp->base);
	return &tmp->base;
}

MirInstr *
create_instr_vargs_impl(Context *cnt, MirType *type, SmallArray_Instr *values)
{
	assert(type);
	MirInstrVArgs *tmp = create_instr(cnt, MIR_INSTR_VARGS, NULL, MirInstrVArgs *);
	tmp->type          = type;
	tmp->values        = values;

	return &tmp->base;
}

/* analyze */
void
erase_instr_tree(MirInstr *instr)
{
	if (!instr) return;

	SmallArray_Instr64 queue;
	sa_init(&queue);

	sa_push_Instr64(&queue, instr);

	MirInstr *top;
	while (queue.size) {
		top = sa_pop_Instr64(&queue);

		if (!top) continue;

		assert(top->analyzed && "Trying to erase not analyzed instruction.");
		if (top->ref_count == NO_REF_COUNTING) continue;
		if (top->ref_count > 0) continue;

		switch (top->kind) {
		case MIR_INSTR_BINOP: {
			MirInstrBinop *binop = (MirInstrBinop *)top;
			unref_instr(binop->lhs);
			unref_instr(binop->rhs);

			sa_push_Instr64(&queue, binop->rhs);
			sa_push_Instr64(&queue, binop->lhs);
			break;
		}

		case MIR_INSTR_LOAD: {
			MirInstrLoad *load = (MirInstrLoad *)top;
			unref_instr(load->src);

			sa_push_Instr64(&queue, load->src);
			break;
		}

		case MIR_INSTR_SIZEOF: {
			MirInstrSizeof *szof = (MirInstrSizeof *)top;
			unref_instr(szof->expr);

			sa_push_Instr64(&queue, szof->expr);
			break;
		}

		case MIR_INSTR_ELEM_PTR: {
			MirInstrElemPtr *ep = (MirInstrElemPtr *)top;
			unref_instr(ep->arr_ptr);
			unref_instr(ep->index);

			sa_push_Instr64(&queue, ep->arr_ptr);
			sa_push_Instr64(&queue, ep->index);
			break;
		}

		case MIR_INSTR_MEMBER_PTR: {
			MirInstrMemberPtr *mp = (MirInstrMemberPtr *)top;
			unref_instr(mp->target_ptr);

			sa_push_Instr64(&queue, mp->target_ptr);
			break;
		}

		case MIR_INSTR_TYPE_INFO: {
			MirInstrTypeInfo *info = (MirInstrTypeInfo *)top;
			unref_instr(info->expr);

			sa_push_Instr64(&queue, info->expr);
			break;
		}

		case MIR_INSTR_CAST: {
			MirInstrCast *cast = (MirInstrCast *)top;
			unref_instr(cast->expr);
			unref_instr(cast->type);

			sa_push_Instr64(&queue, cast->expr);
			sa_push_Instr64(&queue, cast->type);
			break;
		}

		case MIR_INSTR_CALL: {
			MirInstrCall *call = (MirInstrCall *)top;
			if (call->args) {
				MirInstr *it;
				sarray_foreach(call->args, it)
				{
					unref_instr(it);
					sa_push_Instr64(&queue, it);
				}
			}
			break;
		}

		case MIR_INSTR_ADDROF: {
			MirInstrAddrOf *addrof = (MirInstrAddrOf *)top;
			unref_instr(addrof->src);
			sa_push_Instr64(&queue, addrof->src);
			break;
		}

		case MIR_INSTR_UNOP: {
			MirInstrUnop *unop = (MirInstrUnop *)top;
			unref_instr(unop->expr);
			sa_push_Instr64(&queue, unop->expr);
			break;
		}

		case MIR_INSTR_VARGS: {
			MirInstrVArgs *vargs = (MirInstrVArgs *)top;
			if (vargs->values) {
				MirInstr *it;
				sarray_foreach(vargs->values, it)
				{
					unref_instr(it);
					sa_push_Instr64(&queue, it);
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
			bl_abort("Missing erase for instruction '%s'", mir_instr_name(top));
		}

		erase_instr(top);
	}

	sa_terminate(&queue);
}

void
reduce_instr(Context *cnt, MirInstr *instr)
{
	if (!instr) return;
	/* instruction unknown in compile time cannot be reduced */
	if (!instr->comptime && instr->kind != MIR_INSTR_COMPOUND) return;

	switch (instr->kind) {
	case MIR_INSTR_CONST:
	case MIR_INSTR_DECL_MEMBER:
	case MIR_INSTR_DECL_VARIANT:
	case MIR_INSTR_TYPE_FN:
	case MIR_INSTR_TYPE_ARRAY:
	case MIR_INSTR_TYPE_PTR:
	case MIR_INSTR_TYPE_STRUCT:
	case MIR_INSTR_TYPE_SLICE:
	case MIR_INSTR_TYPE_VARGS:
	case MIR_INSTR_TYPE_ENUM:
	case MIR_INSTR_SIZEOF:
	case MIR_INSTR_ALIGNOF:
	case MIR_INSTR_MEMBER_PTR: {
		erase_instr(instr);
		break;
	}

	case MIR_INSTR_COMPOUND: {
		if (!((MirInstrCompound *)instr)->is_naked) erase_instr(instr);
		break;
	}

	case MIR_INSTR_BINOP: {
		exec_instr_binop(cnt, (MirInstrBinop *)instr);
		erase_instr(instr);
		break;
	}

	case MIR_INSTR_UNOP: {
		exec_instr_unop(cnt, (MirInstrUnop *)instr);
		erase_instr(instr);
		break;
	}

	case MIR_INSTR_CAST: {
		exec_instr_cast(cnt, (MirInstrCast *)instr);
		erase_instr(instr);
		break;
	}

	case MIR_INSTR_LOAD: {
		exec_instr_load(cnt, (MirInstrLoad *)instr);
		erase_instr(instr);
		break;
	}

	case MIR_INSTR_DECL_REF: {
		exec_instr_decl_ref(cnt, (MirInstrDeclRef *)instr);
		erase_instr(instr);
		break;
	}

	case MIR_INSTR_DECL_DIRECT_REF: {
		exec_instr_decl_direct_ref(cnt, (MirInstrDeclDirectRef *)instr);
		erase_instr(instr);
		break;
	}

	case MIR_INSTR_ADDROF: {
		exec_instr_addrof(cnt, (MirInstrAddrOf *)instr);
		erase_instr(instr);
		break;
	}

	default:
		break;
	}
}

uint64_t
analyze_instr_toany(Context *cnt, MirInstrToAny *toany)
{
	MirInstr *expr = toany->expr;
	assert(expr && "Missing expression as toany input.");

	MirType *toany_type = mir_deref_type(toany->base.value.type);
	MirType *rtti_type  = expr->value.type;

	reduce_instr(cnt, toany->expr);

	if (!is_allocated_object(expr)) {
		/* Target expression is not allocated object on the stack, so we need to crate
		 * temporary variable containing the value and fetch pointer to this variable. */
		const char *tmp_var_name = gen_uq_name(cnt, IMPL_ANY_EXPR_TMP);
		toany->expr_tmp =
		    create_var_impl(cnt, tmp_var_name, rtti_type, false, false, false);
	} else if (is_load_needed(expr)) {
		rtti_type = mir_deref_type(rtti_type);
	}

	assert(rtti_type);
	schedule_RTTI_generation(cnt, rtti_type);

	{ /* Tmp variable for Any */
		const char *tmp_var_name = gen_uq_name(cnt, IMPL_ANY_TMP);
		toany->tmp = create_var_impl(cnt, tmp_var_name, toany_type, false, false, false);
	}

	toany->rtti_type = rtti_type;
	toany->has_data  = toany->rtti_type->kind != MIR_TYPE_TYPE &&
	                  toany->rtti_type->kind != MIR_TYPE_VOID &&
	                  toany->rtti_type->kind != MIR_TYPE_NULL;

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_phi(Context *cnt, MirInstrPhi *phi)
{
	assert(phi->incoming_blocks && phi->incoming_values);
	assert(phi->incoming_values->size == phi->incoming_blocks->size);

	const size_t count = phi->incoming_values->size;

	bool       comptime = true;
	MirInstr **value_ref;
	MirInstr * block;
	MirType *  type = NULL;

	for (size_t i = 0; i < count; ++i) {
		value_ref = &phi->incoming_values->data[i];
		block     = phi->incoming_blocks->data[i];
		assert(block && block->kind == MIR_INSTR_BLOCK);

		bool is_valid;
		(*value_ref) = analyze_slot_input(cnt, &is_valid, *value_ref, type, false);
		if (!is_valid) return ANALYZE_FAILED;

		if (!type) type = (*value_ref)->value.type;

		comptime &= (*value_ref)->comptime;
	}

	assert(type && "Cannot resolve type of phi instruction!");
	phi->base.value.type      = type;
	phi->base.value.addr_mode = MIR_VAM_RVALUE;
	phi->base.comptime        = comptime;

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_compound(Context *cnt, MirInstrCompound *cmp)
{
	SmallArray_Instr *values = cmp->values;

	/* Setup compound type. */
	MirType *type = cmp->base.value.type;
	if (!type) {
		/* generate load instruction if needed */

		cmp->type = analyze_slot_input(cnt, NULL, cmp->type, NULL, false);

		MirInstr *instr_type = cmp->type;
		if (instr_type->value.type->kind != MIR_TYPE_TYPE) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_INVALID_TYPE,
			            instr_type->node->location,
			            BUILDER_CUR_WORD,
			            "Expected type before compound expression.");
			return ANALYZE_FAILED;
		}
		type = instr_type->value.data.v_ptr.data.type;
	}

	assert(type);

	MirInstr *   value;
	const size_t valc      = values ? values->size : 0;
	bool         comptime  = true;
	bool         zero_init = false;

	/* Check if array is supposed to be initilialized to {0} */
	if (valc == 1) {
		value = values->data[0];
		if (value->kind == MIR_INSTR_CONST && value->value.type->kind == MIR_TYPE_INT &&
		    value->value.data.v_u64 == 0) {
			reduce_instr(cnt, value);
			zero_init = true;
		}
	}

	cmp->is_zero_initialized = zero_init;

	switch (type->kind) {
	case MIR_TYPE_ARRAY: {
		if (zero_init) {
			cmp->base.value.data.v_array.is_zero_initializer = true;
			break;
		}

		if (valc != type->data.array.len) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_INVALID_INITIALIZER,
			            cmp->base.node->location,
			            BUILDER_CUR_WORD,
			            "Array initializer must explicitly set all array elements of "
			            "the array or "
			            "initialize array to 0 by zero initializer {0}. Expected is "
			            "%llu but given %llu.",
			            (unsigned long long)type->data.array.len,
			            (unsigned long long)valc);
			return ANALYZE_FAILED;
		}

		/* Else iterate over values */
		MirInstr **value_ref;
		for (size_t i = 0; i < valc; ++i) {
			value_ref = &values->data[i];

			bool is_valid;
			(*value_ref) = analyze_slot_input(
			    cnt, &is_valid, *value_ref, type->data.array.elem_type, false);
			if (!is_valid) return ANALYZE_FAILED;

			comptime = (*value_ref)->comptime ? comptime : false;
		}

		// NOTE: Instructions can be used as values!!!
		cmp->base.value.data.v_array.elems = (SmallArray_ConstValue *)values;
		break;
	}

	case MIR_TYPE_SLICE:
	case MIR_TYPE_STRING:
	case MIR_TYPE_VARGS:
	case MIR_TYPE_STRUCT: {
		comptime = true;

		if (zero_init) {
			cmp->base.value.data.v_struct.is_zero_initializer = true;
			break;
		}

		const size_t memc = type->data.strct.members->size;
		if (valc != memc) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_INVALID_INITIALIZER,
			            cmp->base.node->location,
			            BUILDER_CUR_WORD,
			            "Structure initializer must explicitly set all members of the "
			            "structure or initialize structure to 0 by zero initializer "
			            "{0}. Expected is %llu but given %llu.",
			            (unsigned long long)memc,
			            (unsigned long long)valc);
			return ANALYZE_FAILED;
		}

		/* Else iterate over values */
		MirInstr **value_ref;
		MirType *  member_type;
		for (uint32_t i = 0; i < valc; ++i) {
			value_ref   = &values->data[i];
			member_type = mir_get_struct_elem_type(type, i);

			bool is_valid;
			(*value_ref) =
			    analyze_slot_input(cnt, &is_valid, *value_ref, member_type, false);
			if (!is_valid) return ANALYZE_FAILED;

			comptime = (*value_ref)->comptime ? comptime : false;
		}

		// NOTE: Instructions can be used as values!!!
		cmp->base.value.data.v_struct.members = (SmallArray_ConstValue *)values;
		break;
	}

	default: {
		/* Non-agregate type. */
		if (valc > 1) {
			value = values->data[1];
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_INVALID_INITIALIZER,
			            value->node->location,
			            BUILDER_CUR_WORD,
			            "One value only is expected for non-agragate types.");
			return ANALYZE_FAILED;
		}

		MirInstr **value_ref = &values->data[0];

		bool is_valid;
		(*value_ref) = analyze_slot_input(cnt, &is_valid, *value_ref, type, false);
		if (!is_valid) return ANALYZE_FAILED;

		comptime = (*value_ref)->comptime ? comptime : false;

		// NOTE: Instructions can be used as values!!!
		cmp->base.value = (*value_ref)->value;
	}
	}

	/*
	 * Create tmp variable for naked compound if needed.
	 */
	if (cmp->is_naked) {
		cmp->base.value.type = create_type_ptr(cnt, type);
		cmp->base.comptime   = false;

		const char *tmp_name = gen_uq_name(cnt, IMPL_COMPOUND_TMP);
		MirVar *    tmp_var  = create_var_impl(cnt, tmp_name, type, true, false, false);
		cmp->tmp_var         = tmp_var;
	} else {
		cmp->base.value.type = type;
		cmp->base.comptime   = comptime;
	}

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_vargs(Context *cnt, MirInstrVArgs *vargs)
{
	MirType *         type   = vargs->type;
	SmallArray_Instr *values = vargs->values;
	assert(type && values);

	type = create_type_struct_special(cnt, MIR_TYPE_VARGS, NULL, create_type_ptr(cnt, type));

	const size_t valc = values->size;

	if (valc > 0) {
		/* Prepare tmp array for values */
		const char *tmp_name = gen_uq_name(cnt, IMPL_VARGS_TMP_ARR);
		MirType *   tmp_type = create_type_array(cnt, vargs->type, valc);
		vargs->arr_tmp       = create_var_impl(cnt, tmp_name, tmp_type, true, false, false);
	}

	{
		/* Prepare tmp slice for vargs */
		const char *tmp_name = gen_uq_name(cnt, IMPL_VARGS_TMP);
		vargs->vargs_tmp     = create_var_impl(cnt, tmp_name, type, true, false, false);
	}

	MirInstr **value;
	bool       is_valid = true;

	for (size_t i = 0; i < valc && is_valid; ++i) {
		value = &values->data[i];

		(*value) = analyze_slot_input(cnt, NULL, *value, vargs->type, true);
	}

	vargs->base.value.type = type;
	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_elem_ptr(Context *cnt, MirInstrElemPtr *elem_ptr)
{
	bool is_valid;
	elem_ptr->index = analyze_slot_input(
	    cnt, &is_valid, elem_ptr->index, cnt->builtin_types.entry_usize, false);
	if (!is_valid) return ANALYZE_FAILED;

	MirInstr *arr_ptr = elem_ptr->arr_ptr;
	assert(arr_ptr);
	assert(arr_ptr->value.type);

	if (!mir_is_pointer_type(arr_ptr->value.type)) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            elem_ptr->arr_ptr->node->location,
		            BUILDER_CUR_WORD,
		            "Expected array type or slice.");
		return ANALYZE_FAILED;
	}

	MirType *arr_type = mir_deref_type(arr_ptr->value.type);
	assert(arr_type);

	if (arr_type->kind == MIR_TYPE_ARRAY) {
		/* array */
		if (elem_ptr->index->comptime) {
			const size_t len = arr_type->data.array.len;
			const size_t i   = elem_ptr->index->value.data.v_u64;
			if (i >= len) {
				builder_msg(cnt->builder,
				            BUILDER_MSG_ERROR,
				            ERR_BOUND_CHECK_FAILED,
				            elem_ptr->index->node->location,
				            BUILDER_CUR_WORD,
				            "Array index is out of the bounds (%llu)",
				            i);
				return ANALYZE_FAILED;
			}
		}

		/* setup ElemPtr instruction const_value type */
		MirType *elem_type = arr_type->data.array.elem_type;
		assert(elem_type);
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
		assert(elem_type);
		elem_ptr->base.value.type = elem_type;

		/* this is important!!! */
		elem_ptr->target_is_slice = true;
	} else {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            arr_ptr->node->location,
		            BUILDER_CUR_WORD,
		            "Expected array or slice type.");
		return ANALYZE_FAILED;
	}

	reduce_instr(cnt, elem_ptr->arr_ptr);
	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_member_ptr(Context *cnt, MirInstrMemberPtr *member_ptr)
{
	MirInstr *target_ptr = member_ptr->target_ptr;
	assert(target_ptr);
	MirType *target_type = target_ptr->value.type;

	if (target_type->kind != MIR_TYPE_PTR) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            target_ptr->node->location,
		            BUILDER_CUR_WORD,
		            "Expected structure type.");
		return ANALYZE_FAILED;
	}

	Ast *ast_member_ident = member_ptr->member_ident;

	target_type = mir_deref_type(target_type);

	/* Array type */
	if (target_type->kind == MIR_TYPE_ARRAY) {
		/* check array builtin members */
		if (member_ptr->builtin_id == MIR_BUILTIN_ID_ARR_LEN ||
		    is_builtin(ast_member_ident, MIR_BUILTIN_ID_ARR_LEN)) {
			/* .len */
			// assert(member_ptr->target_ptr->kind == MIR_INSTR_DECL_REF);
			erase_instr(member_ptr->target_ptr);
			/* mutate instruction into constant */
			MirInstr *len         = mutate_instr(&member_ptr->base, MIR_INSTR_CONST);
			len->comptime         = true;
			len->value.type       = cnt->builtin_types.entry_usize;
			len->value.data.v_u64 = target_type->data.array.len;
		} else if (member_ptr->builtin_id == MIR_BUILTIN_ID_ARR_PTR ||
		           is_builtin(ast_member_ident, MIR_BUILTIN_ID_ARR_PTR)) {
			/* .ptr -> This will be replaced by:
			 *     elemptr
			 *     addrof
			 * to match syntax: &array[0]
			 */

			MirInstr *index = create_instr_const_usize(cnt, NULL, 0);
			MirInstr *elem_ptr =
			    create_instr_elem_ptr(cnt, NULL, target_ptr, index, false);
			ref_instr(elem_ptr);

			insert_instr_before(&member_ptr->base, elem_ptr);

			analyze_instr_rq(cnt, index);
			analyze_instr_rq(cnt, elem_ptr);

			MirInstrAddrOf *addrof_elem =
			    (MirInstrAddrOf *)mutate_instr(&member_ptr->base, MIR_INSTR_ADDROF);
			addrof_elem->src = elem_ptr;
			analyze_instr_rq(cnt, &addrof_elem->base);
		} else {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_INVALID_MEMBER_ACCESS,
			            ast_member_ident->location,
			            BUILDER_CUR_WORD,
			            "Unknown member.");
		}

		return ANALYZE_PASSED;
	}

	if (target_type->kind == MIR_TYPE_PTR) {
		/* We try to access structure member via pointer so we need one more load. */

		member_ptr->target_ptr = insert_instr_load(cnt, member_ptr->target_ptr);
		assert(member_ptr->target_ptr);
		target_type = mir_deref_type(target_type);
	}

	/* struct type */
	if (target_type->kind == MIR_TYPE_STRUCT || target_type->kind == MIR_TYPE_STRING ||
	    target_type->kind == MIR_TYPE_SLICE || target_type->kind == MIR_TYPE_VARGS) {
		reduce_instr(cnt, member_ptr->target_ptr);
		/* lookup for member inside struct */
		Scope *     scope = target_type->data.strct.scope;
		ID *        rid   = &ast_member_ident->data.ident.id;
		ScopeEntry *found = scope_lookup(scope, rid, false, true);
		if (!found) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_UNKNOWN_SYMBOL,
			            member_ptr->member_ident->location,
			            BUILDER_CUR_WORD,
			            "Unknown structure member.");
			return ANALYZE_FAILED;
		}

		{
			assert(found->kind == SCOPE_ENTRY_MEMBER);
			MirMember *member = found->data.member;

			/* setup member_ptr type */
			MirType *type = create_type_ptr(cnt, member->type);
			assert(type);
			member_ptr->base.value.type = type;
		}

		member_ptr->scope_entry = found;

		return ANALYZE_PASSED;
	}

	/* Sub type member. */
	if (target_type->kind == MIR_TYPE_TYPE) {
		/* generate load instruction if needed */

		member_ptr->target_ptr =
		    analyze_slot_input(cnt, NULL, member_ptr->target_ptr, NULL, false);

		MirType *sub_type = member_ptr->target_ptr->value.data.v_ptr.data.type;
		assert(sub_type);

		if (sub_type->kind != MIR_TYPE_ENUM) {
			goto INVALID;
		}

		/* lookup for member inside struct */
		Scope *     scope = sub_type->data.enm.scope;
		ID *        rid   = &ast_member_ident->data.ident.id;
		ScopeEntry *found = scope_lookup(scope, rid, false, true);
		if (!found) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_UNKNOWN_SYMBOL,
			            member_ptr->member_ident->location,
			            BUILDER_CUR_WORD,
			            "Unknown enumerator variant.");
			return ANALYZE_FAILED;
		}

		{
			assert(found->kind == SCOPE_ENTRY_VARIANT);
			MirVariant *variant = found->data.variant;
			assert(variant);
			member_ptr->base.value.data      = variant->value->data;
			member_ptr->base.value.addr_mode = MIR_VAM_LVALUE_CONST;
		}

		member_ptr->scope_entry     = found;
		member_ptr->base.value.type = sub_type;
		member_ptr->base.comptime   = true;

		return ANALYZE_PASSED;
	}

	/* Invalid */
INVALID:
	builder_msg(cnt->builder,
	            BUILDER_MSG_ERROR,
	            ERR_INVALID_MEMBER_ACCESS,
	            target_ptr->node->location,
	            BUILDER_CUR_WORD,
	            "Expected structure or enumerator type.");
	return ANALYZE_FAILED;
}

uint64_t
analyze_instr_addrof(Context *cnt, MirInstrAddrOf *addrof)
{
	MirInstr *src = addrof->src;
	assert(src);

	if (!is_allocated_object(src)) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_EXPECTED_DECL,
		            addrof->base.node->location,
		            BUILDER_CUR_WORD,
		            "Cannot take the address of unallocated object.");
		return ANALYZE_FAILED;
	}

	if (src->value.addr_mode == MIR_VAM_LVALUE_CONST) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_ADDRES_MODE,
		            addrof->base.node->location,
		            BUILDER_CUR_WORD,
		            "Cannot take address of constant.");
	}

	/* setup type */
	MirType *type = NULL;
	assert(src->value.type);
	if (src->value.type->kind == MIR_TYPE_FN) {
		type = create_type_ptr(cnt, src->value.type);
	} else {
		type = src->value.type;
	}

	addrof->base.value.type = type;
	addrof->base.comptime   = addrof->src->comptime;
	assert(addrof->base.value.type && "invalid type");

	reduce_instr(cnt, addrof->src);

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_cast(Context *cnt, MirInstrCast *cast)
{
	MirType *dest_type = cast->base.value.type;

	if (!dest_type) {
		assert(cast->type && cast->type->kind == MIR_INSTR_CALL);
		if (analyze_instr(cnt, cast->type) != ANALYZE_PASSED) return ANALYZE_POSTPONE;
		MirConstValue *type_val = exec_call_top_lvl(cnt, (MirInstrCall *)cast->type);
		// unref_instr(cast->type);
		assert(type_val->type && type_val->type->kind == MIR_TYPE_TYPE);
		dest_type = type_val->data.v_ptr.data.type;
	}

	cast->expr = analyze_slot_input(cnt, NULL, cast->expr, NULL, false);

	MirInstr *src = cast->expr;
	assert(src);
	MirType *src_type = src->value.type;

	assert(dest_type && "invalid cast destination type");
	assert(src_type && "invalid cast source type");

	if (cast->op == MIR_CAST_INVALID) {
		cast->op = get_cast_op(src_type, dest_type);
		if (cast->op == MIR_CAST_INVALID) {
			error_types(cnt,
			            src_type,
			            dest_type,
			            cast->base.node,
			            "Invalid cast from '%s' to '%s'.");
			return ANALYZE_FAILED;
		}
	}

	cast->base.value.type = dest_type;
	cast->base.comptime   = cast->expr->comptime;
	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_sizeof(Context *cnt, MirInstrSizeof *szof)
{
	assert(szof->expr);

	szof->expr = analyze_slot_input(cnt, NULL, szof->expr, NULL, false);

	MirType *type = szof->expr->value.type;
	assert(type);

	if (type->kind == MIR_TYPE_TYPE) {
		type = szof->expr->value.data.v_ptr.data.type;
		assert(type);
	}

	/* sizeof operator needs only type of input expression so we can erase whole call tree
	 * generated to get this expression */
	unref_instr(szof->expr);
	erase_instr_tree(szof->expr);
	szof->base.value.data.v_u64 = type->store_size_bytes;
	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_type_info(Context *cnt, MirInstrTypeInfo *type_info)
{
	assert(type_info->expr);

	/* Resolve TypeInfo struct type */
	MirType *ret_type = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO);
	if (!ret_type) return ANALYZE_POSTPONE;

	type_info->expr = analyze_slot_input(cnt, NULL, type_info->expr, NULL, false);

	MirType *type = type_info->expr->value.type;
	assert(type);

	if (type->kind == MIR_TYPE_TYPE) {
		type = type_info->expr->value.data.v_ptr.data.type;
		assert(type);
	}

	type_info->expr_type = type;

	schedule_RTTI_generation(cnt, type);

	ret_type                   = create_type_ptr(cnt, ret_type);
	type_info->base.value.type = ret_type;

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_alignof(Context *cnt, MirInstrAlignof *alof)
{
	assert(alof->expr);

	alof->expr = analyze_slot_input(cnt, NULL, alof->expr, NULL, false);

	MirType *type = alof->expr->value.type;
	assert(type);

	if (type->kind == MIR_TYPE_TYPE) {
		type = alof->expr->value.data.v_ptr.data.type;
		assert(type);
	}

	alof->base.value.data.v_u64 = type->alignment;
	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_decl_ref(Context *cnt, MirInstrDeclRef *ref)
{
	assert(ref->rid && ref->scope);

	ScopeEntry *found         = NULL;
	Scope *     private_scope = ref->parent_unit->private_scope;

	if (!private_scope) { /* reference in unit without private scope  */
		found = scope_lookup(ref->scope, ref->rid, true, false);
	} else { /* reference in unit with private scope */
		/* search in current tree and ignore global scope */
		found = scope_lookup(ref->scope, ref->rid, true, true);

		/* lookup in private scope and global scope also (private scope has global scope as
		 * parent every time) */
		if (!found) found = scope_lookup(private_scope, ref->rid, true, false);
	}

	if (found ? found->kind == SCOPE_ENTRY_INCOMPLETE : true) {
		return ref->rid->hash;
	}

	switch (found->kind) {
	case SCOPE_ENTRY_FN: {
		MirFn *fn = found->data.fn;
		assert(fn);
		MirType *type = fn->type;
		assert(type);

		ref->base.value.type = type;
		ref->base.comptime   = true;
		ref_instr(fn->prototype);
		set_const_ptr(&ref->base.value.data.v_ptr, found->data.fn, MIR_CP_FN);
		break;
	}

	case SCOPE_ENTRY_TYPE: {
		ref->base.value.type = cnt->builtin_types.entry_type;
		ref->base.comptime   = true;
		set_const_ptr(&ref->base.value.data.v_ptr, found->data.type, MIR_CP_TYPE);
		break;
	}

	case SCOPE_ENTRY_VARIANT: {
		MirVariant *variant = found->data.variant;
		assert(variant);

		MirType *type = variant->value->type;
		assert(type);

		type                      = create_type_ptr(cnt, type);
		ref->base.value.type      = type;
		ref->base.comptime        = true;
		ref->base.value.addr_mode = MIR_VAM_LVALUE_CONST;
		set_const_ptr(&ref->base.value.data.v_ptr, variant->value, MIR_CP_VALUE);

		break;
	}

	case SCOPE_ENTRY_VAR: {
		MirVar *var = found->data.var;
		assert(var);
		++var->ref_count;
		MirType *type = var->value.type;
		assert(type);

		type                      = create_type_ptr(cnt, type);
		ref->base.value.type      = type;
		ref->base.comptime        = var->comptime;
		ref->base.value.addr_mode = var->is_mutable ? MIR_VAM_LVALUE : MIR_VAM_LVALUE_CONST;

		/* set pointer to variable const value directly when variable is compile
		 * time known
		 */
		if (var->comptime) set_const_ptr(&ref->base.value.data.v_ptr, var, MIR_CP_VAR);
		break;
	}

	default:
		bl_abort("invalid scope entry kind");
	}

	ref->scope_entry = found;
	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_decl_direct_ref(Context *cnt, MirInstrDeclDirectRef *ref)
{
	assert(ref->ref && "Missing declaration reference for direct ref.");
	assert(ref->ref->kind == MIR_INSTR_DECL_VAR && "Expected variable declaration.");
	assert(ref->ref->analyzed && "Reference not analyzed.");

	MirVar *var = ((MirInstrDeclVar *)ref->ref)->var;
	assert(var);
	++var->ref_count;
	MirType *type = var->value.type;
	assert(type);

	type                      = create_type_ptr(cnt, type);
	ref->base.value.type      = type;
	ref->base.comptime        = var->comptime;
	ref->base.value.addr_mode = var->is_mutable ? MIR_VAM_LVALUE : MIR_VAM_LVALUE_CONST;

	/* set pointer to variable const value directly when variable is compile
	 * time known
	 */
	if (var->comptime) set_const_ptr(&ref->base.value.data.v_ptr, var, MIR_CP_VAR);

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_arg(Context *cnt, MirInstrArg *arg)
{
	MirFn *fn = arg->base.owner_block->owner_fn;
	assert(fn);

	MirType *type = mir_get_fn_arg_type(fn->type, arg->i);
	assert(type);
	arg->base.value.type = type;

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_unreachable(Context *cnt, MirInstrUnreachable *unr)
{
	/* nothing to do :( */
	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_fn_proto(Context *cnt, MirInstrFnProto *fn_proto)
{
	/* resolve type */
	if (!fn_proto->base.value.type) {
		/* Analyze type of funcion literal. Here we expect call to type resolver
		 * function!
		 */
		assert(fn_proto->type && fn_proto->type->kind == MIR_INSTR_CALL);
		if (analyze_instr(cnt, fn_proto->type) != ANALYZE_PASSED) return ANALYZE_POSTPONE;
		MirConstValue *type_val = exec_call_top_lvl(cnt, (MirInstrCall *)fn_proto->type);
		// unref_instr(fn_proto->type);
		assert(type_val->type && type_val->type->kind == MIR_TYPE_TYPE);

		/* Analyze user defined type (this must be compared with infered type).
		 */
		if (fn_proto->user_type) {
			assert(fn_proto->user_type->kind == MIR_INSTR_CALL);
			if (analyze_instr(cnt, fn_proto->user_type) != ANALYZE_PASSED)
				return ANALYZE_POSTPONE;
			MirConstValue *user_type_val =
			    exec_call_top_lvl(cnt, (MirInstrCall *)fn_proto->user_type);
			// unref_instr(fn_proto->user_type);
			assert(user_type_val->type && user_type_val->type->kind == MIR_TYPE_TYPE);

			if (!type_cmp(type_val->data.v_ptr.data.type,
			              user_type_val->data.v_ptr.data.type)) {
				error_types(cnt,
				            type_val->data.v_ptr.data.type,
				            user_type_val->data.v_ptr.data.type,
				            fn_proto->user_type->node,
				            NULL);
			}
		}

		if (!type_val->data.v_ptr.data.type) return ANALYZE_FAILED;
		assert(type_val->data.v_ptr.data.type->kind == MIR_TYPE_FN);
		fn_proto->base.value.type = type_val->data.v_ptr.data.type;
	}

	MirConstValue *value = &fn_proto->base.value;

	assert(value->type && "function has no valid type");
	assert(value->data.v_ptr.data.fn);
	value->data.v_ptr.data.fn->type = fn_proto->base.value.type;

	MirFn *fn = fn_proto->base.value.data.v_ptr.data.fn;
	assert(fn);

	if (fn->ret_tmp) {
		assert(fn->ret_tmp->kind == MIR_INSTR_DECL_VAR);
		((MirInstrDeclVar *)fn->ret_tmp)->var->value.type = value->type->data.fn.ret_type;
	}

	/* implicit functions has no name -> generate one */
	if (!fn->llvm_name) {
		fn->llvm_name = gen_uq_name(cnt, IMPL_FN_NAME);
		ref_instr(fn->prototype);
	}

	if (is_flag(fn->flags, FLAG_EXTERN)) {
		/* lookup external function exec handle */
		assert(fn->llvm_name);
		fn->extern_entry = assembly_find_extern(cnt->assembly, fn->llvm_name);

		if (!fn->extern_entry) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_UNKNOWN_SYMBOL,
			            fn_proto->base.node->location,
			            BUILDER_CUR_WORD,
			            "External symbol '%s' not found.",
			            fn->llvm_name);
		}
	} else {
		/* Add entry block of the function into analyze queue. */
		MirInstr *entry_block = (MirInstr *)fn->first_block;
		if (!entry_block) {
			/* TODO: not the best place to do this check, move into ast generation later
			 */
			/* TODO: not the best place to do this check, move into ast generation later
			 */
			/* TODO: not the best place to do this check, move into ast generation later
			 */
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_EXPECTED_BODY,
			            fn_proto->base.node->location,
			            BUILDER_CUR_WORD,
			            "Missing function body.");
			return ANALYZE_FAILED;
		}

		analyze_push_front(cnt, entry_block);
	}

	if (fn->id) commit_fn(cnt, fn);

	if (fn_proto->first_unrechable_location) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_WARNING,
		            0,
		            fn_proto->first_unrechable_location,
		            BUILDER_CUR_NONE,
		            "Unrechable code detected.");
	}

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_cond_br(Context *cnt, MirInstrCondBr *br)
{
	assert(br->cond && br->then_block && br->else_block);
	assert(br->cond->analyzed);

	bool is_valid;
	br->cond =
	    analyze_slot_input(cnt, &is_valid, br->cond, cnt->builtin_types.entry_bool, false);
	if (!is_valid) return ANALYZE_FAILED;

	/* PERFORMANCE: When condition is known in compile time, we can discard
	 * whole else/then block based on condition resutl. It is not possible
	 * because we don't have tracked down execution tree for now. */

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_br(Context *cnt, MirInstrBr *br)
{
	assert(br->then_block);
	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_load(Context *cnt, MirInstrLoad *load)
{
	MirInstr *src = load->src;
	assert(src);
	if (!mir_is_pointer_type(src->value.type)) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            src->node->location,
		            BUILDER_CUR_WORD,
		            "Expected pointer.");
		return ANALYZE_FAILED;
	}

	MirType *type = mir_deref_type(src->value.type);
	assert(type);
	load->base.value.type = type;

	reduce_instr(cnt, src);
	load->base.comptime        = src->comptime;
	load->base.value.addr_mode = MIR_VAM_RVALUE;

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_type_fn(Context *cnt, MirInstrTypeFn *type_fn)
{
	assert(type_fn->base.value.type);
	assert(type_fn->ret_type ? type_fn->ret_type->analyzed : true);

	bool is_vargs = false;

	SmallArray_Type *arg_types = NULL;
	if (type_fn->arg_types) {
		const size_t argc = type_fn->arg_types->size;
		arg_types         = create_sarr(SmallArray_Type, cnt->assembly);

		MirInstr **arg_type_ref;
		MirType *  tmp;
		for (size_t i = 0; i < argc; ++i) {
			arg_type_ref = &type_fn->arg_types->data[i];
			assert((*arg_type_ref)->comptime);

			(*arg_type_ref) = analyze_slot_input(cnt, NULL, *arg_type_ref, NULL, false);

			tmp = (*arg_type_ref)->value.data.v_ptr.data.type;
			assert(tmp);

			if (tmp->kind == MIR_TYPE_VARGS) {
				is_vargs = true;
				assert(i == type_fn->arg_types->size - 1 &&
				       "VArgs must be last, this should be an error");
			}

			sa_push_Type(arg_types, tmp);
		}
	}

	MirType *ret_type = NULL;
	if (type_fn->ret_type) {
		type_fn->ret_type = analyze_slot_input(cnt, NULL, type_fn->ret_type, NULL, false);

		assert(type_fn->ret_type->comptime);
		ret_type = type_fn->ret_type->value.data.v_ptr.data.type;
		assert(ret_type);
	}

	{
		MirConstPtr *const_ptr = &type_fn->base.value.data.v_ptr;
		set_const_ptr(
		    const_ptr, create_type_fn(cnt, ret_type, arg_types, is_vargs), MIR_CP_FN);
	}

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_decl_member(Context *cnt, MirInstrDeclMember *decl)
{
	decl->type = analyze_slot_input(cnt, NULL, decl->type, NULL, false);

	/* NOTE: Members will be provided by instr type struct because we need to
	 * know right ordering of members inside structure layout. (index and llvm
	 * element offet need to be calculated)*/
	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_decl_variant(Context *cnt, MirInstrDeclVariant *variant_instr)
{
	MirVariant *variant = variant_instr->variant;
	assert(variant && "Missing variant.");

	if (variant_instr->value) {
		/* User defined initialization value. */
		if (!variant_instr->value->comptime) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_INVALID_EXPR,
			            variant_instr->value->node->location,
			            BUILDER_CUR_WORD,
			            "Enum variant value must be compile time known.");
			return ANALYZE_FAILED;
		}

		variant_instr->value =
		    analyze_slot_input(cnt, NULL, variant_instr->value, NULL, false);

		/* Setup value. */
		variant_instr->variant->value = &variant_instr->value->value;
	} else {
		/*
		 * CLENUP: Automatic initialization value is set in parser, mabye we will prefer to
		 * do automatic initialization here instead of doing so in parser pass.
		 */
		abort();
	}

	commit_variant(cnt, variant);

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_type_struct(Context *cnt, MirInstrTypeStruct *type_struct)
{
	SmallArray_Member *members = NULL;

	if (type_struct->members) {
		MirInstr **         member_instr;
		MirInstrDeclMember *decl_member;
		MirType *           member_type;
		Scope *             scope = type_struct->scope;
		const size_t        memc  = type_struct->members->size;

		members = create_sarr(SmallArray_Member, cnt->assembly);

		for (size_t i = 0; i < memc; ++i) {
			member_instr = &type_struct->members->data[i];

			(*member_instr) = analyze_slot_input(cnt, NULL, *member_instr, NULL, false);

			decl_member = (MirInstrDeclMember *)*member_instr;
			assert(decl_member->base.kind == MIR_INSTR_DECL_MEMBER);
			assert(decl_member->base.comptime);

			/* solve member type */
			member_type = decl_member->type->value.data.v_ptr.data.type;

			if (member_type->kind == MIR_TYPE_FN) {
				builder_msg(cnt->builder,
				            BUILDER_MSG_ERROR,
				            ERR_INVALID_TYPE,
				            (*member_instr)->node->location,
				            BUILDER_CUR_WORD,
				            "Invalid type of the structure member, functions can "
				            "be referenced only by pointers.");
				return ANALYZE_FAILED;
			}

			assert(member_type);

			/* setup and provide member */
			MirMember *member = decl_member->member;
			assert(member);
			member->type       = member_type;
			member->decl_scope = scope;
			member->index      = i;

			sa_push_Member(members, member);

			commit_member(cnt, member);
		}
	}

	{ /* Setup const pointer. */
		MirConstPtr *const_ptr = &type_struct->base.value.data.v_ptr;
		MirType *    tmp       = create_type_struct(cnt,
                                                  MIR_TYPE_STRUCT,
                                                  type_struct->id,
                                                  type_struct->scope,
                                                  members,
                                                  type_struct->is_packed);

		set_const_ptr(const_ptr, tmp, MIR_CP_TYPE);
	}
	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_type_slice(Context *cnt, MirInstrTypeSlice *type_slice)
{
	assert(type_slice->elem_type);
	type_slice->elem_type = analyze_slot_input(cnt, NULL, type_slice->elem_type, NULL, false);

	ID *id = NULL;
	if (type_slice->base.node && type_slice->base.node->kind == AST_IDENT) {
		id = &type_slice->base.node->data.ident.id;
	}

	if (type_slice->elem_type->value.type->kind != MIR_TYPE_TYPE) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            type_slice->elem_type->node->location,
		            BUILDER_CUR_WORD,
		            "Expected type.");
		return ANALYZE_FAILED;
	}

	assert(type_slice->elem_type->comptime && "This should be an error");
	MirType *elem_type = type_slice->elem_type->value.data.v_ptr.data.type;
	assert(elem_type);

	elem_type = create_type_ptr(cnt, elem_type);
	elem_type = create_type_struct_special(cnt, MIR_TYPE_SLICE, id, elem_type);

	{ /* set const pointer value */
		MirConstPtr *const_ptr = &type_slice->base.value.data.v_ptr;
		set_const_ptr(const_ptr, elem_type, MIR_CP_TYPE);
	}

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_type_vargs(Context *cnt, MirInstrTypeVArgs *type_vargs)
{
	MirType *elem_type = NULL;
	if (type_vargs->elem_type) {
		type_vargs->elem_type =
		    analyze_slot_input(cnt, NULL, type_vargs->elem_type, NULL, false);

		if (type_vargs->elem_type->value.type->kind != MIR_TYPE_TYPE) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_INVALID_TYPE,
			            type_vargs->elem_type->node->location,
			            BUILDER_CUR_WORD,
			            "Expected type.");
			return ANALYZE_FAILED;
		}

		assert(type_vargs->elem_type->comptime && "This should be an error");
		elem_type = type_vargs->elem_type->value.data.v_ptr.data.type;
	} else {
		/* use Any */
		elem_type = lookup_builtin(cnt, MIR_BUILTIN_ID_ANY);
		if (!elem_type) return builtin_ids[MIR_BUILTIN_ID_ANY].hash;
	}

	assert(elem_type);

	elem_type = create_type_ptr(cnt, elem_type);
	elem_type = create_type_struct_special(cnt, MIR_TYPE_VARGS, NULL, elem_type);

	{ /* set const pointer value */
		MirConstPtr *const_ptr = &type_vargs->base.value.data.v_ptr;
		set_const_ptr(const_ptr, elem_type, MIR_CP_TYPE);
	}

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_type_array(Context *cnt, MirInstrTypeArray *type_arr)
{
	assert(type_arr->base.value.type);
	assert(type_arr->elem_type->analyzed);

	bool is_valid;
	type_arr->len = analyze_slot_input(
	    cnt, &is_valid, type_arr->len, cnt->builtin_types.entry_usize, false);
	type_arr->elem_type = analyze_slot_input(cnt, &is_valid, type_arr->elem_type, NULL, false);
	if (!is_valid) return ANALYZE_FAILED;

	/* len */
	if (!type_arr->len->comptime) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_EXPECTED_CONST,
		            type_arr->len->node->location,
		            BUILDER_CUR_WORD,
		            "Array size must be compile-time constant.");
		return ANALYZE_FAILED;
	}

	if (type_arr->elem_type->value.type->kind != MIR_TYPE_TYPE) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            type_arr->elem_type->node->location,
		            BUILDER_CUR_WORD,
		            "Expected type.");
		return ANALYZE_FAILED;
	}

	assert(type_arr->len->comptime && "this must be error");
	reduce_instr(cnt, type_arr->len);

	const size_t len = type_arr->len->value.data.v_u64;
	if (len == 0) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_ARR_SIZE,
		            type_arr->len->node->location,
		            BUILDER_CUR_WORD,
		            "Array size cannot be 0.");
		return ANALYZE_FAILED;
	}

	/* elem type */
	assert(type_arr->elem_type->comptime);
	reduce_instr(cnt, type_arr->elem_type);

	MirType *elem_type = type_arr->elem_type->value.data.v_ptr.data.type;
	assert(elem_type);

	elem_type = create_type_array(cnt, elem_type, len);

	{ /* set const pointer value */
		MirConstPtr *const_ptr = &type_arr->base.value.data.v_ptr;
		set_const_ptr(const_ptr, elem_type, MIR_CP_TYPE);
	}

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_type_enum(Context *cnt, MirInstrTypeEnum *type_enum)
{
	SmallArray_Instr *variant_instrs = type_enum->variants;
	Scope *           scope          = type_enum->scope;
	assert(variant_instrs);
	assert(scope);
	const size_t varc = variant_instrs->size;
	assert(varc);

	/*
	 * Validate and settup enum base type.
	 */
	MirType *base_type;
	if (type_enum->base_type) {
		reduce_instr(cnt, type_enum->base_type);
		base_type = type_enum->base_type->value.data.v_ptr.data.type;

		/* Enum type must be integer! */
		if (base_type->kind != MIR_TYPE_INT) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_INVALID_TYPE,
			            type_enum->base_type->node->location,
			            BUILDER_CUR_WORD,
			            "Base type of enumerator must be an integer type.");
			return ANALYZE_FAILED;
		}
	} else {
		/* Use s32 by default. */
		base_type = cnt->builtin_types.entry_s32;
	}

	assert(base_type && "Invalid enum base type.");

	SmallArray_Variant *variants = create_sarr(SmallArray_Variant, cnt->assembly);

	/* Iterate over all enum variants and validate them. */
	MirInstr *  it;
	MirVariant *variant;

	sarray_foreach(variant_instrs, it)
	{
		MirInstrDeclVariant *variant_instr = (MirInstrDeclVariant *)it;
		variant                            = variant_instr->variant;
		assert(variant && "Missing variant.");

		bool is_valid = true;
		analyze_slot_input(cnt, &is_valid, variant_instr->value, base_type, false);
		if (!is_valid) return ANALYZE_FAILED;

		reduce_instr(cnt, &variant_instr->base);

		sa_push_Variant(variants, variant);
	}

	MirType *enum_type = create_type_enum(cnt, type_enum->id, scope, base_type, variants);

	{ /* set const pointer value */
		MirConstPtr *const_ptr = &type_enum->base.value.data.v_ptr;
		set_const_ptr(const_ptr, enum_type, MIR_CP_TYPE);
	}

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_type_ptr(Context *cnt, MirInstrTypePtr *type_ptr)
{
	assert(type_ptr->type);

	type_ptr->type = analyze_slot_input(cnt, NULL, type_ptr->type, NULL, false);
	assert(type_ptr->type->comptime);

	{ /* Target value must be a type. */
		MirType *src_type = type_ptr->type->value.type;
		assert(src_type);

		if (src_type->kind != MIR_TYPE_TYPE) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_INVALID_TYPE,
			            type_ptr->type->node->location,
			            BUILDER_CUR_WORD,
			            "Expected type name.");
			return ANALYZE_FAILED;
		}
	}

	MirType *src_type_value = type_ptr->type->value.data.v_ptr.data.type;
	assert(src_type_value);

	if (src_type_value->kind == MIR_TYPE_TYPE) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            type_ptr->base.node->location,
		            BUILDER_CUR_WORD,
		            "Cannot create pointer to type.");
		return ANALYZE_FAILED;
	}

	MirType *tmp = create_type_ptr(cnt, src_type_value);

	{ /* set const pointer value */
		MirConstPtr *const_ptr = &type_ptr->base.value.data.v_ptr;
		set_const_ptr(const_ptr, tmp, MIR_CP_TYPE);
	}

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_binop(Context *cnt, MirInstrBinop *binop)
{
#define is_valid(_type, _op)                                                                       \
	(((_type)->kind == MIR_TYPE_INT) || ((_type)->kind == MIR_TYPE_NULL) ||                    \
	 ((_type)->kind == MIR_TYPE_REAL) || ((_type)->kind == MIR_TYPE_PTR) ||                    \
	 ((_type)->kind == MIR_TYPE_BOOL && ast_binop_is_logic(_op)) ||                            \
	 ((_type)->kind == MIR_TYPE_ENUM && (_op == BINOP_EQ || _op == BINOP_NEQ)))

	const bool lhs_is_null = binop->lhs->value.type->kind == MIR_TYPE_NULL;

	bool is_valid;
	binop->lhs = analyze_slot_input(cnt, &is_valid, binop->lhs, NULL, false);
	binop->rhs = analyze_slot_input(
	    cnt, &is_valid, binop->rhs, lhs_is_null ? NULL : binop->lhs->value.type, false);

	if (!is_valid) return ANALYZE_FAILED;

	/*
	 * This is special case when lhs is null constant; in such case base type of this null must
	 * corespond with rhs type (due to LLVM IR null type policy).
	 */
	if (lhs_is_null && !setup_instr_const_null(cnt, binop->lhs, binop->rhs->value.type)) {
		return ANALYZE_FAILED;
	}

	MirInstr *lhs = binop->lhs;
	MirInstr *rhs = binop->rhs;
	assert(lhs && rhs);
	assert(lhs->analyzed);
	assert(rhs->analyzed);

	const bool lhs_valid = is_valid(lhs->value.type, binop->op);
	const bool rhs_valid = is_valid(rhs->value.type, binop->op);

	if (!(lhs_valid && rhs_valid)) {
		error_types(cnt,
		            lhs->value.type,
		            rhs->value.type,
		            binop->base.node,
		            "invalid operation for %s type");
		return ANALYZE_FAILED;
	}

	MirType *type =
	    ast_binop_is_logic(binop->op) ? cnt->builtin_types.entry_bool : lhs->value.type;
	assert(type);
	binop->base.value.type = type;

	/* when binary operation has lhs and rhs values known in compile it is known
	 * in compile time also
	 */
	if (lhs->comptime && rhs->comptime) binop->base.comptime = true;

	return ANALYZE_PASSED;
#undef is_valid
}

uint64_t
analyze_instr_unop(Context *cnt, MirInstrUnop *unop)
{
	unop->expr = analyze_slot_input(cnt, NULL, unop->expr, NULL, false);

	assert(unop->expr && unop->expr->analyzed);
	MirType *type = unop->expr->value.type;
	assert(type);
	unop->base.value.type = type;

	unop->base.comptime = unop->expr->comptime;

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_const(Context *cnt, MirInstrConst *cnst)
{
	assert(cnst->base.value.type);
	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_ret(Context *cnt, MirInstrRet *ret)
{
	/* compare return value with current function type */
	MirInstrBlock *block = ret->base.owner_block;
	if (!block->terminal) block->terminal = &ret->base;

	MirType *fn_type = get_current_fn(cnt)->type;
	assert(fn_type);
	assert(fn_type->kind == MIR_TYPE_FN);

	if (ret->value) {
		bool is_valid;
		ret->value = analyze_slot_input(
		    cnt,
		    &is_valid,
		    ret->value,
		    ret->allow_fn_ret_type_override ? NULL : fn_type->data.fn.ret_type,
		    false);

		if (!is_valid) return ANALYZE_FAILED;
	}

	MirInstr *value = ret->value;
	if (value) {
		assert(value->analyzed);
	}

	if (ret->allow_fn_ret_type_override) {
		/* return is supposed to override function return type */
		if (ret->value) {
			assert(ret->value->value.type);
			if (fn_type->data.fn.ret_type != ret->value->value.type) {
				MirFn *fn = get_current_fn(cnt);
				assert(fn);
				fn->type = create_type_fn(cnt,
				                          ret->value->value.type,
				                          fn_type->data.fn.arg_types,
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
	    !type_cmp(fn_type->data.fn.ret_type, cnt->builtin_types.entry_void);

	/* return value is not expected, and it's not provided */
	if (!expected_ret_value && !value) {
		return ANALYZE_PASSED;
	}

	/* return value is expected, but it's not provided */
	if (expected_ret_value && !value) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_EXPR,
		            ret->base.node->location,
		            BUILDER_CUR_AFTER,
		            "Expected return value.");
		return ANALYZE_FAILED;
	}

	/* return value is not expected, but it's provided */
	if (!expected_ret_value && value) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_EXPR,
		            ret->value->node->location,
		            BUILDER_CUR_WORD,
		            "Unexpected return value.");
		return ANALYZE_FAILED;
	}

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_decl_var(Context *cnt, MirInstrDeclVar *decl)
{
	MirVar *var = decl->var;
	assert(var);

	if (decl->type && var->value.type == NULL) {
		assert(decl->type->kind == MIR_INSTR_CALL && "expected type resolver call");
		if (analyze_instr(cnt, decl->type) != ANALYZE_PASSED) return ANALYZE_POSTPONE;
		MirConstValue *resolved_type_value =
		    exec_call_top_lvl(cnt, (MirInstrCall *)decl->type);
		// unref_instr(decl->type);
		assert(resolved_type_value && resolved_type_value->type->kind == MIR_TYPE_TYPE);
		MirType *resolved_type = resolved_type_value->data.v_ptr.data.type;
		if (!resolved_type) return ANALYZE_FAILED;

		var->value.type = resolved_type;
	}

	if (decl->init) {
		if (decl->init->kind == MIR_INSTR_CALL && decl->init->comptime) {
			if (analyze_instr(cnt, decl->init) != ANALYZE_PASSED)
				return ANALYZE_POSTPONE;
			exec_call_top_lvl(cnt, (MirInstrCall *)decl->init);
		}

		/* validate types or infer */
		if (var->value.type) {
			bool is_valid;
			decl->init =
			    analyze_slot_input(cnt, &is_valid, decl->init, var->value.type, false);
			if (!is_valid) return ANALYZE_FAILED;
		} else {
			decl->init = analyze_slot_input(cnt, NULL, decl->init, NULL, false);
			/* infer type */
			MirType *type = decl->init->value.type;
			assert(type);
			var->value.type = type;
		}

		decl->base.comptime = var->comptime = !var->is_mutable && decl->init->comptime;
	} else if (var->is_in_gscope) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_UNINITIALIZED,
		            decl->base.node->location,
		            BUILDER_CUR_WORD,
		            "All globals must be initialized to compile time known value.");
		return ANALYZE_FAILED;
	}

	if (!var->value.type) {
		bl_abort("unknown declaration type");
	}

	if (var->value.type->kind == MIR_TYPE_TYPE && var->is_mutable) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_MUTABILITY,
		            decl->base.node->location,
		            BUILDER_CUR_WORD,
		            "Type declaration must be immutable.");
		return ANALYZE_FAILED;
	}

	if (var->value.type->kind == MIR_TYPE_FN) {
		/* Allocated type is function. */
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            decl->base.node->location,
		            BUILDER_CUR_WORD,
		            "Invalid type of the variable, functions can be referenced "
		            "only by pointers.");
		return ANALYZE_FAILED;
	} else if (var->value.type->kind == MIR_TYPE_VOID) {
		/* Allocated type is void type. */
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_TYPE,
		            decl->base.node->location,
		            BUILDER_CUR_WORD,
		            "Cannot allocate unsized type.");
		return ANALYZE_FAILED;
	}

	if (decl->base.ref_count == 0) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_WARNING,
		            0,
		            decl->base.node->location,
		            BUILDER_CUR_WORD,
		            "Unused declaration.");
	}

	reduce_instr(cnt, decl->init);

	if (decl->base.comptime && decl->init) {
		/* initialize when known in compiletime */
		var->value = decl->init->value;
	}

	if (!decl->var->is_implicit) commit_var(cnt, decl->var);

	/* Type declaration should not be generated in LLVM. */
	var->gen_llvm = var->value.type->kind != MIR_TYPE_TYPE;

	if (var->is_in_gscope) {
		/* Global varibales which are not compile time constants are allocated
		 * on the stack, one option is to do allocation every time when we
		 * invoke comptime function execution, but we don't know which globals
		 * will be used by function and we also don't known whatever function
		 * has some side effect or not. So we produce allocation here. Variable
		 * will be stored in static data segment. There is no need to use
		 * relative pointers here. */
		if (!var->comptime) {
			/* Allocate memory in static block. */
			exec_stack_alloc_var(cnt, var);
			/* Initialize data. */
			exec_instr_decl_var(cnt, decl);
		}
	}

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_call(Context *cnt, MirInstrCall *call)
{
	assert(call->callee);

	/*
	 * Direct call is call without any reference lookup, usually call to anonymous function,
	 * type resolver or variable initializer. Contant value of callee instruction must containt
	 * pointer to the MirFn object.
	 */
	const MirInstrKind callee_kind = call->callee->kind;
	const bool         is_direct_call =
	    callee_kind != MIR_INSTR_DECL_REF && callee_kind != MIR_INSTR_MEMBER_PTR;

	/* callee has not been analyzed yet -> postpone call analyze */
	if (!call->callee->analyzed) return ANALYZE_POSTPONE;

	call->callee = analyze_slot_input(cnt, NULL, call->callee, NULL, false);

	MirType *type = call->callee->value.type;
	assert(type && "invalid type of called object");

	if (mir_is_pointer_type(type)) {
		/* we want to make calls also via pointer to functions so in such case
		 * we need to resolve pointed function */
		type = mir_deref_type(type);
	}

	if (type->kind != MIR_TYPE_FN) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_EXPECTED_FUNC,
		            call->callee->node->location,
		            BUILDER_CUR_WORD,
		            "Expected a function name.");
		return ANALYZE_FAILED;
	}

	if (is_direct_call) {
		MirFn *fn = call->callee->value.data.v_ptr.data.fn;
		assert(fn && "Missing function reference for direct call!");
		if (call->base.comptime) {
			if (!fn->analyzed_for_cmptime_exec) return ANALYZE_POSTPONE;
		} else if (call->callee->kind == MIR_INSTR_FN_PROTO) {
			/* Direct call of anonymous function. */

			/*
			 * CLENUP: Function reference counting is not clear, we can decide to count
			 * references direcly inside MirFn or in function prototype instruction.
			 */
			// ++fn->ref_count;
			fn->emit_llvm = true;
		}
	}

	MirType *result_type = type->data.fn.ret_type;
	assert(result_type && "invalid type of call result");
	call->base.value.type = result_type;

	/* validate arguments */
	const bool is_vargs = type->data.fn.is_vargs;

	size_t       callee_argc = type->data.fn.arg_types ? type->data.fn.arg_types->size : 0;
	const size_t call_argc   = call->args ? call->args->size : 0;

	if (is_vargs) {
		/* This is gonna be tricky... */
		--callee_argc;
		if ((call_argc < callee_argc)) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_INVALID_ARG_COUNT,
			            call->base.node->location,
			            BUILDER_CUR_WORD,
			            "Expected at least %u %s, but called with %u.",
			            callee_argc,
			            callee_argc == 1 ? "argument" : "arguments",
			            call_argc);
			return ANALYZE_FAILED;
		}

		MirType *vargs_type = mir_get_fn_arg_type(type, (uint32_t)callee_argc);
		assert(vargs_type->kind == MIR_TYPE_VARGS && "VArgs is expected to be last!!!");

		vargs_type = mir_get_struct_elem_type(vargs_type, 1);
		assert(vargs_type && mir_is_pointer_type(vargs_type));

		vargs_type = mir_deref_type(vargs_type);

		/* Prepare vargs values. */
		const size_t      vargsc = call_argc - callee_argc;
		SmallArray_Instr *values = create_sarr(SmallArray_Instr, cnt->assembly);
		MirInstr *        vargs  = create_instr_vargs_impl(cnt, vargs_type, values);
		ref_instr(vargs);

		if (vargsc > 0) {
			/* One or more vargs passed. */
			// TODO: check it this is ok!!!
			// TODO: check it this is ok!!!
			// TODO: check it this is ok!!!
			// TODO: check it this is ok!!!
			for (size_t i = 0; i < vargsc; ++i) {
				sa_push_Instr(values, call->args->data[callee_argc + i]);
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

		if (!analyze_instr_vargs(cnt, (MirInstrVArgs *)vargs)) return ANALYZE_FAILED;
		vargs->analyzed = true;

		/* Erase vargs from arguments. */
		sa_resize_Instr(call->args, callee_argc);

		/* Replace last with vargs. */
		sa_push_Instr(call->args, vargs);
	} else {
		if ((callee_argc != call_argc)) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_INVALID_ARG_COUNT,
			            call->base.node->location,
			            BUILDER_CUR_WORD,
			            "Expected %u %s, but called with %u.",
			            callee_argc,
			            callee_argc == 1 ? "argument" : "arguments",
			            call_argc);
			return ANALYZE_FAILED;
		}
	}

	/* validate argument types */
	if (callee_argc) {
		MirInstr **call_arg;
		MirType *  callee_arg_type;
		bool       valid = true;

		for (uint32_t i = 0; i < callee_argc && valid; ++i) {
			call_arg        = &call->args->data[i];
			callee_arg_type = mir_get_fn_arg_type(type, i);

			(*call_arg) =
			    analyze_slot_input(cnt, &valid, *call_arg, callee_arg_type, true);
		}
	}

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_store(Context *cnt, MirInstrStore *store)
{
	MirInstr *dest = store->dest;
	assert(dest);
	assert(dest->analyzed);

	if (!mir_is_pointer_type(dest->value.type)) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_EXPR,
		            store->base.node->location,
		            BUILDER_CUR_WORD,
		            "Left hand side of the expression cannot be assigned.");
		return ANALYZE_FAILED;
	}

	if (dest->value.addr_mode == MIR_VAM_LVALUE_CONST) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_INVALID_EXPR,
		            store->base.node->location,
		            BUILDER_CUR_WORD,
		            "Cannot assign to constant.");
	}

	MirType *dest_type = mir_deref_type(dest->value.type);
	assert(dest_type && "store destination has invalid base type");

	bool is_valid;
	store->src = analyze_slot_input(cnt, &is_valid, store->src, dest_type, false);
	if (!is_valid) return ANALYZE_FAILED;

	reduce_instr(cnt, store->dest);

	return ANALYZE_PASSED;
}

uint64_t
analyze_instr_block(Context *cnt, MirInstrBlock *block)
{
	assert(block);

	MirFn *fn = block->owner_fn;
	assert(fn);
	MirInstrFnProto *fn_proto = (MirInstrFnProto *)fn->prototype;

	/* append implicit return for void functions or generate error when last
	 * block is not terminated
	 */
	if (!is_block_terminated(block)) {
		if (fn->type->data.fn.ret_type->kind == MIR_TYPE_VOID) {
			set_current_block(cnt, block);
			append_instr_ret(cnt, NULL, NULL, false);
		} else {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
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

			builder_msg(cnt->builder,
			            BUILDER_MSG_WARNING,
			            0,
			            fn_proto->first_unrechable_location,
			            BUILDER_CUR_NONE,
			            "Unrechable code detected.");
		}
	}

	return ANALYZE_PASSED;
}

MirInstr *
analyze_slot_input(Context * cnt,
                   bool *    out_valid,
                   MirInstr *input,
                   MirType * slot_type /* optional */,
                   bool      enable_special_cast)
{
	/*
	 * Perform analyze of input instruction passed to input slot of other instruction.
	 * 1) Generate Load if needed.
	 * 2) Generate cast when casting is enabled and slot_type is not NULL.
	 * 3) Generate error if input and slot_type does not match and cannot be implicitly casted.
	 * 4) Reduce instruction.
	 */

	assert(input);
	MirType *input_type = input->value.type;

	if (out_valid) *out_valid = true;

	if (!slot_type) {
		/* slot type not specified, insert only load if needed */
		if (is_load_needed(input)) input = insert_instr_load(cnt, input);
		goto VALID;
	}

	/* TODO: setup nulls? */
	if (input_type->kind == MIR_TYPE_NULL) {
		if (!setup_instr_const_null(cnt, input, slot_type)) goto INVALID_NO_MSG;

		goto VALID;
	}

	if (enable_special_cast) {
		/* check any */
		if (is_to_any_needed(cnt, input, slot_type)) {
			input = insert_instr_toany(cnt, input);
			input = insert_instr_load(cnt, input);
			goto VALID;
		}

		/* TODO: check array to slice */
	}

	/* regular implicit cast */
	if (is_load_needed(input)) input = insert_instr_load(cnt, input);
	input_type = input->value.type;

	/* both types are same -> no cast is needed */
	if (type_cmp(input_type, slot_type)) {
		goto VALID;
	}

	const bool impl_cast = input_type->kind == MIR_TYPE_INT && slot_type->kind == MIR_TYPE_INT;
	if (impl_cast) {
		if (input->kind == MIR_INSTR_CONST) {
			/* constant numeric literal */
			input->value.type = slot_type;
			goto VALID;
		}

		input = insert_instr_cast(cnt, input, slot_type);
		goto VALID;
	}

	error_types(cnt, input->value.type, slot_type, input->node, NULL);

INVALID_NO_MSG:
	if (out_valid) *out_valid = false;
	return input;

VALID:
	reduce_instr(cnt, input);
	return input;
}

uint64_t
analyze_instr(Context *cnt, MirInstr *instr)
{
	if (!instr) return ANALYZE_PASSED;

	/* skip already analyzed instructions */
	if (instr->analyzed) return ANALYZE_PASSED;
	uint64_t state = ANALYZE_PASSED;

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
		state = analyze_instr_addrof(cnt, (MirInstrAddrOf *)instr);
		break;
	case MIR_INSTR_CAST:
		state = analyze_instr_cast(cnt, (MirInstrCast *)instr);
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
	}

	instr->analyzed = state == ANALYZE_PASSED;

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
			owner_block->owner_fn->analyzed_for_cmptime_exec = true;
#if BL_DEBUG && VERBOSE_ANALYZE
			printf("Analyze: " BLUE("Function '%s' completely analyzed.\n"),
			       owner_block->owner_fn->llvm_name);
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
	if (cnt->analyze.verbose_pre) {
		MirInstr *instr;
		BArray *  globals = cnt->assembly->MIR.global_instrs;
		barray_foreach(globals, instr)
		{
			mir_print_instr(instr, stdout);
		}
	}

	/* PERFORMANCE: use array??? */
	/* PERFORMANCE: use array??? */
	/* PERFORMANCE: use array??? */
	BList *   q = cnt->analyze.queue;
	uint64_t  state;
	size_t    postpone_loop_count = 0;
	MirInstr *ip                  = NULL;
	MirInstr *prev_ip             = NULL;
	bool      skip                = false;

	if (bo_list_empty(q)) return;

	while (true) {
		prev_ip = ip;
		ip      = skip ? NULL : analyze_try_get_next(ip);

		if (prev_ip && prev_ip->analyzed) {
			erase_instr_tree(prev_ip);
		}

		if (!ip) {
			if (bo_list_empty(q)) break;

			ip = bo_list_front(q, MirInstr *);
			bo_list_pop_front(q);
			skip = false;
		}

		state = analyze_instr(cnt, ip);
		switch (state) {
		case ANALYZE_PASSED:
#if BL_DEBUG && VERBOSE_ANALYZE
			printf("Analyze: [ " GREEN("PASSED") " ] %16s\n", mir_instr_name(ip));
#endif
			postpone_loop_count = 0;
			break;

		case ANALYZE_FAILED:
#if BL_DEBUG && VERBOSE_ANALYZE
			printf("Analyze: [ " RED("FAILED") " ] %16s\n", mir_instr_name(ip));
#endif
			skip                = true;
			postpone_loop_count = 0;
			break;

		case ANALYZE_POSTPONE:
#if BL_DEBUG && VERBOSE_ANALYZE
			printf("Analyze: [" MAGENTA("POSTPONE") "] %16s\n", mir_instr_name(ip));
#endif

			skip = true;
			if (postpone_loop_count++ < bo_list_size(q)) bo_list_push_back(q, ip);
			break;

		default: {
#if BL_DEBUG && VERBOSE_ANALYZE
			printf("Analyze: [  " YELLOW("WAIT") "  ] %16s is waiting for: '%llu'\n",
			       mir_instr_name(ip),
			       (unsigned long long)state);
#endif

			BArray *      wq   = NULL;
			bo_iterator_t iter = bo_htbl_find(cnt->analyze.waiting, state);
			bo_iterator_t end  = bo_htbl_end(cnt->analyze.waiting);
			if (bo_iterator_equal(&iter, &end)) {
				wq = bo_array_new(sizeof(MirInstr *));
				bo_array_reserve(wq, 16);
				bo_htbl_insert(cnt->analyze.waiting, state, wq);
			} else {
				wq = bo_htbl_iter_peek_value(cnt->analyze.waiting, &iter, BArray *);
			}

			assert(wq);
			bo_array_push_back(wq, ip);
			skip                = true;
			postpone_loop_count = 0;
		}
		}
	}

	if (cnt->analyze.verbose_post) {
		MirInstr *instr;
		BArray *  globals = cnt->assembly->MIR.global_instrs;
		barray_foreach(globals, instr)
		{
			mir_print_instr(instr, stdout);
		}
	}
}

void
analyze_report_unresolved(Context *cnt)
{
	MirInstr *    instr;
	BArray *      wq;
	bo_iterator_t iter;

	bhtbl_foreach(cnt->analyze.waiting, iter)
	{
		wq = bo_htbl_iter_peek_value(cnt->analyze.waiting, &iter, BArray *);
		assert(wq);
		barray_foreach(wq, instr)
		{
			assert(instr);

			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_UNKNOWN_SYMBOL,
			            instr->node->location,
			            BUILDER_CUR_WORD,
			            "Unknown symbol.");
		}
	}
}

/* executing */
MirStack *
exec_new_stack(size_t size)
{
	if (size == 0) bl_abort("invalid frame stack size");

	MirStack *stack = bl_malloc(sizeof(char) * size);
	if (!stack) bl_abort("bad alloc");
#if BL_DEBUG
	memset(stack, 0, size);
#endif

	stack->allocated_bytes = size;
	exec_reset_stack(stack);
	return stack;
}

void
exec_delete_stack(MirStack *stack)
{
	bl_free(stack);
}

void
exec_reset_stack(MirStack *stack)
{
	stack->pc         = NULL;
	stack->ra         = NULL;
	stack->prev_block = NULL;
	stack->aborted    = false;
	const size_t size = exec_stack_alloc_size(sizeof(MirStack));
	stack->used_bytes = size;
	stack->top_ptr    = (uint8_t *)stack + size;
}

void
exec_print_call_stack(Context *cnt, size_t max_nesting)
{
	MirInstr *instr = cnt->exec.stack->pc;
	MirFrame *fr    = cnt->exec.stack->ra;
	size_t    n     = 0;

	if (!instr) return;
	/* print last instruction */
	builder_msg(cnt->builder, BUILDER_MSG_LOG, 0, instr->node->location, BUILDER_CUR_WORD, "");

	while (fr) {
		instr = fr->callee;
		fr    = fr->prev;
		if (!instr) break;

		if (max_nesting && n == max_nesting) {
			msg_note("continue...");
			break;
		}

		builder_msg(
		    cnt->builder, BUILDER_MSG_LOG, 0, instr->node->location, BUILDER_CUR_WORD, "");
		++n;
	}
}

/*
 * Produce decomposition of compile time known value to the stack location.
 */
void
exec_copy_comptime_to_stack(Context *cnt, MirStackPtr dest_ptr, MirConstValue *src_value)
{
	/* This may cause recursive calls for aggregate data types. */
	assert(dest_ptr && src_value);
	MirConstValueData *data     = &src_value->data;
	MirType *          src_type = src_value->type;
	assert(src_type);

	switch (src_type->kind) {
	case MIR_TYPE_SLICE:
	case MIR_TYPE_STRING:
	case MIR_TYPE_VARGS:
	case MIR_TYPE_STRUCT: {
		if (src_value->data.v_struct.is_zero_initializer) {
			memset(dest_ptr, 0, src_type->store_size_bytes);
		} else {
			SmallArray_ConstValue *members = data->v_struct.members;
			MirConstValue *        member;

			assert(members);
			const size_t memc = members->size;
			for (uint32_t i = 0; i < memc; ++i) {
				member = members->data[i];

				/* copy all members to variable allocated memory on the stack */
				MirStackPtr elem_dest_ptr =
				    dest_ptr + get_struct_elem_offest(cnt, src_type, i);
				assert(elem_dest_ptr);
				exec_copy_comptime_to_stack(cnt, elem_dest_ptr, member);
			}
		}
		break;
	}

	case MIR_TYPE_ARRAY: {
		if (src_value->data.v_array.is_zero_initializer) {
			memset(dest_ptr, 0, src_type->store_size_bytes);
		} else {
			SmallArray_ConstValue *elems = data->v_array.elems;
			MirConstValue *        elem;

			assert(elems);
			const size_t memc = elems->size;
			for (uint32_t i = 0; i < memc; ++i) {
				elem = elems->data[i];

				/* copy all elems to variable allocated memory on the stack */
				MirStackPtr elem_dest_ptr =
				    dest_ptr + get_array_elem_offset(src_type, i);
				exec_copy_comptime_to_stack(cnt, elem_dest_ptr, elem);
			}
		}

		break;
	}

	case MIR_TYPE_PTR: {
		MirConstPtr *const_ptr = &src_value->data.v_ptr;
		switch (const_ptr->kind) {

		case MIR_CP_VAR: {
			MirVar *var = const_ptr->data.var;
			assert(var);

			MirStackPtr var_ptr =
			    exec_read_stack_ptr(cnt, var->rel_stack_ptr, var->is_in_gscope);
			memcpy(dest_ptr, &var_ptr, src_type->store_size_bytes);
			break;
		}

		default: {
			memcpy(dest_ptr, (MirStackPtr)src_value, src_type->store_size_bytes);
		}
		}

		break;
	}

	default:
		assert(dest_ptr && "Invalid destination pointer");
		assert(src_value && "Invalid source value pointer");
		memcpy(dest_ptr, (MirStackPtr)src_value, src_type->store_size_bytes);
	}
}

static inline MirType *
_get_RTTI_type(Context *cnt, MirTypeKind kind)
{
	MirType *tmp;
	switch (kind) {
	case MIR_TYPE_TYPE:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_TYPE);
		break;
	case MIR_TYPE_VOID:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_VOID);
		break;
	case MIR_TYPE_INT:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_INT);
		break;
	case MIR_TYPE_REAL:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_REAL);
		break;
	case MIR_TYPE_FN:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_FN);
		break;
	case MIR_TYPE_PTR:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_PTR);
		break;
	case MIR_TYPE_BOOL:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_BOOL);
		break;
	case MIR_TYPE_ARRAY:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_ARRAY);
		break;
	case MIR_TYPE_STRUCT:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_STRUCT);
		break;
	case MIR_TYPE_ENUM:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_ENUM);
		break;
	case MIR_TYPE_NULL:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_NULL);
		break;
	case MIR_TYPE_STRING:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_STRING);
		break;
	case MIR_TYPE_SLICE:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_SLICE);
		break;
	case MIR_TYPE_VARGS:
		tmp = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO_VARGS);
		break;
	default:
		bl_abort("Missing type info user type.");
	}

	assert(tmp);
	return tmp;
}

static inline MirVar *
_create_and_alloc_RTTI_var(Context *cnt, MirType *type)
{
	assert(type);
	const char *name = gen_uq_name(cnt, IMPL_RTTI_ENTRY);
	MirVar *    var  = create_var_impl(cnt, name, type, false, true, false);

	/* allocate */
	exec_stack_alloc_var(cnt, var);

	return var;
}

/*
 * Push RTTI variable to the array of RTTIs, do stack allocation for execution and push current
 * value on the stack.
 */
static inline void
_push_RTTI_var(Context *cnt, MirVar *var)
{
	/* Push into RTTI table */
	bo_array_push_back(cnt->assembly->MIR.RTTI_tmp_vars, var);
	MirStackPtr var_ptr = exec_read_stack_ptr(cnt, var->rel_stack_ptr, true);
	assert(var_ptr);

	exec_copy_comptime_to_stack(cnt, var_ptr, &var->value);
}

MirVar *
exec_gen_type_RTTI(Context *cnt, MirType *type)
{
	assert(type);
	if (type->rtti.var) return type->rtti.var;

	MirType *rtti_type = _get_RTTI_type(cnt, type->kind);
	assert(rtti_type);

	MirVar *       rtti_var   = _create_and_alloc_RTTI_var(cnt, rtti_type);
	MirConstValue *rtti_value = &rtti_var->value;

	/* set base TypeInfo data */
	SmallArray_ConstValue *members = create_sarr(SmallArray_ConstValue, cnt->assembly);

	MirConstValue *tmp;
	{ /* Build TypeInfo entry members. */
		/* .kind */
		SmallArray_ConstValue *type_info_members =
		    create_sarr(SmallArray_ConstValue, cnt->assembly);
		MirType *tmp_type = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO);
		assert(tmp_type);
		tmp                        = create_const_value(cnt, tmp_type);
		tmp->data.v_struct.members = type_info_members;
		sa_push_ConstValue(members, tmp);

		tmp_type = lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_KIND);
		assert(tmp_type);

		tmp             = create_const_value(cnt, tmp_type);
		tmp->data.v_s32 = type->kind;
		sa_push_ConstValue(type_info_members, tmp);

		tmp             = create_const_value(cnt, cnt->builtin_types.entry_usize);
		tmp->data.v_u64 = type->store_size_bytes;
		sa_push_ConstValue(type_info_members, tmp);
	}

	switch (type->kind) {
	case MIR_TYPE_TYPE:
	case MIR_TYPE_VOID:
	case MIR_TYPE_BOOL:
	case MIR_TYPE_NULL:
	case MIR_TYPE_STRING:
	case MIR_TYPE_SLICE:
	case MIR_TYPE_VARGS:
		break;

	case MIR_TYPE_INT: {
		/* .bitcount */
		tmp             = create_const_value(cnt, cnt->builtin_types.entry_s32);
		tmp->data.v_s32 = type->data.integer.bitcount;
		sa_push_ConstValue(members, tmp);

		/* .is_signed */
		tmp              = create_const_value(cnt, cnt->builtin_types.entry_bool);
		tmp->data.v_bool = type->data.integer.is_signed;
		sa_push_ConstValue(members, tmp);
		break;
	}

	case MIR_TYPE_REAL: {
		/* .bitcount */
		tmp             = create_const_value(cnt, cnt->builtin_types.entry_s32);
		tmp->data.v_s32 = type->data.real.bitcount;
		sa_push_ConstValue(members, tmp);
		break;
	}

	case MIR_TYPE_PTR: {
		/* .pointed */
		tmp = create_const_value(cnt, cnt->builtin_types.entry_TypeInfo_ptr);
		MirVar *rtti_pointed = exec_gen_type_RTTI(cnt, type->data.ptr.expr);

		set_const_ptr(&tmp->data.v_ptr, rtti_pointed, MIR_CP_VAR);
		sa_push_ConstValue(members, tmp);
		break;
	}

	case MIR_TYPE_ARRAY: {
		/* .elem */
		tmp               = create_const_value(cnt, cnt->builtin_types.entry_TypeInfo_ptr);
		MirVar *rtti_elem = exec_gen_type_RTTI(cnt, type->data.array.elem_type);

		set_const_ptr(&tmp->data.v_ptr, rtti_elem, MIR_CP_VAR);
		sa_push_ConstValue(members, tmp);

		/* .len */
		tmp             = create_const_value(cnt, cnt->builtin_types.entry_usize);
		tmp->data.v_u64 = type->data.array.len;
		sa_push_ConstValue(members, tmp);

		break;
	}

	case MIR_TYPE_ENUM: {
		/* .base_type */
		tmp = create_const_value(cnt, cnt->builtin_types.entry_TypeInfo_ptr);
		MirVar *rtti_base_type = exec_gen_type_RTTI(cnt, type->data.enm.base_type);

		set_const_ptr(&tmp->data.v_ptr, rtti_base_type, MIR_CP_VAR);
		sa_push_ConstValue(members, tmp);

		break;
	}

	case MIR_TYPE_STRUCT: {
		SmallArray_Member *smembers           = type->data.strct.members;
		const size_t       memc               = members ? smembers->size : 0;
		MirVar *           rtti_var_arr       = NULL;
		MirVar *           rtti_var_arr_names = NULL;

		MirType *rtti_var_arr_type =
		    create_type_array(cnt, cnt->builtin_types.entry_TypeInfo_ptr, memc);

		MirType *rtti_var_arr_names_type =
		    create_type_array(cnt, cnt->builtin_types.entry_string, memc);

		{ /* Members array */
			rtti_var_arr = _create_and_alloc_RTTI_var(cnt, rtti_var_arr_type);
			rtti_var_arr_names = _create_and_alloc_RTTI_var(cnt, rtti_var_arr_names_type);

			SmallArray_ConstValue *elems =
			    create_sarr(SmallArray_ConstValue, cnt->assembly);

			SmallArray_ConstValue *elem_names =
			    create_sarr(SmallArray_ConstValue, cnt->assembly);

			rtti_var_arr->value.data.v_array.elems = elems;
			rtti_var_arr_names->value.data.v_array.elems = elem_names;

			MirMember *member;
			sarray_foreach(smembers, member)
			{
				tmp =
				    create_const_value(cnt, cnt->builtin_types.entry_TypeInfo_ptr);

				MirVar *rtti_base_type = exec_gen_type_RTTI(cnt, member->type);

				set_const_ptr(&tmp->data.v_ptr, rtti_base_type, MIR_CP_VAR);
				sa_push_ConstValue(elems, tmp);

				// TODO: setup strings
			}

			_push_RTTI_var(cnt, rtti_var_arr);
		}

		tmp = create_const_value(cnt, cnt->builtin_types.entry_TypeInfo_slice);

		{ /* .members slice */
			SmallArray_ConstValue *slice_members =
			    create_sarr(SmallArray_ConstValue, cnt->assembly);
			tmp->data.v_struct.members = slice_members;

			MirConstValue *slice_member_tmp =
			    create_const_value(cnt, cnt->builtin_types.entry_usize);
			sa_push_ConstValue(slice_members, slice_member_tmp);
			slice_member_tmp->data.v_u64 = memc;

			MirType *arr_ptr_type = create_type_ptr(cnt, rtti_var_arr_type);
			slice_member_tmp      = create_const_value(cnt, arr_ptr_type);
			sa_push_ConstValue(slice_members, slice_member_tmp);

			MirConstPtr *const_ptr = &slice_member_tmp->data.v_ptr;
			set_const_ptr(const_ptr, rtti_var_arr, MIR_CP_VAR);
		}

		sa_push_ConstValue(members, tmp);
		break;
	}

	case MIR_TYPE_FN: {
		const size_t argc = type->data.fn.arg_types ? type->data.fn.arg_types->size : 0;
		MirVar *     rtti_var_arr = NULL;

		MirType *rtti_var_arr_type =
		    create_type_array(cnt, cnt->builtin_types.entry_TypeInfo_ptr, argc);

		{ /* Args array */
			rtti_var_arr = _create_and_alloc_RTTI_var(cnt, rtti_var_arr_type);

			SmallArray_ConstValue *elems =
			    create_sarr(SmallArray_ConstValue, cnt->assembly);
			rtti_var_arr->value.data.v_array.elems = elems;

			MirType *arg_type;
			sarray_foreach(type->data.fn.arg_types, arg_type)
			{
				tmp =
				    create_const_value(cnt, cnt->builtin_types.entry_TypeInfo_ptr);

				MirVar *rtti_base_type = exec_gen_type_RTTI(cnt, arg_type);

				set_const_ptr(&tmp->data.v_ptr, rtti_base_type, MIR_CP_VAR);
				sa_push_ConstValue(elems, tmp);
			}

			_push_RTTI_var(cnt, rtti_var_arr);
		}

		tmp = create_const_value(cnt, cnt->builtin_types.entry_TypeInfo_slice);

		{ /* .members slice */
			SmallArray_ConstValue *slice_members =
			    create_sarr(SmallArray_ConstValue, cnt->assembly);
			tmp->data.v_struct.members = slice_members;

			MirConstValue *slice_member_tmp =
			    create_const_value(cnt, cnt->builtin_types.entry_usize);
			sa_push_ConstValue(slice_members, slice_member_tmp);
			slice_member_tmp->data.v_u64 = argc;

			MirType *arr_ptr_type = create_type_ptr(cnt, rtti_var_arr_type);
			slice_member_tmp      = create_const_value(cnt, arr_ptr_type);
			sa_push_ConstValue(slice_members, slice_member_tmp);

			MirConstPtr *const_ptr = &slice_member_tmp->data.v_ptr;
			set_const_ptr(const_ptr, rtti_var_arr, MIR_CP_VAR);
		}

		sa_push_ConstValue(members, tmp);

		/* .ret */
		tmp              = create_const_value(cnt, cnt->builtin_types.entry_TypeInfo_ptr);
		MirVar *rtti_ret = exec_gen_type_RTTI(cnt, type->data.fn.ret_type);

		set_const_ptr(&tmp->data.v_ptr, rtti_ret, MIR_CP_VAR);
		sa_push_ConstValue(members, tmp);

		/* .is_vargs */
		tmp              = create_const_value(cnt, cnt->builtin_types.entry_bool);
		tmp->data.v_bool = type->data.fn.is_vargs;
		sa_push_ConstValue(members, tmp);
		break;
	}

	default: {
		char type_name[256];
		mir_type_to_str(type_name, 256, type, true);
		bl_warning("Missing exec RTTI generation for type '%s'", type_name);
		break;
	}
	}

	rtti_value->data.v_struct.members = members;
	type->rtti.var                    = rtti_var;

	_push_RTTI_var(cnt, rtti_var);

	return rtti_var;
}

/*
 * Generate global type table in data segment of an assembly.
 */
void
exec_gen_RTTI_types(Context *cnt)
{
	BHashTable *table = cnt->analyze.RTTI_entry_types;
	if (bo_htbl_size(table) == 0) return;

	{ /* Preload RTTI provided types */
		cnt->builtin_types.entry_TypeInfo_ptr =
		    create_type_ptr(cnt, lookup_builtin(cnt, MIR_BUILTIN_ID_TYPE_INFO));

		cnt->builtin_types.entry_TypeInfo_slice = create_type_struct_special(
		    cnt, MIR_TYPE_SLICE, NULL, cnt->builtin_types.entry_TypeInfo_ptr);
	}

	bo_iterator_t it;
	MirType *     type;
	bhtbl_foreach(table, it)
	{
		type = (MirType *)bo_htbl_iter_peek_key(table, &it);
		exec_gen_type_RTTI(cnt, type);
	}
}

void
exec_instr(Context *cnt, MirInstr *instr)
{
	if (!instr) return;
	if (!instr->analyzed) {
		bl_abort("instruction %s has not been analyzed!", mir_instr_name(instr));
	}

	switch (instr->kind) {
	case MIR_INSTR_CAST:
		exec_instr_cast(cnt, (MirInstrCast *)instr);
		break;
	case MIR_INSTR_ADDROF:
		exec_instr_addrof(cnt, (MirInstrAddrOf *)instr);
		break;
	case MIR_INSTR_BINOP:
		exec_instr_binop(cnt, (MirInstrBinop *)instr);
		break;
	case MIR_INSTR_UNOP:
		exec_instr_unop(cnt, (MirInstrUnop *)instr);
		break;
	case MIR_INSTR_CALL:
		exec_instr_call(cnt, (MirInstrCall *)instr);
		break;
	case MIR_INSTR_RET:
		exec_instr_ret(cnt, (MirInstrRet *)instr);
		break;
	case MIR_INSTR_TYPE_SLICE:
		exec_instr_type_slice(cnt, (MirInstrTypeSlice *)instr);
		break;
	case MIR_INSTR_DECL_VAR:
		exec_instr_decl_var(cnt, (MirInstrDeclVar *)instr);
		break;
	case MIR_INSTR_DECL_REF:
		exec_instr_decl_ref(cnt, (MirInstrDeclRef *)instr);
		break;
	case MIR_INSTR_DECL_DIRECT_REF:
		exec_instr_decl_direct_ref(cnt, (MirInstrDeclDirectRef *)instr);
		break;
	case MIR_INSTR_STORE:
		exec_instr_store(cnt, (MirInstrStore *)instr);
		break;
	case MIR_INSTR_LOAD:
		exec_instr_load(cnt, (MirInstrLoad *)instr);
		break;
	case MIR_INSTR_BR:
		exec_instr_br(cnt, (MirInstrBr *)instr);
		break;
	case MIR_INSTR_COND_BR:
		exec_instr_cond_br(cnt, (MirInstrCondBr *)instr);
		break;
	case MIR_INSTR_PHI:
		exec_instr_phi(cnt, (MirInstrPhi *)instr);
		break;
	case MIR_INSTR_UNREACHABLE:
		exec_instr_unreachable(cnt, (MirInstrUnreachable *)instr);
		break;
	case MIR_INSTR_ARG:
		exec_instr_arg(cnt, (MirInstrArg *)instr);
		break;
	case MIR_INSTR_ELEM_PTR:
		exec_instr_elem_ptr(cnt, (MirInstrElemPtr *)instr);
		break;
	case MIR_INSTR_MEMBER_PTR:
		exec_instr_member_ptr(cnt, (MirInstrMemberPtr *)instr);
		break;
	case MIR_INSTR_VARGS:
		exec_instr_vargs(cnt, (MirInstrVArgs *)instr);
		break;
	case MIR_INSTR_TYPE_INFO:
		exec_instr_type_info(cnt, (MirInstrTypeInfo *)instr);
		break;
	case MIR_INSTR_COMPOUND:
		exec_instr_compound(cnt, NULL, (MirInstrCompound *)instr);
		break;
	case MIR_INSTR_TOANY:
		exec_instr_toany(cnt, (MirInstrToAny *)instr);
		break;

	default:
		bl_abort("missing execution for instruction: %s", mir_instr_name(instr));
	}
}

void
exec_instr_toany(Context *cnt, MirInstrToAny *toany)
{
	MirVar *    tmp      = toany->tmp;
	MirVar *    expr_tmp = toany->expr_tmp;
	MirStackPtr tmp_ptr  = exec_read_stack_ptr(cnt, tmp->rel_stack_ptr, tmp->is_in_gscope);
	MirType *   tmp_type = tmp->value.type;

	{ // set type info
		MirVar *expr_type_rtti = toany->rtti_type->rtti.var;
		assert(expr_type_rtti);

		MirStackPtr dest           = tmp_ptr + get_struct_elem_offest(cnt, tmp_type, 0);
		MirType *   type_info_type = mir_get_struct_elem_type(tmp_type, 0);

		MirStackPtr rtti_ptr = exec_read_stack_ptr(
		    cnt, expr_type_rtti->rel_stack_ptr, expr_type_rtti->is_in_gscope);

		memcpy(dest, &rtti_ptr, type_info_type->store_size_bytes);
	}

	MirStackPtr data_ptr = exec_fetch_value(cnt, toany->expr);

	MirStackPtr dest      = tmp_ptr + get_struct_elem_offest(cnt, tmp_type, 1);
	MirType *   data_type = mir_get_struct_elem_type(tmp_type, 1);

	if (!toany->has_data) {
		memset(dest, 0, data_type->store_size_bytes);
	} else if (expr_tmp) { // set data
		MirStackPtr expr_tmp_ptr =
		    exec_read_stack_ptr(cnt, expr_tmp->rel_stack_ptr, expr_tmp->is_in_gscope);

		if (toany->expr->comptime) {
			exec_copy_comptime_to_stack(cnt, expr_tmp_ptr, (MirConstValue *)data_ptr);
		} else {
			bl_unimplemented;
		}

		memcpy(dest, &expr_tmp_ptr, data_type->store_size_bytes);
	} else {
		memcpy(dest, data_ptr, data_type->store_size_bytes);
	}

	exec_push_stack(cnt, &tmp_ptr, toany->base.value.type);
}

void
exec_instr_phi(Context *cnt, MirInstrPhi *phi)
{
	MirInstrBlock *prev_block = cnt->exec.stack->prev_block;
	assert(prev_block && "Invalid previous block for phi instruction.");
	assert(phi->incoming_blocks && phi->incoming_values);
	assert(phi->incoming_blocks->size == phi->incoming_values->size);

	const size_t c = phi->incoming_values->size;
	assert(c > 0);

	MirInstr *     value = NULL;
	MirInstrBlock *block;
	for (size_t i = 0; i < c; ++i) {
		value = phi->incoming_values->data[i];
		block = (MirInstrBlock *)phi->incoming_blocks->data[i];

		if (block->base.id == prev_block->base.id) break;
	}

	assert(value && "Invalid value for phi income.");

	/* Pop used value from stack or use constant. Result will be pushed on the
	 * stack or used as constant value of phi when phi is compile time known
	 * constant. */
	{
		MirType *phi_type = phi->base.value.type;
		assert(phi_type);

		MirStackPtr value_ptr = exec_fetch_value(cnt, value);

		if (phi->base.comptime) {
			memcpy(&phi->base.value.data, value_ptr, phi_type->store_size_bytes);
		} else {
			exec_push_stack(cnt, value_ptr, phi_type);
		}
	}
}

void
exec_instr_addrof(Context *cnt, MirInstrAddrOf *addrof)
{
	MirInstr *src  = addrof->src;
	MirType * type = src->value.type;
	assert(type);

	if (src->kind == MIR_INSTR_ELEM_PTR || src->kind == MIR_INSTR_COMPOUND) {
		/* address of the element is already on the stack */
		return;
	}

	MirStackPtr ptr = exec_fetch_value(cnt, src);

	ptr = ((MirConstValueData *)ptr)->v_ptr.data.stack_ptr;

	if (addrof->base.comptime) {
		memcpy(&addrof->base.value.data, ptr, type->store_size_bytes);
	} else {
		exec_push_stack(cnt, (MirStackPtr)&ptr, type);
	}
}

void
exec_instr_type_info(Context *cnt, MirInstrTypeInfo *type_info)
{
	// HACK: cleanup stack
	exec_fetch_value(cnt, type_info->expr);

	MirVar *type_info_var = type_info->expr_type->rtti.var;
	assert(type_info_var);

	MirType *type = type_info->base.value.type;
	assert(type);

	MirStackPtr ptr =
	    exec_read_stack_ptr(cnt, type_info_var->rel_stack_ptr, type_info_var->is_in_gscope);

	exec_push_stack(cnt, (MirStackPtr)&ptr, type);
}

void
exec_instr_elem_ptr(Context *cnt, MirInstrElemPtr *elem_ptr)
{
	/* pop index from stack */
	assert(mir_is_pointer_type(elem_ptr->arr_ptr->value.type));
	MirType *         arr_type   = mir_deref_type(elem_ptr->arr_ptr->value.type);
	MirType *         index_type = elem_ptr->index->value.type;
	MirStackPtr       index_ptr  = exec_fetch_value(cnt, elem_ptr->index);
	MirConstValueData result     = {0};

	MirStackPtr arr_ptr = exec_fetch_value(cnt, elem_ptr->arr_ptr);
	arr_ptr             = ((MirConstValueData *)arr_ptr)->v_ptr.data.stack_ptr;
	assert(arr_ptr && index_ptr);

	MirConstValueData index = {0};
	exec_read_value(&index, index_ptr, index_type);

	if (elem_ptr->target_is_slice) {
		MirType *len_type = mir_get_struct_elem_type(arr_type, MIR_SLICE_LEN_INDEX);
		MirType *ptr_type = mir_get_struct_elem_type(arr_type, MIR_SLICE_PTR_INDEX);

		MirType *elem_type = mir_deref_type(ptr_type);
		assert(elem_type);

		MirConstValueData ptr_tmp           = {0};
		MirConstValueData len_tmp           = {0};
		const ptrdiff_t   len_member_offset = get_struct_elem_offest(cnt, arr_type, 0);
		const ptrdiff_t   ptr_member_offset = get_struct_elem_offest(cnt, arr_type, 1);

		MirStackPtr ptr_ptr = arr_ptr + ptr_member_offset;
		MirStackPtr len_ptr = arr_ptr + len_member_offset;

		exec_read_value(&ptr_tmp, ptr_ptr, ptr_type);
		exec_read_value(&len_tmp, len_ptr, len_type);

		if (!ptr_tmp.v_ptr.data.stack_ptr) {
			msg_error("Dereferencing null pointer! Slice has not been set?");
			exec_abort(cnt, 0);
		}

		assert(len_tmp.v_u64 > 0);

		if (index.v_u64 >= len_tmp.v_u64) {
			msg_error("Array index is out of the bounds! Array index is: %llu, but "
			          "array size is: %llu",
			          (unsigned long long)index.v_u64,
			          (unsigned long long)len_tmp.v_u64);
			exec_abort(cnt, 0);
		}

		result.v_ptr.data.stack_ptr = (MirStackPtr)(
		    (ptr_tmp.v_ptr.data.stack_ptr) + (index.v_u64 * elem_type->store_size_bytes));
	} else {
		MirType *elem_type = arr_type->data.array.elem_type;
		assert(elem_type);

		{
			const size_t len = arr_type->data.array.len;
			if (index.v_u64 >= len) {
				msg_error("Array index is out of the bounds! Array index "
				          "is: %llu, "
				          "but array size "
				          "is: %llu",
				          (unsigned long long)index.v_u64,
				          (unsigned long long)len);
				exec_abort(cnt, 0);
			}
		}

		if (elem_ptr->arr_ptr->comptime) bl_abort_issue(57);

		result.v_ptr.data.stack_ptr =
		    (MirStackPtr)((arr_ptr) + (index.v_u64 * elem_type->store_size_bytes));

#if BL_DEBUG
		{
			ptrdiff_t _diff = result.v_u64 - (uintptr_t)arr_ptr;
			assert(_diff / elem_type->store_size_bytes == index.v_u64);
		}
#endif
	}

	/* push result address on the stack */
	exec_push_stack(cnt, &result, elem_ptr->base.value.type);
}

void
exec_instr_member_ptr(Context *cnt, MirInstrMemberPtr *member_ptr)
{
	assert(member_ptr->target_ptr);
	MirType *         target_type = member_ptr->target_ptr->value.type;
	MirConstValueData result      = {0};

	/* lookup for base structure declaration type
	 * IDEA: maybe we can store parent type to the member type? But what about
	 * builtin types???
	 */
	assert(target_type->kind == MIR_TYPE_PTR && "expected pointer");
	target_type = mir_deref_type(target_type);
	assert(mir_is_composit_type(target_type) && "expected structure");

	/* fetch address of the struct begin */
	MirStackPtr ptr = exec_fetch_value(cnt, member_ptr->target_ptr);
	ptr             = ((MirConstValueData *)ptr)->v_ptr.data.stack_ptr;
	assert(ptr);

	LLVMTypeRef llvm_target_type = target_type->llvm_type;
	assert(llvm_target_type && "missing LLVM struct type ref");

	if (member_ptr->builtin_id == MIR_BUILTIN_ID_NONE) {
		assert(member_ptr->scope_entry &&
		       member_ptr->scope_entry->kind == SCOPE_ENTRY_MEMBER);
		MirMember *member = member_ptr->scope_entry->data.member;
		assert(member);
		const int64_t index = member->index;

		/* let the llvm solve poiner offest */
		const ptrdiff_t ptr_offset =
		    get_struct_elem_offest(cnt, target_type, (uint32_t)index);

		result.v_ptr.data.stack_ptr = ptr + ptr_offset; // pointer shift
	} else {
		/* builtin member */
		if (member_ptr->builtin_id == MIR_BUILTIN_ID_ARR_PTR) {
			/* slice .ptr */
			const ptrdiff_t ptr_offset  = get_struct_elem_offest(cnt, target_type, 1);
			result.v_ptr.data.stack_ptr = ptr + ptr_offset; // pointer shift
		} else if (member_ptr->builtin_id == MIR_BUILTIN_ID_ARR_LEN) {
			/* slice .len*/
			const ptrdiff_t len_offset  = get_struct_elem_offest(cnt, target_type, 0);
			result.v_ptr.data.stack_ptr = ptr + len_offset; // pointer shift
		} else {
			bl_abort("invalid slice member!");
		}
	}

	/* push result address on the stack */
	exec_push_stack(cnt, &result, member_ptr->base.value.type);
}

void
exec_instr_unreachable(Context *cnt, MirInstrUnreachable *unr)
{
	msg_error("execution reached unreachable code");
	exec_abort(cnt, 0);
}

void
exec_instr_br(Context *cnt, MirInstrBr *br)
{
	assert(br->then_block);
	cnt->exec.stack->prev_block = br->base.owner_block;
	exec_set_pc(cnt, br->then_block->entry_instr);
}

void
exec_instr_cast(Context *cnt, MirInstrCast *cast)
{
	MirType *         src_type  = cast->expr->value.type;
	MirType *         dest_type = cast->base.value.type;
	MirConstValueData tmp       = {0};

	switch (cast->op) {
	case MIR_CAST_BITCAST:
		/* bitcast is always noop */
		break;

	case MIR_CAST_SEXT: {
		/* src is smaller than dest */
		MirStackPtr from_ptr = exec_fetch_value(cnt, cast->expr);
		exec_read_value(&tmp, from_ptr, src_type);

#define sext_case(v, T)                                                                            \
	case sizeof(v.T):                                                                          \
		tmp.v_s64 = (int64_t)tmp.T;                                                        \
		break;

		// clang-format off
    switch (src_type->store_size_bytes)
    {
      sext_case(tmp, v_s8)
      sext_case(tmp, v_s16)
      sext_case(tmp, v_s32)
    default:
      bl_abort("Invalid sext cast!");
    }
		// clang-format on

#undef sext_case

		if (cast->base.comptime)
			memcpy(&cast->base.value.data, &tmp, sizeof(tmp));
		else
			exec_push_stack(cnt, (MirStackPtr)&tmp, dest_type);

		break;
	}

	case MIR_CAST_FPEXT: {
		/* src is smaller than dest */
		MirStackPtr from_ptr = exec_fetch_value(cnt, cast->expr);
		exec_read_value(&tmp, from_ptr, src_type);

		tmp.v_f64 = (double)tmp.v_f32;

		if (cast->base.comptime)
			memcpy(&cast->base.value.data, &tmp, sizeof(tmp));
		else
			exec_push_stack(cnt, (MirStackPtr)&tmp, dest_type);
		break;
	}

	case MIR_CAST_FPTRUNC: {
		/* src is bigger than dest */
		MirStackPtr from_ptr = exec_fetch_value(cnt, cast->expr);
		exec_read_value(&tmp, from_ptr, src_type);

		tmp.v_f32 = (float)tmp.v_f64;

		if (cast->base.comptime)
			memcpy(&cast->base.value.data, &tmp, sizeof(tmp));
		else
			exec_push_stack(cnt, (MirStackPtr)&tmp, dest_type);
		break;
	}

	case MIR_CAST_FPTOSI: {
		/* real to signed integer */
		MirStackPtr from_ptr = exec_fetch_value(cnt, cast->expr);
		exec_read_value(&tmp, from_ptr, src_type);

		if (src_type->store_size_bytes == sizeof(float))
			tmp.v_s32 = (int32_t)tmp.v_f32;
		else
			tmp.v_s64 = (int64_t)tmp.v_f64;

		if (cast->base.comptime)
			memcpy(&cast->base.value.data, &tmp, sizeof(tmp));
		else
			exec_push_stack(cnt, (MirStackPtr)&tmp, dest_type);
		break;
	}

	case MIR_CAST_FPTOUI: {
		/* real to signed integer */
		MirStackPtr from_ptr = exec_fetch_value(cnt, cast->expr);
		exec_read_value(&tmp, from_ptr, src_type);

		if (src_type->store_size_bytes == sizeof(float))
			tmp.v_u64 = (uint64_t)tmp.v_f32;
		else
			tmp.v_u64 = (uint64_t)tmp.v_f64;

		if (cast->base.comptime)
			memcpy(&cast->base.value.data, &tmp, sizeof(tmp));
		else
			exec_push_stack(cnt, (MirStackPtr)&tmp, dest_type);
		break;
	}

	case MIR_CAST_SITOFP: {
		MirStackPtr from_ptr = exec_fetch_value(cnt, cast->expr);
		exec_read_value(&tmp, from_ptr, src_type);

		if (dest_type->store_size_bytes == sizeof(float)) {
			switch (src_type->store_size_bytes) {
			case sizeof(tmp.v_s8):
				tmp.v_f32 = (float)tmp.v_s8;
				break;
			case sizeof(tmp.v_s16):
				tmp.v_f32 = (float)tmp.v_s16;
				break;
			case sizeof(tmp.v_s32):
				tmp.v_f32 = (float)tmp.v_s32;
				break;
			case sizeof(tmp.v_s64):
				tmp.v_f32 = (float)tmp.v_s64;
				break;
			}
		} else {
			switch (src_type->store_size_bytes) {
			case sizeof(tmp.v_s8):
				tmp.v_f64 = (double)tmp.v_s8;
				break;
			case sizeof(tmp.v_s16):
				tmp.v_f64 = (double)tmp.v_s16;
				break;
			case sizeof(tmp.v_s32):
				tmp.v_f64 = (double)tmp.v_s32;
				break;
			case sizeof(tmp.v_s64):
				tmp.v_f64 = (double)tmp.v_s64;
				break;
			}
		}

		if (cast->base.comptime)
			memcpy(&cast->base.value.data, &tmp, sizeof(tmp));
		else
			exec_push_stack(cnt, (MirStackPtr)&tmp, dest_type);
		break;
	}

	case MIR_CAST_UITOFP: {
		MirStackPtr from_ptr = exec_fetch_value(cnt, cast->expr);
		exec_read_value(&tmp, from_ptr, src_type);

		if (dest_type->store_size_bytes == sizeof(float))
			tmp.v_f32 = (float)tmp.v_u64;
		else
			tmp.v_f64 = (double)tmp.v_u64;

		if (cast->base.comptime)
			memcpy(&cast->base.value.data, &tmp, sizeof(tmp));
		else
			exec_push_stack(cnt, (MirStackPtr)&tmp, dest_type);
		break;
	}

	case MIR_CAST_INTTOPTR:
	case MIR_CAST_PTRTOINT: {
		/* noop for same sizes */
		const size_t src_size  = src_type->store_size_bytes;
		const size_t dest_size = dest_type->store_size_bytes;

		if (src_size != dest_size) {
			/* trunc or zero extend */
			MirStackPtr from_ptr = exec_fetch_value(cnt, cast->expr);
			exec_read_value(&tmp, from_ptr, src_type);

			if (cast->base.comptime)
				memcpy(&cast->base.value.data, &tmp, sizeof(tmp));
			else
				exec_push_stack(cnt, (MirStackPtr)&tmp, dest_type);
		}

		break;
	}

	case MIR_CAST_ZEXT:
	/* src is smaller than dest and destination is unsigned, src value will
	 * be extended with zeros to dest type size */
	case MIR_CAST_TRUNC: {
		/* src is bigger than dest */
		MirStackPtr from_ptr = exec_fetch_value(cnt, cast->expr);
		exec_read_value(&tmp, from_ptr, src_type);

		if (cast->base.comptime)
			memcpy(&cast->base.value.data, &tmp, sizeof(tmp));
		else
			exec_push_stack(cnt, (MirStackPtr)&tmp, dest_type);
		break;
	}

	default:
		bl_abort("invalid cast operation");
	}
}

void
exec_instr_arg(Context *cnt, MirInstrArg *arg)
{
	/* arguments must be pushed before RA in reverse order */
	MirFn *fn = arg->base.owner_block->owner_fn;
	assert(fn);

	MirInstrCall *    caller     = (MirInstrCall *)exec_get_ra(cnt)->callee;
	SmallArray_Instr *arg_values = caller->args;
	assert(arg_values);
	MirInstr *curr_arg_value = arg_values->data[arg->i];

	if (curr_arg_value->comptime) {
		MirType *   type = curr_arg_value->value.type;
		MirStackPtr dest = exec_push_stack_empty(cnt, type);

		exec_copy_comptime_to_stack(cnt, dest, &curr_arg_value->value);
	} else {
		/* resolve argument pointer */
		MirInstr *arg_value = NULL;
		/* starting point */
		MirStackPtr arg_ptr = (MirStackPtr)cnt->exec.stack->ra;
		for (uint32_t i = 0; i <= arg->i; ++i) {
			arg_value = arg_values->data[i];
			assert(arg_value);
			if (arg_value->comptime) continue;
			arg_ptr -= exec_stack_alloc_size(arg_value->value.type->store_size_bytes);
		}

		exec_push_stack(cnt, (MirStackPtr)arg_ptr, arg->base.value.type);
	}
}

void
exec_instr_cond_br(Context *cnt, MirInstrCondBr *br)
{
	assert(br->cond);
	MirType *type = br->cond->value.type;

	/* pop condition from stack */
	MirStackPtr cond = exec_fetch_value(cnt, br->cond);
	assert(cond);

	MirConstValueData tmp = {0};
	exec_read_value(&tmp, cond, type);

	/* Set previous block. */
	cnt->exec.stack->prev_block = br->base.owner_block;
	if (tmp.v_s64) {
		exec_set_pc(cnt, br->then_block->entry_instr);
	} else {
		exec_set_pc(cnt, br->else_block->entry_instr);
	}
}

void
exec_instr_decl_ref(Context *cnt, MirInstrDeclRef *ref)
{
	ScopeEntry *entry = ref->scope_entry;
	assert(entry);

	switch (entry->kind) {
	case SCOPE_ENTRY_VAR: {
		MirVar *var = entry->data.var;
		assert(var);

		const bool  use_static_segment = var->is_in_gscope;
		MirStackPtr real_ptr           = NULL;
		if (var->comptime) {
			real_ptr = (MirStackPtr)&var->value;
		} else {
			real_ptr = exec_read_stack_ptr(cnt, var->rel_stack_ptr, use_static_segment);
		}

		ref->base.value.data.v_ptr.data.stack_ptr = real_ptr;
		break;
	}

	case SCOPE_ENTRY_FN:
	case SCOPE_ENTRY_TYPE:
	case SCOPE_ENTRY_MEMBER:
	case SCOPE_ENTRY_VARIANT:
		break;

	default:
		bl_abort("invalid declaration reference");
	}
}

void
exec_instr_decl_direct_ref(Context *cnt, MirInstrDeclDirectRef *ref)
{
	assert(ref->ref->kind == MIR_INSTR_DECL_VAR);
	MirVar *var = ((MirInstrDeclVar *)ref->ref)->var;
	assert(var);

	const bool  use_static_segment = var->is_in_gscope;
	MirStackPtr real_ptr           = NULL;
	if (var->comptime) {
		real_ptr = (MirStackPtr)&var->value;
	} else {
		real_ptr = exec_read_stack_ptr(cnt, var->rel_stack_ptr, use_static_segment);
	}

	ref->base.value.data.v_ptr.data.stack_ptr = real_ptr;
}

void
exec_instr_compound(Context *cnt, MirStackPtr tmp_ptr, MirInstrCompound *cmp)
{
	MirType *type    = cmp->base.value.type;
	MirVar * tmp_var = cmp->tmp_var;

	bool push_tmp_ptr = false;

	/* We expect compound to be naked when no tmp_ptr has been passed into this function. */
	if (!tmp_ptr) {
		assert(cmp->is_naked);
		assert(tmp_var);
		assert(!tmp_var->is_in_gscope);
		assert(type && type->kind == MIR_TYPE_PTR);

		tmp_ptr = exec_read_stack_ptr(cnt, tmp_var->rel_stack_ptr, false);

		type         = cmp->tmp_var->value.type;
		push_tmp_ptr = true;
	}

	assert(tmp_ptr && "Missing temporary allocation for compound.");
	assert(type);

	SmallArray_Instr *values = cmp->values;

	assert(values);

	MirStackPtr value_ptr;
	MirInstr *  value;

	if (cmp->is_zero_initialized) {
		memset(tmp_ptr, 0, type->store_size_bytes);
	} else {
		MirType *   elem_type;
		MirStackPtr elem_ptr = tmp_ptr;

		sarray_foreach(values, value)
		{
			elem_type = value->value.type;
			switch (type->kind) {

			case MIR_TYPE_STRING:
			case MIR_TYPE_SLICE:
			case MIR_TYPE_VARGS:
			case MIR_TYPE_STRUCT:
				elem_ptr = tmp_ptr + get_struct_elem_offest(cnt, type, (uint32_t)i);
				break;

			case MIR_TYPE_ARRAY:
				elem_ptr = tmp_ptr + get_array_elem_offset(type, (uint32_t)i);
				break;

			default:
				assert(i == 0 && "Invalid elem count for non-agregate type!!!");
			}

			if (value->comptime) {
				exec_copy_comptime_to_stack(cnt, elem_ptr, &value->value);
			} else {
				if (value->kind == MIR_INSTR_COMPOUND) {
					exec_instr_compound(
					    cnt, elem_ptr, (MirInstrCompound *)value);
				} else {
					value_ptr = exec_fetch_value(cnt, value);
					memcpy(elem_ptr, value_ptr, elem_type->store_size_bytes);
				}
			}
		}
	}

	/*
	 * Push pointer to tmp var on the stack. This is done for all naked compounds.
	 */
	if (push_tmp_ptr) {
		MirStackPtr real_ptr = exec_read_stack_ptr(cnt, tmp_var->rel_stack_ptr, false);
		exec_push_stack(cnt, &real_ptr, cmp->base.value.type);
	}
}

void
exec_instr_vargs(Context *cnt, MirInstrVArgs *vargs)
{
	SmallArray_Instr *values    = vargs->values;
	MirVar *          arr_tmp   = vargs->arr_tmp;
	MirVar *          vargs_tmp = vargs->vargs_tmp;

	assert(vargs_tmp->value.type->kind == MIR_TYPE_VARGS);
	assert(vargs_tmp->rel_stack_ptr && "Unalocated vargs slice!!!");
	assert(values);

	MirStackPtr arr_tmp_ptr =
	    arr_tmp ? exec_read_stack_ptr(cnt, arr_tmp->rel_stack_ptr, false) : NULL;

	/* Fill vargs tmp array with values from stack or constants. */
	{
		MirInstr *  value;
		MirStackPtr value_ptr;
		sarray_foreach(values, value)
		{
			const size_t value_size = value->value.type->store_size_bytes;
			MirStackPtr  dest       = arr_tmp_ptr + i * value_size;

			if (value->comptime) {
				exec_copy_comptime_to_stack(cnt, dest, &value->value);
			} else {
				value_ptr = exec_fetch_value(cnt, value);
				memcpy(dest, value_ptr, value_size);
			}
		}
	}

	/* Push vargs slice on the stack. */
	{
		MirStackPtr vargs_tmp_ptr =
		    exec_read_stack_ptr(cnt, vargs_tmp->rel_stack_ptr, false);
		// set len
		{
			MirConstValueData len_tmp = {0};
			MirStackPtr       len_ptr =
			    vargs_tmp_ptr +
			    get_struct_elem_offest(cnt, vargs_tmp->value.type, MIR_SLICE_LEN_INDEX);

			MirType *len_type =
			    mir_get_struct_elem_type(vargs_tmp->value.type, MIR_SLICE_LEN_INDEX);

			len_tmp.v_u64 = values->size;
			memcpy(len_ptr, &len_tmp, len_type->store_size_bytes);
		}

		// set ptr
		{
			MirConstValueData ptr_tmp = {0};
			MirStackPtr       ptr_ptr =
			    vargs_tmp_ptr +
			    get_struct_elem_offest(cnt, vargs_tmp->value.type, MIR_SLICE_PTR_INDEX);

			MirType *ptr_type =
			    mir_get_struct_elem_type(vargs_tmp->value.type, MIR_SLICE_PTR_INDEX);

			ptr_tmp.v_ptr.data.any = arr_tmp_ptr;
			memcpy(ptr_ptr, &ptr_tmp, ptr_type->store_size_bytes);
		}

		exec_push_stack(cnt, vargs_tmp_ptr, vargs_tmp->value.type);
	}
}

void
exec_instr_decl_var(Context *cnt, MirInstrDeclVar *decl)
{
	assert(decl->base.value.type);

	MirVar *var = decl->var;
	assert(var);

	/* compile time known variables cannot be modified and does not need stack
	 * allocated memory, const_value is used instead
	 *
	 * already allocated variables will never be allocated again (in case
	 * declaration is inside loop body!!!)
	 */
	if (var->comptime) return;

	const bool use_static_segment = var->is_in_gscope;

	assert(var->rel_stack_ptr);

	/* initialize variable if there is some init value */
	if (decl->init) {
		MirStackPtr var_ptr =
		    exec_read_stack_ptr(cnt, var->rel_stack_ptr, use_static_segment);
		assert(var_ptr);

		if (decl->init->comptime) {
			/* Compile time constants of agregate type are stored in different way, we
			 * need to produce decomposition of those data. */
			exec_copy_comptime_to_stack(cnt, var_ptr, &decl->init->value);
		} else {
			if (decl->init->kind == MIR_INSTR_COMPOUND) {
				/* used compound initialization!!! */
				exec_instr_compound(cnt, var_ptr, (MirInstrCompound *)decl->init);
			} else {
				/* read initialization value if there is one */
				MirStackPtr init_ptr = exec_fetch_value(cnt, decl->init);
				memcpy(var_ptr, init_ptr, var->value.type->store_size_bytes);
			}
		}
	}
}

void
exec_instr_load(Context *cnt, MirInstrLoad *load)
{
	/* pop source from stack or load directly when src is declaration, push on
	 * to stack dereferenced value of source */
	MirType *src_type  = load->src->value.type;
	MirType *dest_type = load->base.value.type;
	assert(src_type && dest_type);
	assert(mir_is_pointer_type(src_type));

	MirStackPtr src_ptr = exec_fetch_value(cnt, load->src);
	src_ptr             = ((MirConstValueData *)src_ptr)->v_ptr.data.stack_ptr;

	if (!src_ptr) {
		msg_error("Dereferencing null pointer!");
		exec_abort(cnt, 0);
	}

	if (load->base.comptime) {
		memcpy(&load->base.value.data, src_ptr, dest_type->store_size_bytes);
	} else {
		exec_push_stack(cnt, src_ptr, dest_type);
	}
}

void
exec_instr_store(Context *cnt, MirInstrStore *store)
{
	/* loads destination (in case it is not direct reference to declaration) and
	 * source from stack
	 */
	MirType *src_type = store->src->value.type;
	assert(src_type);

	MirStackPtr dest_ptr = exec_fetch_value(cnt, store->dest);
	MirStackPtr src_ptr  = exec_fetch_value(cnt, store->src);

	dest_ptr = ((MirConstValueData *)dest_ptr)->v_ptr.data.stack_ptr;

	assert(dest_ptr && src_ptr);
	memcpy(dest_ptr, src_ptr, src_type->store_size_bytes);
}

void
exec_instr_type_slice(Context *cnt, MirInstrTypeSlice *type_slice)
{
	/* pop elm type */
	MirType *elem_type = *exec_pop_stack_as(cnt, type_slice->elem_type->value.type, MirType **);
	assert(elem_type);

	bl_unimplemented;
	MirConstValueData tmp = {0};
	exec_push_stack(cnt, &tmp, cnt->builtin_types.entry_type);
}

MirConstValue *
exec_call_top_lvl(Context *cnt, MirInstrCall *call)
{
	assert(call && call->base.analyzed);

	assert(call->callee && call->base.value.type);
	MirConstValue *callee_val = &call->callee->value;
	assert(callee_val->type && callee_val->type->kind == MIR_TYPE_FN);

	MirFn *fn = callee_val->data.v_ptr.data.fn;
	exec_fn(cnt, fn, call->args, (MirConstValueData *)&call->base.value);
	return &call->base.value;
}

bool
exec_fn(Context *cnt, MirFn *fn, SmallArray_Instr *args, MirConstValueData *out_value)
{
	assert(fn);
	MirType *ret_type = fn->type->data.fn.ret_type;
	assert(ret_type);
	const bool does_return_value = ret_type->kind != MIR_TYPE_VOID;

	/* push terminal frame on stack */
	exec_push_ra(cnt, NULL);

	/* allocate local variables */
	exec_stack_alloc_local_vars(cnt, fn);

	/* store return frame pointer */
	fn->exec_ret_value = out_value;

	/* setup entry instruction */
	exec_set_pc(cnt, fn->first_block->entry_instr);

	/* iterate over entry block of executable */
	MirInstr *instr, *prev;
	while (true) {
		instr = exec_get_pc(cnt);
		prev  = instr;
		if (!instr || cnt->exec.stack->aborted) break;

		exec_instr(cnt, instr);

		/* stack head can be changed by br instructions */
		if (exec_get_pc(cnt) == prev) exec_set_pc(cnt, instr->next);
	}

	return does_return_value && !cnt->exec.stack->aborted;
}

static inline void
exec_push_dc_arg(Context *cnt, MirStackPtr val_ptr, MirType *type)
{
	assert(type);

	DCCallVM *vm = cnt->assembly->dl.vm;
	assert(vm);
	MirConstValueData tmp = {0};
	exec_read_value(&tmp, val_ptr, type);

	switch (type->kind) {
	case MIR_TYPE_INT: {
		switch (type->store_size_bytes) {
		case sizeof(int64_t):
			dcArgLongLong(vm, tmp.v_s64);
			break;
		case sizeof(int32_t):
			dcArgInt(vm, (DCint)tmp.v_s32);
			break;
		case sizeof(int16_t):
			dcArgShort(vm, (DCshort)tmp.v_s16);
			break;
		case sizeof(int8_t):
			dcArgChar(vm, (DCchar)tmp.v_s8);
			break;
		default:
			bl_abort("unsupported external call integer argument type");
		}
		break;
	}

	case MIR_TYPE_REAL: {
		switch (type->store_size_bytes) {
		case sizeof(float):
			dcArgFloat(vm, tmp.v_f32);
			break;
		case sizeof(double):
			dcArgDouble(vm, tmp.v_f64);
			break;
		default:
			bl_abort("unsupported external call integer argument type");
		}
		break;
	}

	case MIR_TYPE_NULL:
	case MIR_TYPE_PTR: {
		dcArgPointer(vm, (DCpointer)tmp.v_ptr.data.any);
		break;
	}

	default:
		bl_abort("unsupported external call argument type");
	}
}

void
exec_instr_call(Context *cnt, MirInstrCall *call)
{
	assert(call->callee && call->base.value.type);
	assert(call->callee->value.type);

	MirStackPtr       callee_ptr = exec_fetch_value(cnt, call->callee);
	MirConstValueData callee     = {0};

	exec_read_value(&callee, callee_ptr, call->callee->value.type);

	/* Function called via pointer. */
	if (call->callee->value.type->kind == MIR_TYPE_PTR) {
		assert(mir_deref_type(call->callee->value.type)->kind == MIR_TYPE_FN);
		callee.v_ptr.data.fn =
		    callee.v_ptr.data.any ? callee.v_ptr.data.value->data.v_ptr.data.fn : NULL;
	}

	MirFn *fn = callee.v_ptr.data.fn;
	if (fn == NULL) {
		msg_error("Function pointer not set!");
		exec_abort(cnt, 0);
		return;
	}

	assert(fn->type);
	MirType *ret_type = fn->type->data.fn.ret_type;
	assert(ret_type);

	if (is_flag(fn->flags, FLAG_EXTERN)) {
		DCCallVM *vm = cnt->assembly->dl.vm;
		assert(vm);

		/* call setup and clenup */
		assert(fn->extern_entry);
		dcMode(vm, DC_CALL_C_DEFAULT);
		dcReset(vm);

		/* pop all arguments from the stack */
		MirStackPtr       arg_ptr;
		SmallArray_Instr *arg_values = call->args;
		if (arg_values) {
			MirInstr *arg_value;
			sarray_foreach(arg_values, arg_value)
			{
				arg_ptr = exec_fetch_value(cnt, arg_value);
				exec_push_dc_arg(cnt, arg_ptr, arg_value->value.type);
			}
		}

		bool does_return = true;

		MirConstValueData result = {0};
		switch (ret_type->kind) {
		case MIR_TYPE_INT:
			switch (ret_type->store_size_bytes) {
			case sizeof(char):
				result.v_s8 = dcCallChar(vm, fn->extern_entry);
				break;
			case sizeof(short):
				result.v_s16 = dcCallShort(vm, fn->extern_entry);
				break;
			case sizeof(int):
				result.v_s32 = dcCallInt(vm, fn->extern_entry);
				break;
			case sizeof(long long):
				result.v_s64 = dcCallLongLong(vm, fn->extern_entry);
				break;
			default:
				bl_abort("unsupported integer size for external call result");
			}
			break;

		case MIR_TYPE_PTR:
			result.v_ptr.data.any = dcCallPointer(vm, fn->extern_entry);
			break;

		case MIR_TYPE_REAL: {
			switch (ret_type->store_size_bytes) {
			case sizeof(float):
				result.v_f32 = dcCallFloat(vm, fn->extern_entry);
				break;
			case sizeof(double):
				result.v_f64 = dcCallDouble(vm, fn->extern_entry);
				break;
			default:
				bl_abort("unsupported real number size for external call "
				         "result");
			}
			break;
		}

		case MIR_TYPE_VOID:
			dcCallVoid(vm, fn->extern_entry);
			does_return = false;
			break;

		default:
			bl_abort("unsupported external call return type");
		}

		/* PUSH result only if it is used */
		if (call->base.ref_count > 1 && does_return) {
			exec_push_stack(cnt, (MirStackPtr)&result, ret_type);
		}
	} else {
		/* Push current frame stack top. (Later poped by ret instruction)*/
		exec_push_ra(cnt, &call->base);
		assert(fn->first_block->entry_instr);

		exec_stack_alloc_local_vars(cnt, fn);

		/* setup entry instruction */
		exec_set_pc(cnt, fn->first_block->entry_instr);
	}
}

void
exec_instr_ret(Context *cnt, MirInstrRet *ret)
{
	MirFn *fn = ret->base.owner_block->owner_fn;
	assert(fn);

	/* read callee from frame stack */
	MirInstrCall *caller = (MirInstrCall *)exec_get_ra(cnt)->callee;

	MirType *   ret_type     = NULL;
	MirStackPtr ret_data_ptr = NULL;

	/* pop return value from stack */
	if (ret->value) {
		ret_type = ret->value->value.type;
		assert(ret_type);

		ret_data_ptr = exec_fetch_value(cnt, ret->value);
		assert(ret_data_ptr);

		/* TODO: remove */
		/* set fn execution resulting instruction */
		if (fn->exec_ret_value) {
			const size_t size = ret_type->store_size_bytes;
			memcpy(fn->exec_ret_value, ret_data_ptr, size);
		}

		/* discard return value pointer if result is not used on caller side,
		 * this solution is kinda messy... */
		if (!(caller && caller->base.ref_count > 1)) ret_data_ptr = NULL;
	}

	/* do frame stack rollback */
	MirInstr *pc = exec_pop_ra(cnt);

	/* clean up all arguments from the stack */
	if (caller) {
		SmallArray_Instr *arg_values = caller->args;
		if (arg_values) {
			MirInstr *arg_value;
			sarray_foreach(arg_values, arg_value)
			{
				if (arg_value->comptime) continue;
				exec_pop_stack(cnt, arg_value->value.type);
			}
		}
	}

	/* push return value on the stack if there is one */
	if (ret_data_ptr) {
		if (ret->value->comptime) {
			MirStackPtr dest = exec_push_stack_empty(cnt, ret_type);
			exec_copy_comptime_to_stack(cnt, dest, &ret->value->value);
		} else {
			exec_push_stack(cnt, ret_data_ptr, ret_type);
		}
	}

	/* set program counter to next instruction */
	pc = pc ? pc->next : NULL;
	exec_set_pc(cnt, pc);
}

void
exec_instr_binop(Context *cnt, MirInstrBinop *binop)
{
#define _binop_int(_op, _lhs, _rhs, _result, _v_T)                                                 \
	case BINOP_ADD:                                                                            \
		(_result)._v_T = _lhs._v_T + _rhs._v_T;                                            \
		break;                                                                             \
	case BINOP_SUB:                                                                            \
		(_result)._v_T = _lhs._v_T - _rhs._v_T;                                            \
		break;                                                                             \
	case BINOP_MUL:                                                                            \
		(_result)._v_T = _lhs._v_T * _rhs._v_T;                                            \
		break;                                                                             \
	case BINOP_DIV:                                                                            \
		assert(_rhs._v_T != 0 && "divide by zero, this should be an error");               \
		(_result)._v_T = _lhs._v_T / _rhs._v_T;                                            \
		break;                                                                             \
	case BINOP_EQ:                                                                             \
		(_result).v_bool = _lhs._v_T == _rhs._v_T;                                         \
		break;                                                                             \
	case BINOP_NEQ:                                                                            \
		(_result).v_bool = _lhs._v_T != _rhs._v_T;                                         \
		break;                                                                             \
	case BINOP_LESS:                                                                           \
		(_result).v_bool = _lhs._v_T < _rhs._v_T;                                          \
		break;                                                                             \
	case BINOP_LESS_EQ:                                                                        \
		(_result).v_bool = _lhs._v_T == _rhs._v_T;                                         \
		break;                                                                             \
	case BINOP_GREATER:                                                                        \
		(_result).v_bool = _lhs._v_T > _rhs._v_T;                                          \
		break;                                                                             \
	case BINOP_GREATER_EQ:                                                                     \
		(_result).v_bool = _lhs._v_T >= _rhs._v_T;                                         \
		break;

#define binop_case_int(_op, _lhs, _rhs, _result, _v_T)                                             \
	case sizeof(_lhs._v_T): {                                                                  \
		switch (_op) {                                                                     \
			_binop_int(_op, _lhs, _rhs, _result, _v_T);                                \
		case BINOP_SHR:                                                                    \
			(_result)._v_T = _lhs._v_T >> _rhs._v_T;                                   \
			break;                                                                     \
		case BINOP_SHL:                                                                    \
			(_result)._v_T = _lhs._v_T << _rhs._v_T;                                   \
			break;                                                                     \
		case BINOP_MOD:                                                                    \
			(_result)._v_T = _lhs._v_T % _rhs._v_T;                                    \
			break;                                                                     \
		case BINOP_AND:                                                                    \
			(_result)._v_T = _lhs._v_T & _rhs._v_T;                                    \
			break;                                                                     \
		case BINOP_OR:                                                                     \
			(_result)._v_T = _lhs._v_T | _rhs._v_T;                                    \
			break;                                                                     \
		default:                                                                           \
			bl_unimplemented;                                                          \
		}                                                                                  \
	} break;

#define binop_case_real(_op, _lhs, _rhs, _result, _v_T)                                            \
	case sizeof(_lhs._v_T): {                                                                  \
		switch (_op) {                                                                     \
			_binop_int(_op, _lhs, _rhs, _result, _v_T) default : bl_unimplemented;     \
		}                                                                                  \
	} break;
	// clang-format on

	/* binop expects lhs and rhs on stack in exact order and push result again
	 * to the stack */
	MirType *type = binop->lhs->value.type;
	assert(type);

	MirStackPtr lhs_ptr = exec_fetch_value(cnt, binop->lhs);
	MirStackPtr rhs_ptr = exec_fetch_value(cnt, binop->rhs);
	assert(rhs_ptr && lhs_ptr);

	MirConstValueData result = {0};
	MirConstValueData lhs    = {0};
	MirConstValueData rhs    = {0};

	exec_read_value(&lhs, lhs_ptr, type);
	exec_read_value(&rhs, rhs_ptr, type);

	const size_t s = type->store_size_bytes;

	switch (type->kind) {
	case MIR_TYPE_ENUM:
	case MIR_TYPE_PTR:
	case MIR_TYPE_NULL:
	case MIR_TYPE_BOOL:
	case MIR_TYPE_INT: {
		if (type->data.integer.is_signed) {
			switch (s) {
				binop_case_int(binop->op, lhs, rhs, result, v_s8);
				binop_case_int(binop->op, lhs, rhs, result, v_s16);
				binop_case_int(binop->op, lhs, rhs, result, v_s32);
				binop_case_int(binop->op, lhs, rhs, result, v_s64);
			default:
				bl_abort("invalid integer data type");
			}
		} else {
			switch (s) {
				binop_case_int(binop->op, lhs, rhs, result, v_u8);
				binop_case_int(binop->op, lhs, rhs, result, v_u16);
				binop_case_int(binop->op, lhs, rhs, result, v_u32);
				binop_case_int(binop->op, lhs, rhs, result, v_u64);
			default:
				bl_abort("invalid integer data type");
			}
		}
		break;
	}

	case MIR_TYPE_REAL: {
		switch (s) {
			binop_case_real(binop->op, lhs, rhs, result, v_f32);
			binop_case_real(binop->op, lhs, rhs, result, v_f64);
		default:
			bl_abort("invalid real data type");
		}
		break;
	}

	default:
		bl_abort("invalid binop type");
	}

	if (binop->base.comptime)
		memcpy(&binop->base.value.data, &result, sizeof(result));
	else
		exec_push_stack(cnt, &result, binop->base.value.type);
#undef binop_case_int
#undef binop_case_real
#undef _binop_int
}

void
exec_instr_unop(Context *cnt, MirInstrUnop *unop)
{
#define unop_case(_op, _value, _result, _v_T)                                                      \
	case sizeof(_value._v_T): {                                                                \
		switch (_op) {                                                                     \
		case UNOP_NOT:                                                                     \
			(_result)._v_T = !_value._v_T;                                             \
			break;                                                                     \
		case UNOP_NEG:                                                                     \
			(_result)._v_T = _value._v_T * -1;                                         \
			break;                                                                     \
		case UNOP_POS:                                                                     \
			(_result)._v_T = _value._v_T;                                              \
			break;                                                                     \
		default:                                                                           \
			bl_unimplemented;                                                          \
		}                                                                                  \
	} break;

	assert(unop->base.value.type);
	MirType *   value_type = unop->expr->value.type;
	MirStackPtr value_ptr  = exec_fetch_value(cnt, unop->expr);
	assert(value_ptr);

	MirType *type = unop->expr->value.type;
	assert(type);

	MirConstValueData result = {0};
	MirConstValueData value  = {0};
	exec_read_value(&value, value_ptr, type);

	switch (type->kind) {
	case MIR_TYPE_BOOL:
	case MIR_TYPE_INT: {
		const size_t s = type->store_size_bytes;
		if (type->data.integer.is_signed) {
			switch (s) {
				unop_case(unop->op, value, result, v_s8);
				unop_case(unop->op, value, result, v_s16);
				unop_case(unop->op, value, result, v_s32);
				unop_case(unop->op, value, result, v_s64);
			default:
				bl_abort("invalid integer data type");
			}
		} else {
			switch (s) {
				unop_case(unop->op, value, result, v_u8);
				unop_case(unop->op, value, result, v_u16);
				unop_case(unop->op, value, result, v_u32);
				unop_case(unop->op, value, result, v_u64);
			default:
				bl_abort("invalid integer data type");
			}
		}
		break;
	}

	case MIR_TYPE_REAL: {
		const size_t s = type->store_size_bytes;

		switch (s) {
			unop_case(unop->op, value, result, v_f32);
			unop_case(unop->op, value, result, v_f64);
		default:
			bl_abort("invalid real data type");
		}
		break;
	}

	default:
		bl_abort("invalid unop type");
	}

	if (unop->expr->comptime) {
		assert(unop->base.comptime);
		memcpy(&unop->base.value.data, &result, sizeof(result));
	} else {
		exec_push_stack(cnt, &result, value_type);
	}
#undef unop
}

/* MIR builting */
void
ast_defer_block(Context *cnt, Ast *block, bool whole_tree)
{
	SmallArray_DeferStack *stack = &cnt->ast.defer_stack;
	Ast *                  defer;

	for (size_t i = stack->size; i-- > 0;) {
		defer = stack->data[i];

		if (defer->owner_scope == block->owner_scope) {
			sa_pop_DeferStack(stack);
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
	barray_foreach(ublock->data.ublock.nodes, tmp) ast(cnt, tmp);
}

void
ast_block(Context *cnt, Ast *block)
{
	if (cnt->debug_mode) init_llvm_DI_scope(cnt, block->owner_scope);

	Ast *tmp;
	barray_foreach(block->data.block.nodes, tmp) ast(cnt, tmp);

	if (!block->data.block.has_return) ast_defer_block(cnt, block, false);
}

void
ast_test_case(Context *cnt, Ast *test)
{
	/* build test function */
	Ast *ast_block = test->data.test_case.block;
	assert(ast_block);

	MirInstrFnProto *fn_proto = (MirInstrFnProto *)append_instr_fn_proto(cnt, test, NULL, NULL);

	fn_proto->base.value.type = cnt->builtin_types.entry_test_case_fn;

	const bool emit_llvm = cnt->assembly->options.force_test_to_llvm;

	const char *llvm_name = gen_uq_name(cnt, TEST_CASE_FN_NAME);
	MirFn *     fn = create_fn(cnt, test, NULL, llvm_name, FLAG_TEST, fn_proto, emit_llvm);

	assert(test->data.test_case.desc);
	fn->test_case_desc                      = test->data.test_case.desc;
	fn_proto->base.value.data.v_ptr.data.fn = fn;

	bo_array_push_back(cnt->test_cases, fn);

	MirInstrBlock *entry_block =
	    append_block(cnt, fn_proto->base.value.data.v_ptr.data.fn, "entry");

	cnt->ast.exit_block = append_block(cnt, fn_proto->base.value.data.v_ptr.data.fn, "exit");

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
	assert(ast_cond && ast_then);

	MirFn *fn = get_current_fn(cnt);
	assert(fn);

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
	assert(ast_block);

	MirFn *fn = get_current_fn(cnt);
	assert(fn);

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
	assert(cnt->ast.break_block && "break statement outside the loop");
	append_instr_br(cnt, br, cnt->ast.break_block);
}

void
ast_stmt_continue(Context *cnt, Ast *cont)
{
	assert(cnt->ast.continue_block && "break statement outside the loop");
	append_instr_br(cnt, cont, cnt->ast.continue_block);
}

void
ast_stmt_return(Context *cnt, Ast *ret)
{
	/* Return statement produce only setup of .ret temporary and break into the exit block of
	 * the function. */
	MirInstr *value = ast(cnt, ret->data.stmt_return.expr);

	if (!is_current_block_terminated(cnt)) {
		MirFn *fn = get_current_fn(cnt);
		assert(fn);

		if (fn->ret_tmp) {
			if (!value) {
				builder_msg(cnt->builder,
				            BUILDER_MSG_ERROR,
				            ERR_EXPECTED_EXPR,
				            ret->location,
				            BUILDER_CUR_AFTER,
				            "Expected return value.");
			}

			MirInstr *ref = append_instr_decl_direct_ref(cnt, fn->ret_tmp);
			append_instr_store(cnt, ret, value, ref);
		} else if (value) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_UNEXPECTED_EXPR,
			            value->node->location,
			            BUILDER_CUR_WORD,
			            "Unexpected return value.");
		}

		ast_defer_block(cnt, ret->data.stmt_return.owner_block, true);
	}

	assert(cnt->ast.exit_block);
	append_instr_br(cnt, ret, cnt->ast.exit_block);
}

void
ast_stmt_defer(Context *cnt, Ast *defer)
{
	Ast *ast_expr = defer->data.stmt_defer.expr;
	assert(ast_expr && "Missing defer expression.");

	/* push new defer record */
	sa_push_DeferStack(&cnt->ast.defer_stack, defer);
}

MirInstr *
ast_expr_compound(Context *cnt, Ast *cmp)
{
	SmallArray_Ast *ast_values = cmp->data.expr_compound.values;
	Ast *           ast_type   = cmp->data.expr_compound.type;
	MirInstr *      type       = ast(cnt, ast_type);
	assert(type);

	if (!ast_values) {
		return append_instr_compound(cnt, cmp, type, NULL);
	}

	const size_t valc = ast_values->size;

	assert(ast_type);

	SmallArray_Instr *values = create_sarr(SmallArray_Instr, cnt->assembly);
	sa_resize_Instr(values, valc);

	Ast *     ast_value;
	MirInstr *value;

	/* Values must be appended in reverse order. */
	for (size_t i = valc; i-- > 0;) {
		ast_value = ast_values->data[i];
		value     = ast(cnt, ast_value);
		assert(value);
		values->data[i] = value;
	}

	return append_instr_compound(cnt, cmp, type, values);
}

MirInstr *
ast_expr_addrof(Context *cnt, Ast *addrof)
{
	MirInstr *src = ast(cnt, addrof->data.expr_addrof.next);
	assert(src);

	return append_instr_addrof(cnt, addrof, src);
}

MirInstr *
ast_expr_cast(Context *cnt, Ast *cast)
{
	Ast *ast_type = cast->data.expr_cast.type;
	Ast *ast_next = cast->data.expr_cast.next;
	assert(ast_type && ast_next);

	// TODO: const type!!!
	MirInstr *type = ast_create_impl_fn_call(
	    cnt, ast_type, RESOLVE_TYPE_FN_NAME, cnt->builtin_types.entry_resolve_type_fn);
	MirInstr *next = ast(cnt, ast_next);

	return append_instr_cast(cnt, cast, type, next);
}

MirInstr *
ast_expr_sizeof(Context *cnt, Ast *szof)
{
	Ast *ast_node = szof->data.expr_sizeof.node;
	assert(ast_node);

	MirInstr *expr = ast(cnt, ast_node);
	return append_instr_sizeof(cnt, szof, expr);
}

MirInstr *
ast_expr_type_info(Context *cnt, Ast *type_info)
{
	Ast *ast_node = type_info->data.expr_type_info.node;
	assert(ast_node);

	MirInstr *expr = ast(cnt, ast_node);
	return append_instr_type_info(cnt, type_info, expr);
}

MirInstr *
ast_expr_alignof(Context *cnt, Ast *szof)
{
	Ast *ast_node = szof->data.expr_alignof.node;
	assert(ast_node);

	MirInstr *expr = ast(cnt, ast_node);
	return append_instr_alignof(cnt, szof, expr);
}

MirInstr *
ast_expr_deref(Context *cnt, Ast *deref)
{
	MirInstr *next = ast(cnt, deref->data.expr_deref.next);
	assert(next);
	return append_instr_load(cnt, deref, next);
}

MirInstr *
ast_expr_lit_int(Context *cnt, Ast *expr)
{
	return append_instr_const_int(cnt, expr, expr->data.expr_integer.val);
}

MirInstr *
ast_expr_lit_float(Context *cnt, Ast *expr)
{
	return append_instr_const_float(cnt, expr, expr->data.expr_float.val);
}

MirInstr *
ast_expr_lit_double(Context *cnt, Ast *expr)
{
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
	return append_instr_const_char(cnt, expr, expr->data.expr_character.val);
}

MirInstr *
ast_expr_null(Context *cnt, Ast *nl)
{
	return append_instr_const_null(cnt, nl);
}

MirInstr *
ast_expr_call(Context *cnt, Ast *call)
{
	Ast *           ast_callee = call->data.expr_call.ref;
	SmallArray_Ast *ast_args   = call->data.expr_call.args;
	assert(ast_callee);

	SmallArray_Instr *args = create_sarr(SmallArray_Instr, cnt->assembly);

	/* arguments need to be generated into reverse order due to bytecode call
	 * conventions */
	if (ast_args) {
		const size_t argc = ast_args->size;
		sa_resize_Instr(args, argc);
		MirInstr *arg;
		Ast *     ast_arg;
		for (size_t i = argc; i-- > 0;) {
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
	assert(ident);
	assert(ident->kind == AST_IDENT);

	Scope *scope = ident->owner_scope;
	Unit * unit  = ident->location->unit;
	assert(unit);
	assert(scope);

	return append_instr_decl_ref(cnt, ref, unit, &ident->data.ident.id, scope, NULL);
}

MirInstr *
ast_expr_elem(Context *cnt, Ast *elem)
{
	Ast *ast_arr   = elem->data.expr_elem.next;
	Ast *ast_index = elem->data.expr_elem.index;
	assert(ast_arr && ast_index);

	MirInstr *arr_ptr = ast(cnt, ast_arr);
	MirInstr *index   = ast(cnt, ast_index);

	return append_instr_elem_ptr(cnt, elem, arr_ptr, index, false);
}

MirInstr *
ast_expr_member(Context *cnt, Ast *member)
{
	Ast *ast_next = member->data.expr_member.next;
	// assert(ast_next);

	MirInstr *target = ast(cnt, ast_next);
	// assert(target);

	return append_instr_member_ptr(
	    cnt, member, target, member->data.expr_member.ident, NULL, MIR_BUILTIN_ID_NONE);
}

MirInstr *
ast_expr_lit_fn(Context *cnt, Ast *lit_fn)
{
	/* creates function prototype */
	Ast *ast_block   = lit_fn->data.expr_fn.block;
	Ast *ast_fn_type = lit_fn->data.expr_fn.type;

	MirInstrFnProto *fn_proto =
	    (MirInstrFnProto *)append_instr_fn_proto(cnt, lit_fn, NULL, NULL);

	fn_proto->type = ast_create_impl_fn_call(
	    cnt, ast_fn_type, RESOLVE_TYPE_FN_NAME, cnt->builtin_types.entry_resolve_type_fn);
	assert(fn_proto->type);

	MirInstrBlock *prev_block      = get_current_block(cnt);
	MirInstrBlock *prev_exit_block = cnt->ast.exit_block;

	MirFn *fn =
	    create_fn(cnt, lit_fn, NULL, NULL, 0, fn_proto, true); /* TODO: based on user flag!!! */
	fn_proto->base.value.data.v_ptr.data.fn = fn;

	/* function body */
	/* external functions has no body */
	if (!ast_block) return &fn_proto->base;

	/* Set body scope for DI. */
	fn->body_scope = ast_block->owner_scope;

	/* create block for initialization locals and arguments */
	MirInstrBlock *init_block =
	    append_block(cnt, fn_proto->base.value.data.v_ptr.data.fn, "entry");

	/* Every user generated function must contain exit block; this block is invoked last in
	 * every function a eventually can return .ret value stored in temporary storage. When ast
	 * parser hit user defined 'return' statement it sets up .ret temporary if there is one and
	 * produce break into exit block. This approach is needed due to defer statement, because we
	 * need to call defer blocks after return value evaluation and before terminal instruction
	 * of the function. Last defer block always breaks into the exit block. */
	cnt->ast.exit_block = append_block(cnt, fn_proto->base.value.data.v_ptr.data.fn, "exit");

	if (ast_fn_type->data.type_fn.ret_type) {
		set_current_block(cnt, init_block);
		fn->ret_tmp = append_instr_decl_var_impl(
		    cnt, gen_uq_name(cnt, IMPL_RET_TMP), NULL, NULL, true, false, -1, 0);

		set_current_block(cnt, cnt->ast.exit_block);
		MirInstr *ret_init = append_instr_decl_direct_ref(cnt, fn->ret_tmp);

		append_instr_ret(cnt, NULL, ret_init, false);
	} else {
		set_current_block(cnt, cnt->ast.exit_block);
		append_instr_ret(cnt, NULL, NULL, false);
	}

	set_current_block(cnt, init_block);

	/* build MIR for fn arguments */
	SmallArray_Ast *ast_args = ast_fn_type->data.type_fn.args;
	if (ast_args) {
		Ast *ast_arg;
		Ast *ast_arg_name;

		const size_t argc = ast_args->size;
		for (size_t i = argc; i-- > 0;) {
			ast_arg = ast_args->data[i];
			assert(ast_arg->kind == AST_DECL_ARG);
			ast_arg_name = ast_arg->data.decl.name;
			assert(ast_arg_name);

			/* create tmp declaration for arg variable */
			MirInstr *arg = append_instr_arg(cnt, NULL, (unsigned long)i);
			append_instr_decl_var(cnt, ast_arg_name, NULL, arg, true, false, i, 0);

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
	assert(cstr);
	return append_instr_const_string(cnt, lit_string, cstr);
}

MirInstr *
ast_expr_binop(Context *cnt, Ast *binop)
{
	Ast *ast_lhs = binop->data.expr_binop.lhs;
	Ast *ast_rhs = binop->data.expr_binop.rhs;
	assert(ast_lhs && ast_rhs);

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
	assert(ast_next);

	MirInstr *next = ast(cnt, ast_next);
	assert(next);

	return append_instr_unop(cnt, unop, next, unop->data.expr_unary.kind);
}

MirInstr *
ast_expr_type(Context *cnt, Ast *type)
{
	Ast *next_type = type->data.expr_type.type;
	assert(next_type);
	return ast(cnt, next_type);
}

MirInstr *
ast_decl_entity(Context *cnt, Ast *entity)
{
	MirInstr * result        = NULL;
	Ast *      ast_name      = entity->data.decl.name;
	Ast *      ast_type      = entity->data.decl.type;
	Ast *      ast_value     = entity->data.decl_entity.value;
	const bool is_mutable    = entity->data.decl_entity.mut;
	const bool is_in_gscope  = entity->data.decl_entity.in_gscope;
	const bool is_compiler   = is_flag(entity->data.decl_entity.flags, FLAG_COMPILER);
	bool       enable_groups = false;

	assert(ast_name && "Missing entity name.");
	assert(ast_name->kind == AST_IDENT && "Expected identificator.");

	Scope *scope = ast_name->owner_scope;
	ID *   id    = &ast_name->data.ident.id;

	if (ast_value && ast_value->kind == AST_EXPR_LIT_FN) {
		/* recognised function */
		const int32_t flags = entity->data.decl_entity.flags;
		MirInstr *    value = ast(cnt, ast_value);
		enable_groups       = true;
		if (is_in_gscope) {
			value->value.data.v_ptr.data.fn->llvm_name = ast_name->data.ident.id.str;
		} else {
			if (is_flag(entity->data.decl_entity.flags, FLAG_EXTERN))
				value->value.data.v_ptr.data.fn->llvm_name =
				    ast_name->data.ident.id.str;
			else
				value->value.data.v_ptr.data.fn->llvm_name =
				    gen_uq_name(cnt, ast_name->data.ident.id.str);
		}

		value->value.data.v_ptr.data.fn->id        = id;
		value->value.data.v_ptr.data.fn->decl_node = ast_name;
		value->value.data.v_ptr.data.fn->flags     = flags;

		if (ast_type) {
			((MirInstrFnProto *)value)->user_type =
			    ast_create_impl_fn_call(cnt,
			                            ast_type,
			                            RESOLVE_TYPE_FN_NAME,
			                            cnt->builtin_types.entry_resolve_type_fn);
		}

		/* check main */
		if (is_builtin(ast_name, MIR_BUILTIN_ID_MAIN)) {
			assert(!cnt->entry_fn);
			cnt->entry_fn = value->value.data.v_ptr.data.fn;
			ref_instr(cnt->entry_fn->prototype); /* main must be generated into LLVM */

			/* TODO: set flag for fn instance to use it later for DI generation */
			/* TODO: set flag for fn instance to use it later for DI generation */
			/* TODO: set flag for fn instance to use it later for DI generation */
		}
	} else {
		/* other declaration types */
		MirInstr *type =
		    ast_type ? ast_create_impl_fn_call(cnt,
		                                       ast_type,
		                                       RESOLVE_TYPE_FN_NAME,
		                                       cnt->builtin_types.entry_resolve_type_fn)
		             : NULL;

		cnt->ast.current_entity_id = &ast_name->data.ident.id;
		/* initialize value */
		MirInstr *value = NULL;
		if (is_in_gscope) {
			/* Initialization of global variables must be done in implicit
			 * initializer function executed in compile time. Every
			 * initialization function must be able to be executed in compile
			 * time. */
			value =
			    ast_value
			        ? ast_create_impl_fn_call(cnt, ast_value, INIT_VALUE_FN_NAME, NULL)
			        : NULL;
		} else {
			value = ast(cnt, ast_value);
		}

		append_instr_decl_var(cnt,
		                      ast_name,
		                      type,
		                      value,
		                      is_mutable,
		                      is_in_gscope,
		                      -1,
		                      entity->data.decl_entity.flags);

		cnt->ast.current_entity_id = NULL;

		if (is_builtin(ast_name, MIR_BUILTIN_ID_MAIN)) {
			builder_msg(cnt->builder,
			            BUILDER_MSG_ERROR,
			            ERR_EXPECTED_FUNC,
			            ast_name->location,
			            BUILDER_CUR_WORD,
			            "Main is expected to be a function.");
		}
	}

	register_symbol(cnt, ast_name, id, scope, is_compiler, enable_groups);
	return result;
}

MirInstr *
ast_decl_arg(Context *cnt, Ast *arg)
{
	Ast *ast_type = arg->data.decl.type;

	assert(ast_type);
	MirInstr *type = ast(cnt, ast_type);

	return type;
}

MirInstr *
ast_decl_member(Context *cnt, Ast *arg)
{
	Ast *ast_type = arg->data.decl.type;
	Ast *ast_name = arg->data.decl.name;

	assert(ast_type);
	MirInstr *result = ast(cnt, ast_type);

	/* named member? */
	if (ast_name) {
		assert(ast_name->kind == AST_IDENT);
		result = append_instr_decl_member(cnt, ast_name, result);

		register_symbol(
		    cnt, ast_name, &ast_name->data.ident.id, ast_name->owner_scope, false, false);
	}

	assert(result);
	return result;
}

MirInstr *
ast_decl_variant(Context *cnt, Ast *variant)
{
	Ast *ast_name  = variant->data.decl.name;
	Ast *ast_value = variant->data.decl_variant.value;
	assert(ast_name && "Missing enum variant name!");

	MirInstr *value = ast(cnt, ast_value);

	register_symbol(
	    cnt, ast_name, &ast_name->data.ident.id, ast_name->owner_scope, false, false);

	return append_instr_decl_variant(cnt, ast_name, value);
}

MirInstr *
ast_type_ref(Context *cnt, Ast *type_ref)
{
	Ast *ident = type_ref->data.type_ref.ident;
	assert(ident);

	Scope *scope = ident->owner_scope;
	Unit * unit  = ident->location->unit;
	assert(unit);
	assert(scope);

	MirInstr *ref =
	    append_instr_decl_ref(cnt, type_ref, unit, &ident->data.ident.id, scope, NULL);
	return ref;
}

MirInstr *
ast_type_fn(Context *cnt, Ast *type_fn)
{
	Ast *           ast_ret_type  = type_fn->data.type_fn.ret_type;
	SmallArray_Ast *ast_arg_types = type_fn->data.type_fn.args;

	/* return type */
	MirInstr *ret_type = NULL;
	if (ast_ret_type) {
		ret_type = ast(cnt, ast_ret_type);
		ref_instr(ret_type);
	}

	SmallArray_Instr *arg_types = NULL;
	if (ast_arg_types && ast_arg_types->size) {
		const size_t c = ast_arg_types->size;
		arg_types      = create_sarr(SmallArray_Instr, cnt->assembly);
		sa_resize_Instr(arg_types, c);

		Ast *     ast_arg_type;
		MirInstr *arg_type;
		for (size_t i = c; i-- > 0;) {
			ast_arg_type = ast_arg_types->data[i];
			arg_type     = ast(cnt, ast_arg_type);
			ref_instr(arg_type);
			arg_types->data[i] = arg_type;
		}
	}

	return append_instr_type_fn(cnt, type_fn, ret_type, arg_types);
}

MirInstr *
ast_type_arr(Context *cnt, Ast *type_arr)
{
	Ast *ast_elem_type = type_arr->data.type_arr.elem_type;
	Ast *ast_len       = type_arr->data.type_arr.len;
	assert(ast_elem_type && ast_len);

	MirInstr *len       = ast(cnt, ast_len);
	MirInstr *elem_type = ast(cnt, ast_elem_type);
	return append_instr_type_array(cnt, type_arr, elem_type, len);
}

MirInstr *
ast_type_slice(Context *cnt, Ast *type_slice)
{
	Ast *ast_elem_type = type_slice->data.type_arr.elem_type;
	assert(ast_elem_type);

	MirInstr *elem_type = ast(cnt, ast_elem_type);
	return append_instr_type_slice(cnt, type_slice, elem_type);
}

MirInstr *
ast_type_ptr(Context *cnt, Ast *type_ptr)
{
	Ast *ast_type = type_ptr->data.type_ptr.type;
	assert(ast_type && "invalid pointee type");
	MirInstr *type = ast(cnt, ast_type);
	assert(type);
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
	SmallArray_Ast *ast_variants  = type_enum->data.type_enm.variants;
	Ast *           ast_base_type = type_enum->data.type_enm.type;
	assert(ast_variants);

	const size_t varc = ast_variants->size;
	if (varc == 0) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_EMPTY_ENUM,
		            type_enum->location,
		            BUILDER_CUR_WORD,
		            "Empty enumerator.");
		return NULL;
	}

	MirInstr *base_type = ast(cnt, ast_base_type);

	Scope *scope = type_enum->data.type_enm.scope;
	assert(scope);
	if (cnt->debug_mode) init_llvm_DI_scope(cnt, scope);

	SmallArray_Instr *variants = create_sarr(SmallArray_Instr, cnt->assembly);

	/* Build variant instructions */
	MirInstr *variant;
	Ast *     ast_variant;
	sarray_foreach(ast_variants, ast_variant)
	{
		variant = ast(cnt, ast_variant);
		assert(variant);
		sa_push_Instr(variants, variant);
	}

	/* Consume declaration identificator. */
	ID *id                     = cnt->ast.current_entity_id;
	cnt->ast.current_entity_id = NULL;

	return append_instr_type_enum(cnt, type_enum, id, scope, variants, base_type);
}

MirInstr *
ast_type_struct(Context *cnt, Ast *type_struct)
{
	SmallArray_Ast *ast_members = type_struct->data.type_strct.members;
	const bool      is_raw      = type_struct->data.type_strct.raw;
	if (is_raw) {
		bl_abort_issue(31);
	}

	assert(ast_members);

	const size_t memc = ast_members->size;
	if (memc == 0) {
		builder_msg(cnt->builder,
		            BUILDER_MSG_ERROR,
		            ERR_EMPTY_STRUCT,
		            type_struct->location,
		            BUILDER_CUR_WORD,
		            "Empty structure.");
		return NULL;
	}

	SmallArray_Instr *members = create_sarr(SmallArray_Instr, cnt->assembly);

	MirInstr *tmp = NULL;
	Ast *     ast_member;
	sarray_foreach(ast_members, ast_member)
	{
		tmp = ast(cnt, ast_member);
		assert(tmp);
		sa_push_Instr(members, tmp);
	}

	Scope *scope = type_struct->data.type_strct.scope;
	assert(scope);

	if (cnt->debug_mode) init_llvm_DI_scope(cnt, scope);

	/* Consume declaration identificator. */
	ID *id                     = cnt->ast.current_entity_id;
	cnt->ast.current_entity_id = NULL;

	return append_instr_type_struct(cnt, type_struct, id, scope, members, false);
}

MirInstr *
ast_create_impl_fn_call(Context *cnt, Ast *node, const char *fn_name, MirType *fn_type)
{
	if (!node) return NULL;

	/* Sometimes we need to have implicit function return type based on
	 * resulting type of the AST expression, in such case we must allow return
	 * instruction to change function return
	 * type and create dummy type for the function. */
	MirType *final_fn_type  = fn_type;
	bool     infer_ret_type = false;
	if (!final_fn_type) {
		final_fn_type  = create_type_fn(cnt, NULL, NULL, false);
		infer_ret_type = true;
	}

	MirInstrBlock *prev_block = get_current_block(cnt);
	MirInstr *     fn_proto   = append_instr_fn_proto(cnt, NULL, NULL, NULL);
	fn_proto->value.type      = final_fn_type;
	fn_proto->value.data.v_ptr.data.fn =
	    create_fn(cnt, NULL, NULL, fn_name, 0, (MirInstrFnProto *)fn_proto, false);
	fn_proto->value.data.v_ptr.data.fn->type = final_fn_type;

	MirInstrBlock *entry = append_block(cnt, fn_proto->value.data.v_ptr.data.fn, "entry");
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
		return ast_expr_lit_fn(cnt, node);
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
		bl_abort("invalid node %s", ast_get_name(node));
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
	}

	return "UNKNOWN";
}

/* public */
static void
_type_to_str(char *buf, int32_t len, MirType *type, bool prefer_name)
{
#define append_buf(buf, len, str)                                                                  \
	{                                                                                          \
		const size_t filled = strlen(buf);                                                 \
		snprintf((buf) + filled, (len)-filled, "%s", str);                                 \
	}
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
		SmallArray_Member *members = type->data.strct.members;
		MirMember *        tmp;

		append_buf(buf, len, "struct{");
		if (members) {
			sarray_foreach(members, tmp)
			{
				_type_to_str(buf, len, tmp->type, true);
				if (i < members->size - 1) append_buf(buf, len, ", ");
			}
		}
		append_buf(buf, len, "}");

		break;
	}

	case MIR_TYPE_ENUM: {
		SmallArray_Variant *variants = type->data.enm.variants;
		append_buf(buf, len, "enum{");

		if (variants) {
			MirVariant *variant;
			sarray_foreach(variants, variant)
			{
				append_buf(buf, len, variant->id->str);
				append_buf(buf, len, " :: ");

				if (variant->value) {
					char value_str[35];
					snprintf(value_str,
					         array_size(value_str),
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

		MirType *        tmp;
		SmallArray_Type *arg_types = type->data.fn.arg_types;
		if (arg_types) {
			sarray_foreach(arg_types, tmp)
			{
				_type_to_str(buf, len, tmp, true);
				if (i < arg_types->size - 1) append_buf(buf, len, ", ");
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
mir_type_to_str(char *buf, int32_t len, MirType *type, bool prefer_name)
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
	assert(fn_type && fn_type->kind == MIR_TYPE_FN);

	/* TODO: support passing of arguments. */
	if (fn_type->data.fn.arg_types) {
		msg_error("Main function expects arguments, this is not supported yet!");
		return;
	}

	/* tmp return value storage */
	MirConstValueData result = {0};
	if (exec_fn(cnt, cnt->entry_fn, NULL, &result)) {
		int64_t tmp = result.v_s64;
		msg_log("Execution finished with state: %lld\n", (long long)tmp);
	} else {
		msg_log("Execution finished %s\n",
		        cnt->exec.stack->aborted ? "with errors" : "without errors");
	}
}

void
execute_test_cases(Context *cnt)
{
	msg_log("\nExecuting test cases...");

	const size_t c      = bo_array_size(cnt->test_cases);
	int32_t      failed = 0;
	MirFn *      test_fn;
	int32_t      line;
	const char * file;

	barray_foreach(cnt->test_cases, test_fn)
	{
		cnt->exec.stack->aborted = false;
		assert(is_flag(test_fn->flags, FLAG_TEST));
		exec_fn(cnt, test_fn, NULL, NULL);

		line = test_fn->decl_node ? test_fn->decl_node->location->line : -1;
		file = test_fn->decl_node ? test_fn->decl_node->location->unit->filepath : "?";

		msg_log("[ %s ] (%llu/%llu) %s:%d '%s'",
		        cnt->exec.stack->aborted ? RED("FAILED") : GREEN("PASSED"),
		        (unsigned long long)i + 1,
		        (unsigned long long)c,
		        file,
		        line,
		        test_fn->test_case_desc);

		if (cnt->exec.stack->aborted) ++failed;
	}

	{
		int32_t perc = c > 0 ? (int32_t)((float)(c - failed) / (c * 0.01f)) : 100;

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
	{
		// initialize all hashes once
		for (int32_t i = 0; i < _MIR_BUILTIN_ID_COUNT; ++i) {
			builtin_ids[i].hash = bo_hash_from_str(builtin_ids[i].str);
		}
	}

	{ // TYPES
		struct BuiltinTypes *bt = &cnt->builtin_types;
		bt->entry_type          = create_type_type(cnt);
		bt->entry_void          = create_type_void(cnt);

		bt->entry_s8 = create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_S8], 8, true);
		bt->entry_s16 =
		    create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_S16], 16, true);
		bt->entry_s32 =
		    create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_S32], 32, true);
		bt->entry_s64 =
		    create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_S64], 64, true);
		bt->entry_u8 = create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_U8], 8, false);
		bt->entry_u16 =
		    create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_U16], 16, false);
		bt->entry_u32 =
		    create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_U32], 32, false);
		bt->entry_u64 =
		    create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_U64], 64, false);
		bt->entry_usize =
		    create_type_int(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_USIZE], 64, false);
		bt->entry_bool   = create_type_bool(cnt);
		bt->entry_f32    = create_type_real(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_F32], 32);
		bt->entry_f64    = create_type_real(cnt, &builtin_ids[MIR_BUILTIN_ID_TYPE_F64], 64);
		bt->entry_u8_ptr = create_type_ptr(cnt, bt->entry_u8);
		bt->entry_string =
		    create_type_struct_special(cnt,
		                               MIR_TYPE_STRING,
		                               &builtin_ids[MIR_BUILTIN_ID_TYPE_STRING],
		                               bt->entry_u8_ptr);

		bt->entry_resolve_type_fn = create_type_fn(cnt, bt->entry_type, NULL, false);
		bt->entry_test_case_fn    = create_type_fn(cnt, bt->entry_void, NULL, false);

		/* Provide types into global scope */
		provide_builtin_type(cnt, bt->entry_type);
		provide_builtin_type(cnt, bt->entry_s8);
		provide_builtin_type(cnt, bt->entry_s16);
		provide_builtin_type(cnt, bt->entry_s32);
		provide_builtin_type(cnt, bt->entry_s64);
		provide_builtin_type(cnt, bt->entry_u8);
		provide_builtin_type(cnt, bt->entry_u16);
		provide_builtin_type(cnt, bt->entry_u32);
		provide_builtin_type(cnt, bt->entry_u64);
		provide_builtin_type(cnt, bt->entry_usize);
		provide_builtin_type(cnt, bt->entry_bool);
		provide_builtin_type(cnt, bt->entry_f32);
		provide_builtin_type(cnt, bt->entry_f64);
		provide_builtin_type(cnt, bt->entry_string);
	}
}

void
mir_arenas_init(MirArenas *arenas)
{
	arena_init(&arenas->instr, sizeof(union _MirInstr), ARENA_CHUNK_COUNT, NULL);

	arena_init(&arenas->type, sizeof(MirType), ARENA_CHUNK_COUNT, NULL);

	arena_init(&arenas->var, sizeof(MirVar), ARENA_CHUNK_COUNT, NULL);

	arena_init(&arenas->fn, sizeof(MirFn), ARENA_CHUNK_COUNT, NULL);

	arena_init(&arenas->member, sizeof(MirMember), ARENA_CHUNK_COUNT, NULL);

	arena_init(&arenas->variant, sizeof(MirVariant), ARENA_CHUNK_COUNT, NULL);

	arena_init(&arenas->value, sizeof(MirConstValue), ARENA_CHUNK_COUNT / 2, NULL);
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
}

void
mir_run(Builder *builder, Assembly *assembly)
{
	Context cnt;
	memset(&cnt, 0, sizeof(Context));
	cnt.builder                  = builder;
	cnt.assembly                 = assembly;
	cnt.debug_mode               = assembly->options.debug_mode;
	cnt.analyze.verbose_pre      = false;
	cnt.analyze.verbose_post     = false;
	cnt.analyze.queue            = bo_list_new(sizeof(MirInstr *));
	cnt.analyze.RTTI_entry_types = bo_htbl_new(0, 1024);
	cnt.analyze.waiting          = bo_htbl_new_bo(bo_typeof(BArray), true, ANALYZE_TABLE_SIZE);
	cnt.analyze.llvm_di_builder  = assembly->llvm.di_builder;
	cnt.test_cases               = bo_array_new(sizeof(MirFn *));
	cnt.exec.stack               = exec_new_stack(DEFAULT_EXEC_FRAME_STACK_SIZE);
	cnt.tmp_sh                   = bo_string_new(1024);
	cnt.type_table               = assembly->type_table;
	cnt.builtin_types.cache =
	    scope_create(&assembly->arenas.scope, SCOPE_GLOBAL, NULL, 64, NULL);

	sa_init(&cnt.ast.defer_stack);

	/* initialize all builtin types */
	init_builtins(&cnt);

	/* Gen MIR from AST pass */
	Unit *unit;
	barray_foreach(assembly->units, unit)
	{
		ast(&cnt, unit->ast);
	}

	if (builder->errorc) goto SKIP;

	/* Analyze pass */
	analyze(&cnt);
	analyze_report_unresolved(&cnt);

	if (builder->errorc) goto SKIP;

	exec_gen_RTTI_types(&cnt);

	if (assembly->options.run_tests) execute_test_cases(&cnt);
	if (assembly->options.run_main) execute_entry_fn(&cnt);

SKIP:
	bo_unref(cnt.analyze.queue);
	bo_unref(cnt.analyze.waiting);
	bo_unref(cnt.analyze.RTTI_entry_types);
	bo_unref(cnt.test_cases);
	bo_unref(cnt.tmp_sh);

	sa_terminate(&cnt.ast.defer_stack);
	exec_delete_stack(cnt.exec.stack);
}
