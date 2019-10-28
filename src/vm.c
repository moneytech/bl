//************************************************************************************************
// bl
//
// File:   vm.c
// Author: Martin Dorazil
// Date:   9/17/19
//
// Copyright 2019 Martin Dorazil
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

#include "vm.h"
#include "builder.h"
#include "mir.h"
#include "threading.h"

#define MAX_ALIGNMENT 8
#define VERBOSE_EXEC false
#define CHCK_STACK true
#define TMP_STACK_SIZE 262144 /* 256kB */
#define TMP_STACK_INDEX 0
#define MAIN_THREAD_STACK_INDEX 1
#define PTR_SIZE sizeof(void *) /* HACK: can cause problems with different build targets. */

#define pop_stack_as(cnt, type, T) ((T)pop_stack((cnt), (type)))

// Debug helpers
#if BL_DEBUG && VERBOSE_EXEC
/**************************************************************************************************/
#define LOG_PUSH_RA                                                                                \
	{                                                                                          \
		if (vm->stack[si]->pc) {                                                           \
			fprintf(stdout,                                                            \
			        "[%llu] %6llu %20s  PUSH RA\n",                                    \
			        (unsigned long long)si,                                            \
			        vm->stack[si]->pc->id,                                             \
			        mir_instr_name(vm->stack[si]->pc));                                \
		} else {                                                                           \
			fprintf(stdout,                                                            \
			        "[%llu]      - %20s  PUSH RA\n",                                   \
			        (unsigned long long)si,                                            \
			        "Terminal");                                                       \
		}                                                                                  \
	}
/**************************************************************************************************/

/**************************************************************************************************/
#define LOG_POP_RA                                                                                 \
	{                                                                                          \
		fprintf(stdout,                                                                    \
		        "[%llu] %6llu %20s  POP RA\n",                                             \
		        (unsigned long long)si,                                                    \
		        vm->stack[si]->pc->id,                                                     \
		        mir_instr_name(vm->stack[si]->pc));                                        \
	}
/**************************************************************************************************/

/**************************************************************************************************/
#define LOG_PUSH_STACK                                                                             \
	{                                                                                          \
		char type_name[256];                                                               \
		mir_type_to_str(type_name, 256, type, true);                                       \
		if (vm->stack[si]->pc) {                                                           \
			fprintf(stdout,                                                            \
			        "[%llu] %6llu %20s  PUSH    (%luB, %p) %s\n",                      \
			        (unsigned long long)si,                                            \
			        (unsigned long long)vm->stack[si]->pc->id,                         \
			        mir_instr_name(vm->stack[si]->pc),                                 \
			        size,                                                              \
			        tmp,                                                               \
			        type_name);                                                        \
		} else {                                                                           \
			fprintf(stdout,                                                            \
			        "[%llu]      -                       PUSH    (%luB, %p) %s\n",     \
			        (unsigned long long)si,                                            \
			        size,                                                              \
			        tmp,                                                               \
			        type_name);                                                        \
		}                                                                                  \
	}
/**************************************************************************************************/

/**************************************************************************************************/
#define LOG_POP_STACK                                                                              \
	{                                                                                          \
		char type_name[256];                                                               \
		mir_type_to_str(type_name, 256, type, true);                                       \
		if (vm->stack[si]->pc) {                                                           \
			fprintf(stdout,                                                            \
			        "[%llu] %6llu %20s  POP     (%luB, %p) %s\n",                      \
			        (unsigned long long)si,                                            \
			        vm->stack[si]->pc->id,                                             \
			        mir_instr_name(vm->stack[si]->pc),                                 \
			        size,                                                              \
			        vm->stack[si]->top_ptr - size,                                     \
			        type_name);                                                        \
		} else {                                                                           \
			fprintf(stdout,                                                            \
			        "[%llu]      -                       POP     (%luB, %p) %s\n",     \
			        (unsigned long long)si,                                            \
			        size,                                                              \
			        vm->stack[si]->top_ptr - size,                                     \
			        type_name);                                                        \
		}                                                                                  \
	}
/**************************************************************************************************/

#else
#define LOG_PUSH_RA
#define LOG_POP_RA
#define LOG_PUSH_STACK
#define LOG_POP_STACK
#endif

#if BL_DEBUG && CHCK_STACK
#define CHCK_SIZE() sizeof(void *)
#define CHCK_WRITE(_ptr, _data_size) memcpy((_ptr) + (_data_size), &(_ptr), CHCK_SIZE())
#define CHCK_VALIDATE(_ptr, _data_size)                                                            \
	if ((*(intptr_t *)((_ptr) + (_data_size))) != (intptr_t)(_ptr)) {                          \
		BL_ABORT("Stack memory malformed!");                                               \
	}
#else
#define CHCK_SIZE() 0
#define CHCK_WRITE(_ptr, _data_size)                                                               \
	while (0) {                                                                                \
	}

#define CHCK_VALIDATE(_ptr, _data_size)                                                            \
	while (0) {                                                                                \
	}
#endif

TSMALL_ARRAY_TYPE(ConstValue, MirConstValue, 32);

/*************/
/* fwd decls */
/*************/
static VMStack *
stack_new(usize stack_size);

static void
copy_comptime_to_stack(VM *vm, VMStackPtr dest_ptr, MirConstValue *src_value);

/* zero max nesting = unlimited nesting */
static void
print_call_stack(VM *vm, usize si, usize max_nesting);

static void
dyncall_cb_read_arg(VM *vm, MirConstValue *dest, DCArgs *src);

static char
dyncall_cb_handler(DCCallback *cb, DCArgs *args, DCValue *result, void *userdata);

static void
_dyncall_generate_signature(VM *vm, MirType *type);

static const char *
dyncall_generate_signature(VM *vm, MirType *type);

static DCCallback *
dyncall_fetch_callback(VM *vm, MirFn *fn);

static void
dyncall_push_arg(VM *vm, VMStackPtr val_ptr, MirType *type);

static bool
execute_fn_top_level(VM *vm, MirInstr *call, VMStackPtr *out_ptr);

static bool
execute_fn_impl_top_level(VM *vm, MirFn *fn, TSmallArray_ConstValue *args, VMStackPtr *out_ptr);

static bool
_execute_fn_top_level(VM *                    vm,
                      MirFn *                 fn,
                      MirInstr *              call,       /* Optional */
                      TSmallArray_ConstValue *arg_values, /* Optional */
                      VMStackPtr *            out_ptr     /* Optional */
);

static void
calculate_unop(MirConstValueData *out, MirConstValueData *v, UnopKind op, MirType *type);

static void
calculate_binop(MirConstValueData *out,
                MirConstValueData *l,
                MirConstValueData *r,
                BinopKind          op,
                MirType *          type);

static void
make_cast(MirConstValueData *dest,
          MirConstValueData *src,
          MirCastOp          op,
          MirType *          dest_type,
          MirType *          src_type);

static void
interp_instr(VM *vm, MirInstr *instr);

static void
interp_extern_call(VM *vm, MirFn *fn, MirInstrCall *call);

static void
interp_instr_toany(VM *vm, MirInstrToAny *toany);

static void
interp_instr_unreachable(VM *vm, MirInstrUnreachable *unr);

static void
interp_instr_phi(VM *vm, MirInstrPhi *phi);

static void
interp_instr_type_info(VM *vm, MirInstrTypeInfo *type_info);

static void
interp_instr_cast(VM *vm, MirInstrCast *cast);

static void
interp_instr_addrof(VM *vm, MirInstrAddrof *addrof);

static void
interp_instr_br(VM *vm, MirInstrBr *br);

static void
interp_instr_switch(VM *vm, MirInstrSwitch *sw);

static void
interp_instr_elem_ptr(VM *vm, MirInstrElemPtr *elem_ptr);

static void
interp_instr_member_ptr(VM *vm, MirInstrMemberPtr *member_ptr);

static void
interp_instr_arg(VM *vm, MirInstrArg *arg);

static void
interp_instr_cond_br(VM *vm, MirInstrCondBr *br);

static void
interp_instr_load(VM *vm, MirInstrLoad *load);

static void
interp_instr_store(VM *vm, MirInstrStore *store);

static void
interp_instr_binop(VM *vm, MirInstrBinop *binop);

static void
interp_instr_unop(VM *vm, MirInstrUnop *unop);

static void
interp_instr_call(VM *vm, MirInstrCall *call);

static void
interp_instr_ret(VM *vm, MirInstrRet *ret);

static void
interp_instr_compound(VM *vm, VMStackPtr tmp_ptr, MirInstrCompound *init);

static void
interp_instr_vargs(VM *vm, MirInstrVArgs *vargs);

static void
interp_instr_decl_var(VM *vm, MirInstrDeclVar *var);

static void
interp_instr_decl_ref(VM *vm, MirInstrDeclRef *ref);

static void
interp_instr_decl_direct_ref(VM *vm, MirInstrDeclDirectRef *ref);

/**************/
/* evaluation */
/**************/
static void
eval_instr(VM *vm, MirInstr *instr);

static void
eval_instr_call(VM *vm, MirInstrCall *call);

static void
eval_instr_compound(VM *vm, MirInstrCompound *cmp);

static void
eval_instr_ret(VM *vm, MirInstrRet *ret);

static void
eval_instr_member_ptr(VM *vm, MirInstrMemberPtr *member_ptr);

static void
eval_instr_load(VM *vm, MirInstrLoad *load);

static void
eval_instr_sizeof(VM *vm, MirInstrSizeof *szof);

static void
eval_instr_alignof(VM *vm, MirInstrAlignof *alof);

static void
eval_instr_addrof(VM *vm, MirInstrAddrof *addrof);

static void
eval_instr_elem_ptr(VM *vm, MirInstrElemPtr *elem_ptr);

static void
eval_instr_binop(VM *vm, MirInstrBinop *binop);

static void
eval_instr_unop(VM *vm, MirInstrUnop *unop);

static void
eval_instr_cast(VM *vm, MirInstrCast *cast);

static void
eval_instr_decl_direct_ref(VM *vm, MirInstrDeclDirectRef *ref);

static void
eval_instr_decl_ref(VM *vm, MirInstrDeclRef *ref);

/***********/
/* inlines */
/***********/
/* Push caller to the queue when call instruction is evaluated. */
static void
eval_push_caller(VM *vm, MirInstrCall *caller)
{
	tsa_push_InstrPtr(&vm->eval_caller_queue, (MirInstr *)caller);
}

/* Pop latest call instruction when evaluated function returns. */
static MirInstrCall *
eval_pop_caller(VM *vm)
{
	BL_ASSERT(vm->eval_caller_queue.size > 0);
	return (MirInstrCall *)tsa_pop_InstrPtr(&vm->eval_caller_queue);
}

static inline VMStackPtr
deref_stack_ptr(VMStackPtr ptr)
{
	return (VMStackPtr) * (uintptr_t *)ptr;
}

static inline MirFn *
get_callee(MirInstrCall *call)
{
	MirConstValue *callee_val = &call->callee->value;
	BL_ASSERT(callee_val->type && callee_val->type->kind == MIR_TYPE_FN);

	MirFn *fn = callee_val->data.v_ptr.data.fn;
	BL_ASSERT(fn);
	return fn;
}

static inline void
exec_abort(VM *vm, s32 report_stack_nesting)
{
	print_call_stack(vm, MAIN_THREAD_STACK_INDEX, report_stack_nesting);
	vm->stack[MAIN_THREAD_STACK_INDEX]->aborted = true;
}

static inline bool
stack_is_aborted(VM *vm, usize si)
{
	return vm->stack[si]->aborted;
}

static inline MirInstrBlock *
stack_get_prev_block(VM *vm, usize si)
{
	return vm->stack[si]->prev_block;
}

static inline void
stack_set_prev_block(VM *vm, usize si, MirInstrBlock *block)
{
	vm->stack[si]->prev_block = block;
}

static inline usize
stack_alloc_size(usize size)
{
	BL_ASSERT(size != 0);
	size += CHCK_SIZE();
	return size + (MAX_ALIGNMENT - (size % MAX_ALIGNMENT));
}

/* allocate memory on frame stack, size is in bits!!! */
static inline VMStackPtr
stack_alloc(VM *vm, usize si, usize size)
{
	BL_ASSERT(size && "trying to allocate 0 bits on stack");

#if BL_DEBUG && CHCK_STACK
	const usize orig_size = size;
#endif
	VMStack *stack = vm->stack[si];
	size           = stack_alloc_size(size);
	stack->used_bytes += size;
	if (stack->used_bytes > stack->allocated_bytes) {
		msg_error("Stack overflow!!!");
		exec_abort(vm, 10);
	}

	VMStackPtr mem = (VMStackPtr)stack->top_ptr;
	stack->top_ptr = stack->top_ptr + size;

	if (!is_aligned(mem, MAX_ALIGNMENT)) {
		BL_WARNING("BAD ALIGNMENT %p, %d bytes", mem, size);
	}

	CHCK_WRITE(mem, orig_size);

	return mem;
}

/* shift stack top by the size in bytes */
static inline VMStackPtr
stack_free(VM *vm, usize si, usize size)
{
#if BL_DEBUG && CHCK_STACK
	const usize orig_size = size;
#endif

	VMStack *stack = vm->stack[si];

	size               = stack_alloc_size(size);
	VMStackPtr new_top = stack->top_ptr - size;
	if (new_top < (u8 *)(stack->ra + 1)) BL_ABORT("Stack underflow!!!");
	stack->top_ptr = new_top;
	stack->used_bytes -= size;

	CHCK_VALIDATE(new_top, orig_size);

	return new_top;
}

static inline void
push_ra(VM *vm, usize si, MirInstr *caller)
{
	VMStack *stack = vm->stack[si];
	VMFrame *prev  = stack->ra;
	VMFrame *tmp   = (VMFrame *)stack_alloc(vm, si, sizeof(VMFrame));
	tmp->caller    = caller;
	tmp->prev      = prev;
	stack->ra      = tmp;
	LOG_PUSH_RA;
}

static inline MirInstr *
pop_ra(VM *vm, usize si)
{
	VMStack *stack = vm->stack[si];
	if (!stack->ra) return NULL;
	MirInstr *caller = stack->ra->caller;

	LOG_POP_RA;

	/* rollback */
	VMStackPtr new_top_ptr = (VMStackPtr)stack->ra;
	stack->used_bytes      = stack->top_ptr - new_top_ptr;
	stack->top_ptr         = new_top_ptr;
	stack->ra              = stack->ra->prev;
	return caller;
}

static inline VMStackPtr
push_stack_empty(VM *vm, usize si, MirType *type)
{
	BL_ASSERT(type);
	const usize size = type->store_size_bytes;
	BL_ASSERT(size && "pushing zero sized data on stack");
	VMStackPtr tmp = stack_alloc(vm, si, size);

	LOG_PUSH_STACK;
	return tmp;
}

static inline VMStackPtr
push_stack(VM *vm, usize si, void *value, MirType *type)
{
	BL_ASSERT(value && "try to push NULL value");
	VMStackPtr  tmp  = push_stack_empty(vm, si, type);
	const usize size = type->store_size_bytes;
	memcpy(tmp, value, size);

	/* pointer relative to frame top */
	return tmp;
}

static inline VMStackPtr
pop_stack(VM *vm, usize si, MirType *type)
{
	BL_ASSERT(type);
	const usize size = type->store_size_bytes;
	BL_ASSERT(size && "popping zero sized data on stack");

	LOG_POP_STACK;

	return stack_free(vm, si, size);
}

/* Global variables are allocated in static data segment, so there
 * is no need to use relative pointer. When we set ignore to true
 * original pointer is returned as absolute pointer to the stack. */
static inline VMStackPtr
read_stack_ptr(VM *vm, usize si, VMRelStackPtr rel_ptr, bool ignore)
{
	if (ignore) return (VMStackPtr)rel_ptr;
	BL_ASSERT(rel_ptr);

	VMStackPtr base = (VMStackPtr)vm->stack[si]->ra;
	BL_ASSERT(base);
	return base + rel_ptr;
}

static inline VMStackPtr
fetch_comptime_tmp(VM *vm, MirConstValue *v)
{
	BL_ASSERT(v->is_comptime);

	if (v->comptime_alloc) return v->comptime_alloc;

	switch (v->type->kind) {
	case MIR_TYPE_ARRAY:
	case MIR_TYPE_SLICE:
	case MIR_TYPE_STRING:
	case MIR_TYPE_VARGS:
	case MIR_TYPE_STRUCT:
		v->comptime_alloc = push_stack_empty(vm, TMP_STACK_INDEX, v->type);
		copy_comptime_to_stack(vm, v->comptime_alloc, v);
		return v->comptime_alloc;

	default:
		v->comptime_alloc = (VMStackPtr)&v->data;
		return v->comptime_alloc;
	}
}

/*
 * Return pointer to value evaluated from src instruction value.
 */
static inline VMStackPtr
fetch_value(VM *vm, MirInstr *src)
{
	if (src->value.is_comptime) {
		return fetch_comptime_tmp(vm, &src->value);
	} else {
		return pop_stack(vm, MAIN_THREAD_STACK_INDEX, src->value.type);
	}
}

static inline void
read_value(MirConstValueData *dest, VMStackPtr src, MirType *type)
{
	BL_ASSERT(dest && src && type);
	const usize size = type->store_size_bytes;

	switch (type->kind) {
	case MIR_TYPE_INT:
	case MIR_TYPE_REAL:
	case MIR_TYPE_ENUM:
	case MIR_TYPE_BOOL:
	case MIR_TYPE_NULL:
		memcpy(dest, src, size);
		break;

	case MIR_TYPE_FN:
		mir_set_const_ptr(&dest->v_ptr, *(MirFn **)src, MIR_CP_FN);
		break;

	case MIR_TYPE_TYPE:
		mir_set_const_ptr(&dest->v_ptr, *(MirType **)src, MIR_CP_TYPE);
		break;

	case MIR_TYPE_PTR:
		mir_set_const_ptr(&dest->v_ptr, *(void **)src, MIR_CP_UNKNOWN);
		break;

	default: {
		char type_name[256];
		mir_type_to_str(type_name, 256, type, true);
		BL_ABORT("Cannot load pointer to value of type '%s'", type_name);
	}
	}
}

static inline MirInstr *
stack_get_pc(VM *vm, usize si)
{
	return vm->stack[si]->pc;
}

static inline VMFrame *
stack_get_ra(VM *vm, usize si)
{
	return vm->stack[si]->ra;
}

static inline void
set_pc(VM *vm, usize si, MirInstr *instr)
{
	vm->stack[si]->pc = instr;
}

static inline VMRelStackPtr
stack_alloc_var(VM *vm, usize si, MirVar *var)
{
	BL_ASSERT(var);
	BL_ASSERT(!var->is_comptime && "cannot allocate compile time constant");
	/* allocate memory for variable on stack */

	VMStackPtr tmp     = push_stack_empty(vm, si, var->value.type);
	var->rel_stack_ptr = tmp - (VMStackPtr)vm->stack[si]->ra;
	return var->rel_stack_ptr;
}

static inline void
stack_alloc_local_vars(VM *vm, usize si, MirFn *fn)
{
	BL_ASSERT(fn);
	/* Init all stack variables. */
	TArray *vars = fn->variables;
	MirVar *var;
	TARRAY_FOREACH(MirVar *, vars, var)
	{
		/* Compile time variables does not require stack
		 * allocations in general. When variable's value is
		 * evaluated in LAZY mode it will create temporary
		 * stack
		 * allocation on first use, containing copy of the
		 * const expression value. */
		if (var->is_comptime) continue;
		stack_alloc_var(vm, si, var);
	}
}

/********/
/* impl */
/********/
void
print_call_stack(VM *vm, usize si, usize max_nesting)
{
	VMStack * stack = vm->stack[si];
	MirInstr *instr = stack->pc;
	VMFrame * fr    = stack->ra;
	usize     n     = 0;

	if (!instr) return;
	/* print last instruction */
	builder_msg(BUILDER_MSG_LOG, 0, instr->node->location, BUILDER_CUR_WORD, "");

	while (fr) {
		instr = (MirInstr *)fr->caller;
		fr    = fr->prev;
		if (!instr) break;

		if (max_nesting && n == max_nesting) {
			msg_note("continue...");
			break;
		}

		builder_msg(BUILDER_MSG_LOG, 0, instr->node->location, BUILDER_CUR_WORD, "");
		++n;
	}
}

/*
 * Produce decomposition of compile time known value to the stack
 * location. Stack location must have enough allocated space.
 */
void
copy_comptime_to_stack(VM *vm, VMStackPtr dest_ptr, MirConstValue *src_value)
{
	/* This may cause recursive calls for aggregate data types.
	 */
	BL_ASSERT(dest_ptr && src_value);
	MirConstValueData *data     = &src_value->data;
	MirType *          src_type = src_value->type;
	BL_ASSERT(src_type);

	switch (src_type->kind) {
	case MIR_TYPE_SLICE:
	case MIR_TYPE_STRING:
	case MIR_TYPE_VARGS:
	case MIR_TYPE_STRUCT: {
		if (src_value->data.v_struct.is_zero_initializer) {
			memset(dest_ptr, 0, src_type->store_size_bytes);
		} else {
			TSmallArray_ConstValuePtr *members = data->v_struct.members;
			MirConstValue *            member;

			BL_ASSERT(members);
			const usize memc = members->size;
			for (u32 i = 0; i < memc; ++i) {
				member = members->data[i];

				/* copy all members to variable
				 * allocated memory on the stack */
				VMStackPtr elem_dest_ptr =
				    dest_ptr +
				    mir_get_struct_elem_offest(vm->assembly, src_type, i);
				BL_ASSERT(elem_dest_ptr);
				copy_comptime_to_stack(vm, elem_dest_ptr, member);
			}
		}
		break;
	}

	case MIR_TYPE_ARRAY: {
		if (src_value->data.v_array.is_zero_initializer) {
			memset(dest_ptr, 0, src_type->store_size_bytes);
		} else {
			TSmallArray_ConstValuePtr *elems = data->v_array.elems;
			MirConstValue *            elem;

			BL_ASSERT(elems);
			const usize memc = elems->size;
			for (u32 i = 0; i < memc; ++i) {
				elem = elems->data[i];

				/* copy all elems to variable
				 * allocated memory on the stack
				 */
				VMStackPtr elem_dest_ptr =
				    dest_ptr + mir_get_array_elem_offset(src_type, i);
				copy_comptime_to_stack(vm, elem_dest_ptr, elem);
			}
		}

		break;
	}

	case MIR_TYPE_PTR: {
		MirConstPtr *const_ptr = &src_value->data.v_ptr;
		switch (const_ptr->kind) {

		case MIR_CP_VAR: {
			MirVar *var = const_ptr->data.var;
			BL_ASSERT(var);

			VMStackPtr var_ptr = NULL;
			if (var->value.is_comptime) {
				var_ptr = (VMStackPtr)&var->value;
			} else {
				var_ptr = read_stack_ptr(vm,
				                         MAIN_THREAD_STACK_INDEX,
				                         var->rel_stack_ptr,
				                         var->is_in_gscope);
			}

			memcpy(dest_ptr, &var_ptr, src_type->store_size_bytes);
			break;
		}

		default: {
			memcpy(dest_ptr, (VMStackPtr)src_value, src_type->store_size_bytes);
		}
		}

		break;
	}

	default:
		BL_ASSERT(dest_ptr && "Invalid destination pointer");
		BL_ASSERT(src_value && "Invalid source value pointer");
		memcpy(dest_ptr, (VMStackPtr)src_value, src_type->store_size_bytes);
	}
}

void
dyncall_cb_read_arg(VM *vm, MirConstValue *dest, DCArgs *src)
{
	BL_ASSERT(dest->type && "Argument destination has no type specified.");

	memset(&dest->data, 0, sizeof(dest->data));

	switch (dest->type->kind) {
	case MIR_TYPE_INT: {
		const s32 bitcount = dest->type->data.integer.bitcount;
		switch (bitcount) {
		case 8:
			dest->data.v_u8 = dcbArgUChar(src);
			break;
		case 16:
			dest->data.v_u16 = dcbArgUShort(src);
			break;
		case 32:
			dest->data.v_u32 = dcbArgULong(src);
			break;
		case 64:
			dest->data.v_u64 = dcbArgULongLong(src);
			break;
		default:
			BL_ABORT("invalid bitcount");
		}

		break;
	}

	case MIR_TYPE_REAL: {
		const s32 bitcount = dest->type->data.real.bitcount;
		switch (bitcount) {
		case 32:
			dest->data.v_f32 = dcbArgFloat(src);
			break;
		case 64:
			dest->data.v_f64 = dcbArgDouble(src);
			break;
		default:
			BL_ABORT("invalid bitcount");
		}

		break;
	}

	case MIR_TYPE_BOOL: {
		dest->data.v_bool = (bool)dcbArgBool(src);
		break;
	}

	case MIR_TYPE_PTR: {
		mir_set_const_ptr(&dest->data.v_ptr, dcbArgPointer(src), MIR_CP_STACK);
		break;
	}

	default:
		BL_UNIMPLEMENTED;
	}
}

char
dyncall_cb_handler(DCCallback *cb, DCArgs *dc_args, DCValue *result, void *userdata)
{
	/* TODO: External callback can be invoked from different
	 * thread. This can cause problems for now since interpreter
	 * is strictly single-threaded, but we must handle such
	 * situation in future. */
	BL_ASSERT(thread_get_id() == main_thread_id &&
	          "External callback handler must be invoked from "
	          "main thread.");

	DyncallCBContext *cnt = (DyncallCBContext *)userdata;
	MirFn *           fn  = cnt->fn;
	VM *              vm  = cnt->vm;
	BL_ASSERT(fn && vm);

	MirType *  ret_type     = fn->type->data.fn.ret_type;
	const bool is_fn_extern = IS_FLAG(cnt->fn->flags, FLAG_EXTERN);
	const bool has_args     = fn->type->data.fn.args;
	const bool has_return   = ret_type->kind != MIR_TYPE_VOID;

	if (is_fn_extern) {
		/* TODO: external callback */
		/* TODO: external callback */
		/* TODO: external callback */
		BL_ABORT("External function used as callback is "
		         "not supported yet!");
	}

	TSmallArray_ConstValue arg_tmp;
	tsa_init(&arg_tmp);

	if (has_args) {
		TSmallArray_ArgPtr *args = fn->type->data.fn.args;
		tsa_resize_ConstValue(&arg_tmp, args->size);

		MirArg *it;
		TSA_FOREACH(args, it)
		{
			arg_tmp.data[i].type = it->type;
			dyncall_cb_read_arg(vm, &arg_tmp.data[i], dc_args);
		}
	}

	VMStackPtr ret_ptr = NULL;
	if (!execute_fn_impl_top_level(vm, fn, &arg_tmp, &ret_ptr)) {
		result->L = 0;
	} else if (has_return) {
		BL_ASSERT(ret_ptr && "Function is supposed to return some value.");
		MirConstValueData tmp = {0};
		read_value(&tmp, ret_ptr, ret_type);
		result->L = tmp.v_s64;
	}

	tsa_terminate(&arg_tmp);
	return dyncall_generate_signature(vm, ret_type)[0];
}

void
_dyncall_generate_signature(VM *vm, MirType *type)
{
	TSmallArray_Char *tmp = &vm->dyncall_sig_tmp;

	switch (type->kind) {
	case MIR_TYPE_FN: {
		if (type->data.fn.args) {
			MirArg *arg;
			TSA_FOREACH(type->data.fn.args, arg)
			{
				_dyncall_generate_signature(vm, arg->type);
			}
		}
		tsa_push_Char(tmp, DC_SIGCHAR_ENDARG);
		_dyncall_generate_signature(vm, type->data.fn.ret_type);
		break;
	}

	case MIR_TYPE_INT: {
		const bool is_signed = type->data.integer.is_signed;
		switch (type->store_size_bytes) {
		case 1:
			tsa_push_Char(tmp, is_signed ? DC_SIGCHAR_CHAR : DC_SIGCHAR_UCHAR);
			break;
		case 2:
			tsa_push_Char(tmp, is_signed ? DC_SIGCHAR_SHORT : DC_SIGCHAR_USHORT);
			break;
		case 4:
			tsa_push_Char(tmp, is_signed ? DC_SIGCHAR_INT : DC_SIGCHAR_UINT);
			break;
		case 8:
			tsa_push_Char(tmp, is_signed ? DC_SIGCHAR_LONGLONG : DC_SIGCHAR_ULONGLONG);
			break;
		}
		break;
	}

	case MIR_TYPE_REAL: {
		switch (type->store_size_bytes) {
		case 4:
			tsa_push_Char(tmp, DC_SIGCHAR_FLOAT);
			break;
		case 8:
			tsa_push_Char(tmp, DC_SIGCHAR_DOUBLE);
			break;
		}
		break;
	}

	case MIR_TYPE_NULL:
	case MIR_TYPE_PTR: {
		tsa_push_Char(tmp, DC_SIGCHAR_POINTER);
		break;
	}

	case MIR_TYPE_VOID: {
		tsa_push_Char(tmp, DC_SIGCHAR_VOID);
		break;
	}

	case MIR_TYPE_STRUCT: {
		TSmallArray_MemberPtr *members = type->data.strct.members;
		MirMember *            member;
		TSA_FOREACH(members, member)
		{
			_dyncall_generate_signature(vm, member->type);
		}
		break;
	}

	case MIR_TYPE_ENUM: {
		_dyncall_generate_signature(vm, type->data.enm.base_type);
		break;
	}

	case MIR_TYPE_ARRAY: {
		for (s64 i = 0; i < type->data.array.len; i += 1) {
			_dyncall_generate_signature(vm, type->data.array.elem_type);
		}
		break;
	}

	default: {
		char type_name[256];
		mir_type_to_str(type_name, 256, type, true);
		BL_ABORT("Unsupported DC-signature type '%s'.", type_name);
	}
	}
}

const char *
dyncall_generate_signature(VM *vm, MirType *type)
{
	TSmallArray_Char *tmp = &vm->dyncall_sig_tmp;
	tmp->size             = 0; /* reset size */

	_dyncall_generate_signature(vm, type);
	tsa_push_Char(tmp, '\0');

	return tmp->data;
}

DCCallback *
dyncall_fetch_callback(VM *vm, MirFn *fn)
{
	if (fn->dyncall.extern_callback_handle) return fn->dyncall.extern_callback_handle;

	const char *sig = dyncall_generate_signature(vm, fn->type);

	fn->dyncall.context = (DyncallCBContext){.fn = fn, .vm = vm};

	fn->dyncall.extern_callback_handle =
	    dcbNewCallback(sig, &dyncall_cb_handler, &fn->dyncall.context);

	return fn->dyncall.extern_callback_handle;
}

void
dyncall_push_arg(VM *vm, VMStackPtr val_ptr, MirType *type)
{
	BL_ASSERT(type);

	/* CLEANUP: include dvm into VM */
	/* CLEANUP: include dvm into VM */
	/* CLEANUP: include dvm into VM */
	/* CLEANUP: include dvm into VM */
	DCCallVM *dvm = vm->assembly->dl.vm;
	BL_ASSERT(dvm);
	MirConstValueData tmp = {0};

	if (type->kind == MIR_TYPE_ENUM) {
		type = type->data.enm.base_type;
	}

	switch (type->kind) {
	case MIR_TYPE_BOOL: {
		read_value(&tmp, val_ptr, type);
		dcArgBool(dvm, tmp.v_bool);
		break;
	}

	case MIR_TYPE_INT: {
		read_value(&tmp, val_ptr, type);
		switch (type->store_size_bytes) {
		case 1:
			dcArgChar(dvm, (DCchar)tmp.v_s8);
			break;
		case 2:
			dcArgShort(dvm, (DCshort)tmp.v_s16);
			break;
		case 4:
			dcArgInt(dvm, (DCint)tmp.v_s32);
			break;
		case 8:
			dcArgLongLong(dvm, tmp.v_s64);
			break;
		default:
			BL_ABORT("unsupported external call "
			         "integer argument type");
		}
		break;
	}

	case MIR_TYPE_REAL: {
		read_value(&tmp, val_ptr, type);
		switch (type->store_size_bytes) {
		case 4:
			dcArgFloat(dvm, tmp.v_f32);
			break;
		case 8:
			dcArgDouble(dvm, tmp.v_f64);
			break;
		default:
			BL_ABORT("unsupported external call "
			         "integer argument type");
		}
		break;
	}

	case MIR_TYPE_NULL: {
		read_value(&tmp, val_ptr, type);
		dcArgPointer(dvm, (DCpointer)tmp.v_ptr.data.any);
		break;
	}

	case MIR_TYPE_STRUCT: {
		BL_ABORT("External function taking structure "
		         "argument by value cannot be executed by "
		         "interpreter on this platform.");
		break;
	}

	case MIR_TYPE_ARRAY: {
		BL_ABORT("External function taking array argument "
		         "by value cannot be executed by "
		         "interpreter on this platform.");
		break;
	}

	case MIR_TYPE_PTR: {
		read_value(&tmp, val_ptr, type);

		if (mir_deref_type(type)->kind == MIR_TYPE_FN) {
			MirConstValue *value = (MirConstValue *)val_ptr;

			BL_ASSERT(value->data.v_ptr.data.fn);
			BL_ASSERT(value->data.v_ptr.kind == MIR_CP_FN);

			MirFn *fn = value->data.v_ptr.data.fn;
			dcArgPointer(dvm, (DCpointer)dyncall_fetch_callback(vm, fn));
		} else {
			dcArgPointer(dvm, (DCpointer)tmp.v_ptr.data.any);
		}
		break;
	}

	default:
		BL_ABORT("unsupported external call argument type");
	}
}

void
interp_extern_call(VM *vm, MirFn *fn, MirInstrCall *call)
{
	MirType *ret_type = fn->type->data.fn.ret_type;
	BL_ASSERT(ret_type);

	DCCallVM *dvm = vm->assembly->dl.vm;
	BL_ASSERT(vm);

	/* call setup and clenup */
	if (!fn->dyncall.extern_entry) {
		msg_error("External function '%s' not found!", fn->linkage_name);
		exec_abort(vm, 0);
		return;
	}

	dcMode(dvm, DC_CALL_C_DEFAULT);
	dcReset(dvm);

	/* pop all arguments from the stack */
	VMStackPtr            arg_ptr;
	TSmallArray_InstrPtr *arg_values = call->args;
	if (arg_values) {
		MirInstr *arg_value;
		TSA_FOREACH(arg_values, arg_value)
		{
			arg_ptr = fetch_value(vm, arg_value);
			dyncall_push_arg(vm, arg_ptr, arg_value->value.type);
		}
	}

	bool does_return = true;

	MirConstValueData result = {0};
	switch (ret_type->kind) {
	case MIR_TYPE_INT:
		switch (ret_type->store_size_bytes) {
		case 1:
			result.v_s8 = dcCallChar(dvm, fn->dyncall.extern_entry);
			break;
		case 2:
			result.v_s16 = dcCallShort(dvm, fn->dyncall.extern_entry);
			break;
		case 4:
			result.v_s32 = dcCallInt(dvm, fn->dyncall.extern_entry);
			break;
		case 8:
			result.v_s64 = dcCallLongLong(dvm, fn->dyncall.extern_entry);
			break;
		default:
			BL_ABORT("unsupported integer size for "
			         "external call result");
		}
		break;

	case MIR_TYPE_ENUM:
		switch (ret_type->data.enm.base_type->store_size_bytes) {
		case 1:
			result.v_s8 = dcCallChar(dvm, fn->dyncall.extern_entry);
			break;
		case 2:
			result.v_s16 = dcCallShort(dvm, fn->dyncall.extern_entry);
			break;
		case 4:
			result.v_s32 = dcCallInt(dvm, fn->dyncall.extern_entry);
			break;
		case 8:
			result.v_s64 = dcCallLongLong(dvm, fn->dyncall.extern_entry);
			break;
		default:
			BL_ABORT("unsupported integer size for "
			         "external call result");
		}
		break;

	case MIR_TYPE_PTR:
		result.v_ptr.data.any = dcCallPointer(dvm, fn->dyncall.extern_entry);
		break;

	case MIR_TYPE_REAL: {
		switch (ret_type->store_size_bytes) {
		case 4:
			result.v_f32 = dcCallFloat(dvm, fn->dyncall.extern_entry);
			break;
		case 8:
			result.v_f64 = dcCallDouble(dvm, fn->dyncall.extern_entry);
			break;
		default:
			BL_ABORT("Unsupported real number size for "
			         "external call "
			         "result");
		}
		break;
	}

	case MIR_TYPE_VOID:
		dcCallVoid(dvm, fn->dyncall.extern_entry);
		does_return = false;
		break;

	case MIR_TYPE_STRUCT: {
		BL_ABORT("External function '%s' returning "
		         "structure cannot be executed by "
		         "interpreter on "
		         "this platform.",
		         fn->id->str);
	}

	case MIR_TYPE_ARRAY: {
		BL_ABORT("External function '%s' returning array "
		         "cannot be executed by interpreter on "
		         "this platform.",
		         fn->id->str);
	}

	default: {
		char type_name[256];
		mir_type_to_str(type_name, 256, ret_type, true);
		BL_ABORT("Unsupported external call return type '%s'", type_name);
	}
	}

	/* PUSH result only if it is used */
	if (call->base.ref_count > 1 && does_return) {
		push_stack(vm, MAIN_THREAD_STACK_INDEX, (VMStackPtr)&result, ret_type);
	}
}

bool
execute_fn_top_level(VM *vm, MirInstr *call, VMStackPtr *out_ptr)
{
	return _execute_fn_top_level(vm, get_callee((MirInstrCall *)call), call, NULL, out_ptr);
}

bool
execute_fn_impl_top_level(VM *vm, MirFn *fn, TSmallArray_ConstValue *args, VMStackPtr *out_ptr)
{
	return _execute_fn_top_level(vm, fn, NULL, args, out_ptr);
}

bool
_execute_fn_top_level(VM *                    vm,
                      MirFn *                 fn,
                      MirInstr *              call,
                      TSmallArray_ConstValue *arg_values,
                      VMStackPtr *            out_ptr)
{
	BL_ASSERT(fn);

	if (!fn->fully_analyzed)
		BL_ABORT("Function is not fully analyzed for "
		         "compile time execution!!!");

	MirType *           ret_type = fn->type->data.fn.ret_type;
	TSmallArray_ArgPtr *args     = fn->type->data.fn.args;

	const bool does_return_value    = ret_type->kind != MIR_TYPE_VOID;
	const bool is_return_value_used = call ? call->ref_count > 1 : true;
	const bool is_caller_comptime   = call ? call->value.is_comptime : false;
	const bool pop_return_value =
	    does_return_value && is_return_value_used && !is_caller_comptime;
	const usize argc = args ? args->size : 0;

	if (args) {
		BL_ASSERT(!call && "Caller instruction cannot be used when "
		                   "call arguments are passed explicitly.");

		BL_ASSERT(argc == args->size && "Invalid count of eplicitly passed arguments");

		/* Push all arguments in reverse order on the stack.
		 */
		for (usize i = argc; i-- > 0;) {
			VMStackPtr dest_ptr =
			    push_stack_empty(vm, MAIN_THREAD_STACK_INDEX, args->data[i]->type);
			copy_comptime_to_stack(vm, dest_ptr, &arg_values->data[i]);
		}
	}

	/* push terminal frame on stack */
	push_ra(vm, MAIN_THREAD_STACK_INDEX, call);

	/* allocate local variables */
	stack_alloc_local_vars(vm, MAIN_THREAD_STACK_INDEX, fn);

	/* setup entry instruction */
	set_pc(vm, MAIN_THREAD_STACK_INDEX, fn->first_block->entry_instr);

	/* iterate over entry block of executable */
	MirInstr *instr, *prev;
	while (true) {
		instr = stack_get_pc(vm, MAIN_THREAD_STACK_INDEX);
		prev  = instr;
		if (!instr || stack_is_aborted(vm, MAIN_THREAD_STACK_INDEX)) break;

		interp_instr(vm, instr);

		/* stack head can be changed by br instructions */
		if (!stack_get_pc(vm, MAIN_THREAD_STACK_INDEX) ||
		    stack_get_pc(vm, MAIN_THREAD_STACK_INDEX) == prev)
			set_pc(vm, MAIN_THREAD_STACK_INDEX, instr->next);
	}

	if (stack_is_aborted(vm, MAIN_THREAD_STACK_INDEX)) return false;

	if (pop_return_value) {
		VMStackPtr ret_ptr = pop_stack(vm, MAIN_THREAD_STACK_INDEX, ret_type);
		if (out_ptr) (*out_ptr) = ret_ptr;
	} else if (is_caller_comptime) {
		if (out_ptr) (*out_ptr) = (VMStackPtr)&call->value.data;
	}

	return true;
}

void
calculate_binop(MirConstValueData *out,
                MirConstValueData *l,
                MirConstValueData *r,
                BinopKind          op,
                MirType *          type)
{
	/******************************************************************************************/
#define _BINOP_INT(T)                                                                              \
	case BINOP_ADD:                                                                            \
		out->v_##T = l->v_##T + r->v_##T;                                                  \
		break;                                                                             \
	case BINOP_SUB:                                                                            \
		out->v_##T = l->v_##T - r->v_##T;                                                  \
		break;                                                                             \
	case BINOP_MUL:                                                                            \
		out->v_##T = l->v_##T * r->v_##T;                                                  \
		break;                                                                             \
	case BINOP_DIV:                                                                            \
		BL_ASSERT(r->v_##T != 0 && "divide by zero, this should be an error");             \
		out->v_##T = l->v_##T / r->v_##T;                                                  \
		break;                                                                             \
	case BINOP_EQ:                                                                             \
		out->v_bool = l->v_##T == r->v_##T;                                                \
		break;                                                                             \
	case BINOP_NEQ:                                                                            \
		out->v_bool = l->v_##T != r->v_##T;                                                \
		break;                                                                             \
	case BINOP_LESS:                                                                           \
		out->v_bool = l->v_##T < r->v_##T;                                                 \
		break;                                                                             \
	case BINOP_LESS_EQ:                                                                        \
		out->v_bool = l->v_##T == r->v_##T;                                                \
		break;                                                                             \
	case BINOP_GREATER:                                                                        \
		out->v_bool = l->v_##T > r->v_##T;                                                 \
		break;                                                                             \
	case BINOP_GREATER_EQ:                                                                     \
		out->v_bool = l->v_##T >= r->v_##T;                                                \
		break;
	/******************************************************************************************/

	/******************************************************************************************/
#define BINOP_CASE_INT(T)                                                                          \
	case sizeof(l->v_##T): {                                                                   \
		switch (op) {                                                                      \
			_BINOP_INT(T);                                                             \
		case BINOP_SHR:                                                                    \
			out->v_##T = l->v_##T >> r->v_##T;                                         \
			break;                                                                     \
		case BINOP_SHL:                                                                    \
			out->v_##T = l->v_##T << r->v_##T;                                         \
			break;                                                                     \
		case BINOP_MOD:                                                                    \
			out->v_##T = l->v_##T % r->v_##T;                                          \
			break;                                                                     \
		case BINOP_AND:                                                                    \
			out->v_##T = l->v_##T & r->v_##T;                                          \
			break;                                                                     \
		case BINOP_OR:                                                                     \
			out->v_##T = l->v_##T | r->v_##T;                                          \
			break;                                                                     \
		default:                                                                           \
			BL_UNIMPLEMENTED;                                                          \
		}                                                                                  \
	} break;
	/******************************************************************************************/

	/******************************************************************************************/
#define BINOP_CASE_REAL(T)                                                                         \
	case sizeof(l->v_##T): {                                                                   \
		switch (op) {                                                                      \
			_BINOP_INT(T) default : BL_UNIMPLEMENTED;                                  \
		}                                                                                  \
	} break;
	/******************************************************************************************/

	const usize s = type->store_size_bytes;

	switch (type->kind) {
	case MIR_TYPE_ENUM:
	case MIR_TYPE_PTR:
	case MIR_TYPE_NULL:
	case MIR_TYPE_BOOL:
	case MIR_TYPE_INT: {
		if (type->data.integer.is_signed) {
			switch (s) {
				BINOP_CASE_INT(s8);
				BINOP_CASE_INT(s16);
				BINOP_CASE_INT(s32);
				BINOP_CASE_INT(s64);
			default:
				BL_ABORT("invalid integer data type");
			}
		} else {
			switch (s) {
				BINOP_CASE_INT(u8);
				BINOP_CASE_INT(u16);
				BINOP_CASE_INT(u32);
				BINOP_CASE_INT(u64);
			default:
				BL_ABORT("invalid integer data type");
			}
		}
		break;
	}

	case MIR_TYPE_REAL: {
		switch (s) {
			BINOP_CASE_REAL(f32);
			BINOP_CASE_REAL(f64);
		default:
			BL_ABORT("invalid real data type");
		}
		break;
	}

	default:
		BL_ABORT("invalid binop type");
	}

#undef BINOP_CASE_INT
#undef BINOP_CASE_REAL
#undef _BINOP_INT
}

void
calculate_unop(MirConstValueData *out, MirConstValueData *v, UnopKind op, MirType *type)
{
	/******************************************************************************************/
#define UNOP_CASE(T)                                                                               \
	case sizeof(v->v_##T): {                                                                   \
		switch (op) {                                                                      \
		case UNOP_NOT:                                                                     \
			out->v_##T = !v->v_##T;                                                    \
			break;                                                                     \
		case UNOP_NEG:                                                                     \
			out->v_##T = v->v_##T * -1;                                                \
			break;                                                                     \
		case UNOP_POS:                                                                     \
			out->v_##T = v->v_##T;                                                     \
			break;                                                                     \
		default:                                                                           \
			BL_UNIMPLEMENTED;                                                          \
		}                                                                                  \
		break;                                                                             \
	};
	/******************************************************************************************/

	switch (type->kind) {
	case MIR_TYPE_BOOL:
	case MIR_TYPE_INT: {
		const usize s = type->store_size_bytes;
		if (type->data.integer.is_signed) {
			switch (s) {
				UNOP_CASE(s8);
				UNOP_CASE(s16);
				UNOP_CASE(s32);
				UNOP_CASE(s64);
			default:
				BL_ABORT("invalid integer data type");
			}
		} else {
			switch (s) {
				UNOP_CASE(u8);
				UNOP_CASE(u16);
				UNOP_CASE(u32);
				UNOP_CASE(u64);
			default:
				BL_ABORT("invalid integer data type");
			}
		}
		break;
	}

	case MIR_TYPE_REAL: {
		const usize s = type->store_size_bytes;

		switch (s) {
			UNOP_CASE(f32);
			UNOP_CASE(f64);
		default:
			BL_ABORT("invalid real data type");
		}
		break;
	}

	default:
		BL_ABORT("invalid unop type");
	}
#undef unop
}

void
make_cast(MirConstValueData *dest,
          MirConstValueData *src,
          MirCastOp          op,
          MirType *          dest_type,
          MirType *          src_type)
{
	switch (op) {
	case MIR_CAST_INTTOPTR:
	case MIR_CAST_PTRTOINT:
	case MIR_CAST_NONE:
	case MIR_CAST_BITCAST:
	case MIR_CAST_ZEXT:
	case MIR_CAST_TRUNC:
		*dest = *src;
		break;

	case MIR_CAST_SEXT: {
		/* src is smaller than dest */
		switch (src_type->store_size_bytes) {
		case sizeof(src->v_s8):
			dest->v_s64 = (s64)src->v_s8;
			break;

		case sizeof(src->v_s16):
			dest->v_s64 = (s64)src->v_s16;
			break;

		case sizeof(src->v_s32):
			dest->v_s64 = (s64)src->v_s32;
			break;

		default:
			BL_ABORT("Invalid sext cast!");
		}
		break;
	}

	case MIR_CAST_FPEXT: {
		/* src is smaller than dest */
		dest->v_f64 = (f64)src->v_f32;
		break;
	}

	case MIR_CAST_FPTRUNC: {
		/* src is bigger than dest */
		dest->v_f32 = (f32)src->v_f64;
		break;
	}

	case MIR_CAST_FPTOSI: {
		/* real to signed integer */
		if (src_type->store_size_bytes == sizeof(f32))
			dest->v_s32 = (s32)src->v_f32;
		else
			dest->v_s64 = (s64)src->v_f64;

		break;
	}

	case MIR_CAST_FPTOUI: {
		/* real to signed integer */
		if (src_type->store_size_bytes == sizeof(f32))
			dest->v_u64 = (u64)src->v_f32;
		else
			dest->v_u64 = (u64)src->v_f64;

		break;
	}

	case MIR_CAST_SITOFP: {
		if (dest_type->store_size_bytes == sizeof(f32)) {
			switch (src_type->store_size_bytes) {
			case sizeof(src->v_s8):
				dest->v_f32 = (f32)src->v_s8;
				break;
			case sizeof(src->v_s16):
				dest->v_f32 = (f32)src->v_s16;
				break;
			case sizeof(src->v_s32):
				dest->v_f32 = (f32)src->v_s32;
				break;
			case sizeof(src->v_s64):
				dest->v_f32 = (f32)src->v_s64;
				break;
			}
		} else {
			switch (src_type->store_size_bytes) {
			case sizeof(src->v_s8):
				dest->v_f64 = (f64)src->v_s8;
				break;
			case sizeof(src->v_s16):
				dest->v_f64 = (f64)src->v_s16;
				break;
			case sizeof(src->v_s32):
				dest->v_f64 = (f64)src->v_s32;
				break;
			case sizeof(src->v_s64):
				dest->v_f64 = (f64)src->v_s64;
				break;
			}
		}

		break;
	}

	case MIR_CAST_UITOFP: {
		if (dest_type->store_size_bytes == sizeof(f32))
			dest->v_f32 = (f32)src->v_u64;
		else
			dest->v_f64 = (f64)src->v_u64;

		break;
	}

	default:
		BL_ABORT("invalid cast operation");
	}
}

void
interp_instr(VM *vm, MirInstr *instr)
{
	if (!instr) return;
	if (!instr->analyzed) {
		BL_ABORT("Instruction '%s' has not been analyzed!", mir_instr_name(instr));
	}

	if (instr->value.is_comptime) {
		BL_ABORT("Instruction '%s' is comptime!", mir_instr_name(instr));
	}

	switch (instr->kind) {
	case MIR_INSTR_CAST:
		interp_instr_cast(vm, (MirInstrCast *)instr);
		break;
	case MIR_INSTR_ADDROF:
		interp_instr_addrof(vm, (MirInstrAddrof *)instr);
		break;
	case MIR_INSTR_BINOP:
		interp_instr_binop(vm, (MirInstrBinop *)instr);
		break;
	case MIR_INSTR_UNOP:
		interp_instr_unop(vm, (MirInstrUnop *)instr);
		break;
	case MIR_INSTR_CALL:
		interp_instr_call(vm, (MirInstrCall *)instr);
		break;
	case MIR_INSTR_RET:
		interp_instr_ret(vm, (MirInstrRet *)instr);
		break;
	case MIR_INSTR_DECL_VAR:
		interp_instr_decl_var(vm, (MirInstrDeclVar *)instr);
		break;
	case MIR_INSTR_DECL_REF:
		interp_instr_decl_ref(vm, (MirInstrDeclRef *)instr);
		break;
	case MIR_INSTR_DECL_DIRECT_REF:
		interp_instr_decl_direct_ref(vm, (MirInstrDeclDirectRef *)instr);
		break;
	case MIR_INSTR_STORE:
		interp_instr_store(vm, (MirInstrStore *)instr);
		break;
	case MIR_INSTR_LOAD:
		interp_instr_load(vm, (MirInstrLoad *)instr);
		break;
	case MIR_INSTR_BR:
		interp_instr_br(vm, (MirInstrBr *)instr);
		break;
	case MIR_INSTR_COND_BR:
		interp_instr_cond_br(vm, (MirInstrCondBr *)instr);
		break;
	case MIR_INSTR_PHI:
		interp_instr_phi(vm, (MirInstrPhi *)instr);
		break;
	case MIR_INSTR_UNREACHABLE:
		interp_instr_unreachable(vm, (MirInstrUnreachable *)instr);
		break;
	case MIR_INSTR_ARG:
		interp_instr_arg(vm, (MirInstrArg *)instr);
		break;
	case MIR_INSTR_ELEM_PTR:
		interp_instr_elem_ptr(vm, (MirInstrElemPtr *)instr);
		break;
	case MIR_INSTR_MEMBER_PTR:
		interp_instr_member_ptr(vm, (MirInstrMemberPtr *)instr);
		break;
	case MIR_INSTR_VARGS:
		interp_instr_vargs(vm, (MirInstrVArgs *)instr);
		break;
	case MIR_INSTR_TYPE_INFO:
		interp_instr_type_info(vm, (MirInstrTypeInfo *)instr);
		break;
	case MIR_INSTR_COMPOUND:
		interp_instr_compound(vm, NULL, (MirInstrCompound *)instr);
		break;
	case MIR_INSTR_TOANY:
		interp_instr_toany(vm, (MirInstrToAny *)instr);
		break;
	case MIR_INSTR_SWITCH:
		interp_instr_switch(vm, (MirInstrSwitch *)instr);
		break;

	default:
		BL_ABORT("Missing execution for instruction '%s'.", mir_instr_name(instr));
	}
}

void
interp_instr_toany(VM *vm, MirInstrToAny *toany)
{
	MirVar * tmp      = toany->tmp;
	MirVar * expr_tmp = toany->expr_tmp;
	MirType *tmp_type = tmp->value.type;

	VMStackPtr tmp_ptr =
	    read_stack_ptr(vm, MAIN_THREAD_STACK_INDEX, tmp->rel_stack_ptr, tmp->is_in_gscope);

	BL_ASSERT(tmp_ptr);

	/* type_info */
	MirVar *expr_type_rtti = toany->rtti_type->vm_rtti_var_cache;
	if (!expr_type_rtti) {
		expr_type_rtti = assembly_get_rtti(vm->assembly, toany->rtti_type->id.hash);
		toany->rtti_type->vm_rtti_var_cache = expr_type_rtti;
	}
	BL_ASSERT(expr_type_rtti);

	VMStackPtr dest           = tmp_ptr + mir_get_struct_elem_offest(vm->assembly, tmp_type, 0);
	MirType *  type_info_type = mir_get_struct_elem_type(tmp_type, 0);

	VMStackPtr rtti_ptr = read_stack_ptr(vm,
	                                     MAIN_THREAD_STACK_INDEX,
	                                     expr_type_rtti->rel_stack_ptr,
	                                     expr_type_rtti->is_in_gscope);

	memcpy(dest, &rtti_ptr, type_info_type->store_size_bytes);

	VMStackPtr data_ptr = fetch_value(vm, toany->expr);

	/* data */
	dest               = tmp_ptr + mir_get_struct_elem_offest(vm->assembly, tmp_type, 1);
	MirType *data_type = mir_get_struct_elem_type(tmp_type, 1);

	if (!toany->has_data) {
		memset(dest, 0, data_type->store_size_bytes);
	} else if (toany->rtti_type_specification) {
		/* Use type specificaiton as an data value. */
		MirVar *spec_type_rtti = toany->rtti_type_specification->vm_rtti_var_cache;
		if (!spec_type_rtti) {
			spec_type_rtti = assembly_get_rtti(vm->assembly,
			                                   toany->rtti_type_specification->id.hash);
			toany->rtti_type_specification->vm_rtti_var_cache = spec_type_rtti;
		}
		BL_ASSERT(spec_type_rtti);

		VMStackPtr rtti_spec_ptr = read_stack_ptr(vm,
		                                          MAIN_THREAD_STACK_INDEX,
		                                          spec_type_rtti->rel_stack_ptr,
		                                          spec_type_rtti->is_in_gscope);

		memcpy(dest, &rtti_spec_ptr, PTR_SIZE);
	} else if (expr_tmp) { // set data
		VMStackPtr expr_tmp_ptr = read_stack_ptr(
		    vm, MAIN_THREAD_STACK_INDEX, expr_tmp->rel_stack_ptr, expr_tmp->is_in_gscope);

		if (toany->expr->value.is_comptime) {
			copy_comptime_to_stack(vm, expr_tmp_ptr, (MirConstValue *)data_ptr);
		} else {
			memcpy(expr_tmp_ptr, data_ptr, data_type->store_size_bytes);
		}

		memcpy(dest, &expr_tmp_ptr, data_type->store_size_bytes);
	} else {
		memcpy(dest, data_ptr, data_type->store_size_bytes);
	}

	push_stack(vm, MAIN_THREAD_STACK_INDEX, &tmp_ptr, toany->base.value.type);
}

void
interp_instr_phi(VM *vm, MirInstrPhi *phi)
{
	MirInstrBlock *prev_block = stack_get_prev_block(vm, MAIN_THREAD_STACK_INDEX);
	BL_ASSERT(prev_block && "Invalid previous block for phi instruction.");
	BL_ASSERT(phi->incoming_blocks && phi->incoming_values);
	BL_ASSERT(phi->incoming_blocks->size == phi->incoming_values->size);

	const usize c = phi->incoming_values->size;
	BL_ASSERT(c > 0);

	MirInstr *     value = NULL;
	MirInstrBlock *block;
	for (usize i = 0; i < c; ++i) {
		value = phi->incoming_values->data[i];
		block = (MirInstrBlock *)phi->incoming_blocks->data[i];

		if (block->base.id == prev_block->base.id) break;
	}

	BL_ASSERT(value && "Invalid value for phi income.");

	/* Pop used value from stack or use constant. Result will be
	 * pushed on the stack or used as constant value of phi when
	 * phi is compile time known constant. */
	MirType *phi_type = phi->base.value.type;
	BL_ASSERT(phi_type);

	VMStackPtr value_ptr = fetch_value(vm, value);

	if (phi->base.value.is_comptime) {
		memcpy(&phi->base.value.data, value_ptr, sizeof(phi->base.value.data));
	} else {
		push_stack(vm, MAIN_THREAD_STACK_INDEX, value_ptr, phi_type);
	}
}

void
interp_instr_addrof(VM *vm, MirInstrAddrof *addrof)
{
	MirInstr *src  = addrof->src;
	MirType * type = src->value.type;
	BL_ASSERT(type);

	if (src->kind == MIR_INSTR_ELEM_PTR || src->kind == MIR_INSTR_COMPOUND) {
		/* address of the element is already on the stack */
		return;
	}

	VMStackPtr ptr = fetch_value(vm, src);
	ptr            = ((MirConstValueData *)ptr)->v_ptr.data.stack_ptr;
	push_stack(vm, MAIN_THREAD_STACK_INDEX, (VMStackPtr)&ptr, type);
}

void
interp_instr_type_info(VM *vm, MirInstrTypeInfo *type_info)
{
	// HACK: cleanup stack
	fetch_value(vm, type_info->expr);

	MirVar *type_info_var = type_info->expr_type->vm_rtti_var_cache;
	if (!type_info_var) {
		type_info_var = assembly_get_rtti(vm->assembly, type_info->expr_type->id.hash);
		type_info->expr_type->vm_rtti_var_cache = type_info_var;
	}
	BL_ASSERT(type_info_var);

	MirType *type = type_info->base.value.type;
	BL_ASSERT(type);

	VMStackPtr ptr = read_stack_ptr(
	    vm, MAIN_THREAD_STACK_INDEX, type_info_var->rel_stack_ptr, type_info_var->is_in_gscope);

	push_stack(vm, MAIN_THREAD_STACK_INDEX, (VMStackPtr)&ptr, type);
}

void
interp_instr_elem_ptr(VM *vm, MirInstrElemPtr *elem_ptr)
{
	/* pop index from stack */
	BL_ASSERT(mir_is_pointer_type(elem_ptr->arr_ptr->value.type));
	MirType *  arr_type   = mir_deref_type(elem_ptr->arr_ptr->value.type);
	MirType *  index_type = elem_ptr->index->value.type;
	VMStackPtr index_ptr  = fetch_value(vm, elem_ptr->index);
	VMStackPtr arr_ptr    = fetch_value(vm, elem_ptr->arr_ptr);
	arr_ptr               = ((MirConstValueData *)arr_ptr)->v_ptr.data.stack_ptr;

	VMStackPtr result = NULL;
	BL_ASSERT(arr_ptr && index_ptr);

	MirConstValueData index = {0};
	read_value(&index, index_ptr, index_type);

	/* Slice */
	if (elem_ptr->target_is_slice) {
		BL_ASSERT(!elem_ptr->arr_ptr->value.is_comptime &&
		          "Missing comptime elem_ptr interpretation!");
		MirType *len_type = mir_get_struct_elem_type(arr_type, MIR_SLICE_LEN_INDEX);
		MirType *ptr_type = mir_get_struct_elem_type(arr_type, MIR_SLICE_PTR_INDEX);

		MirType *elem_type = mir_deref_type(ptr_type);
		BL_ASSERT(elem_type);

		MirConstValueData ptr_tmp = {0};
		MirConstValueData len_tmp = {0};
		const ptrdiff_t   len_member_offset =
		    mir_get_struct_elem_offest(vm->assembly, arr_type, 0);
		const ptrdiff_t ptr_member_offset =
		    mir_get_struct_elem_offest(vm->assembly, arr_type, 1);

		VMStackPtr ptr_ptr = arr_ptr + ptr_member_offset;
		VMStackPtr len_ptr = arr_ptr + len_member_offset;

		read_value(&ptr_tmp, ptr_ptr, ptr_type);
		read_value(&len_tmp, len_ptr, len_type);

		if (!ptr_tmp.v_ptr.data.stack_ptr) {
			msg_error("Dereferencing null pointer! "
			          "Slice has not been set?");
			exec_abort(vm, 0);
		}

		BL_ASSERT(len_tmp.v_s64 > 0);

		if (index.v_s64 >= len_tmp.v_s64) {
			msg_error("Array index is out of the bounds! "
			          "Array index is: %lli, but "
			          "array size is: %lli",
			          (long long)index.v_s64,
			          (long long)len_tmp.v_s64);
			exec_abort(vm, 0);
		}

		result = (VMStackPtr)((ptr_tmp.v_ptr.data.stack_ptr) +
		                      (index.v_u64 * elem_type->store_size_bytes));

		/* push result address on the stack */
		push_stack(vm, MAIN_THREAD_STACK_INDEX, &result, elem_ptr->base.value.type);
		return;
	}

	/* Array */
	MirType *elem_type = arr_type->data.array.elem_type;
	BL_ASSERT(elem_type);

	{ /* Check ranges. */
		const s64 len = arr_type->data.array.len;
		if (index.v_s64 >= len) {
			msg_error("Array index is out of the "
			          "bounds! Array index "
			          "is: %lli, "
			          "but array size "
			          "is: %lli",
			          (long long)index.v_s64,
			          (long long)len);
			exec_abort(vm, 0);
		}
	}

	result = (VMStackPtr)((arr_ptr) + (index.v_u64 * elem_type->store_size_bytes));

	/* push result address on the stack */
	push_stack(vm, MAIN_THREAD_STACK_INDEX, &result, elem_ptr->base.value.type);
}

void
interp_instr_member_ptr(VM *vm, MirInstrMemberPtr *member_ptr)
{
	BL_ASSERT(member_ptr->target_ptr);
	MirType *  target_type = member_ptr->target_ptr->value.type;
	const bool comptime    = member_ptr->base.value.is_comptime;

	/* fetch address of the struct begin */
	VMStackPtr ptr = fetch_value(vm, member_ptr->target_ptr);
	ptr            = ((MirConstValueData *)ptr)->v_ptr.data.stack_ptr;
	BL_ASSERT(ptr);

	if (mir_is_pointer_type(target_type)) target_type = mir_deref_type(target_type);

	switch (target_type->kind) {
	case MIR_TYPE_TYPE: {
		/* This is valid only for enum types. We try to get
		 * one of enum's valiants. Variants
		 * are parts of the enum type passed here as the
		 * actual value of target_ptr. */
		BL_ASSERT(comptime);
		BL_ASSERT(member_ptr->scope_entry &&
		          member_ptr->scope_entry->kind == SCOPE_ENTRY_VARIANT);

		/* Just copy the varaint const expression value. */
		MirVariant *variant         = member_ptr->scope_entry->data.variant;
		member_ptr->base.value.data = variant->value->data;

		/* No push is needed, member_ptr is guaranteed to be
		 * comptime. */
		break;
	}

	case MIR_TYPE_VARGS:
	case MIR_TYPE_STRING:
	case MIR_TYPE_SLICE:
	case MIR_TYPE_STRUCT: {
		BL_ASSERT(member_ptr->scope_entry &&
		          member_ptr->scope_entry->kind == SCOPE_ENTRY_MEMBER);

		VMStackPtr result = NULL;
		MirMember *member = member_ptr->scope_entry->data.member;
		BL_ASSERT(member);

		const s64 index = member->index;

		if (comptime) {
			MirConstValue *ptr_val = (MirConstValue *)ptr;
			result = (VMStackPtr)ptr_val->data.v_struct.members->data[index];
			mir_set_const_ptr(&member_ptr->base.value.data.v_ptr, result, MIR_CP_VALUE);
			break;
		}

		/* let the llvm solve poiner offest */
		const ptrdiff_t ptr_offset =
		    mir_get_struct_elem_offest(vm->assembly, target_type, (u32)index);

		result = ptr + ptr_offset; // pointer shift

		/* push result address on the stack */
		push_stack(vm, MAIN_THREAD_STACK_INDEX, &result, member_ptr->base.value.type);
		break;
	}

	case MIR_TYPE_ARRAY: {
		BL_ASSERT(!comptime && "Builtin on comptime is not implemented yet!");

		VMStackPtr result = NULL;
		switch (member_ptr->builtin_id) {
		case MIR_BUILTIN_ID_ARR_PTR: {
			/* array .ptr */
			const ptrdiff_t ptr_offset =
			    mir_get_struct_elem_offest(vm->assembly, target_type, 1);
			result = ptr + ptr_offset; // pointer shift
			break;
		}

		case MIR_BUILTIN_ID_ARR_LEN: {
			/* array .len*/
			const ptrdiff_t len_offset =
			    mir_get_struct_elem_offest(vm->assembly, target_type, 0);
			result = ptr + len_offset; // pointer shift
			break;
		}

		default:
			BL_ABORT("invalid slice member!");
		}

		if (comptime) {
			/* INCOMPLETE */
		} else {
			/* push result address on the stack */
			push_stack(
			    vm, MAIN_THREAD_STACK_INDEX, &result, member_ptr->base.value.type);
		}
		break;
	}

	default:
		BL_ABORT("Invalid member_ptr target type!");
	}
}

void
interp_instr_unreachable(VM *vm, MirInstrUnreachable *unr)
{
	msg_error("execution reached unreachable code");
	exec_abort(vm, 0);
}

void
interp_instr_br(VM *vm, MirInstrBr *br)
{
	BL_ASSERT(br->then_block);
	stack_set_prev_block(vm, MAIN_THREAD_STACK_INDEX, br->base.owner_block);
	set_pc(vm, MAIN_THREAD_STACK_INDEX, br->then_block->entry_instr);
}

void
interp_instr_switch(VM *vm, MirInstrSwitch *sw)
{
	VMStackPtr value_ptr = fetch_value(vm, sw->value);
	BL_ASSERT(value_ptr);

	MirConstValueData value = {0};
	read_value(&value, value_ptr, sw->value->value.type);

	stack_set_prev_block(vm, MAIN_THREAD_STACK_INDEX, sw->base.owner_block);

	/* PERFORMANCE: We can speed this up little bit by ordering
	 * cases by value. */
	/* PERFORMANCE: We can speed this up little bit by ordering
	 * cases by value. */
	/* PERFORMANCE: We can speed this up little bit by ordering
	 * cases by value. */
	TSmallArray_SwitchCase *cases = sw->cases;
	for (usize i = 0; i < cases->size; ++i) {
		MirSwitchCase *c = &cases->data[i];
		if (value.v_s64 == c->on_value->value.data.v_s64) {
			set_pc(vm, MAIN_THREAD_STACK_INDEX, c->block->entry_instr);
			return;
		}
	}

	set_pc(vm, MAIN_THREAD_STACK_INDEX, sw->default_block->entry_instr);
}

void
interp_instr_cast(VM *vm, MirInstrCast *cast)
{
	MirType *src_type  = cast->expr->value.type;
	MirType *dest_type = cast->base.value.type;

	MirConstValueData src  = {0};
	MirConstValueData dest = {0};

	VMStackPtr src_ptr = fetch_value(vm, cast->expr);
	read_value(&src, src_ptr, src_type);

	make_cast(&dest, &src, cast->op, dest_type, src_type);

	push_stack(vm, MAIN_THREAD_STACK_INDEX, (VMStackPtr)&dest, dest_type);
}

void
interp_instr_arg(VM *vm, MirInstrArg *arg)
{
	/* Caller is optional, when we call function implicitly
	 * there is no call instruction which we can use, so we need
	 * to handle also this situation. In such case we expect all
	 * arguments to be already pushed on the stack. */
	MirInstrCall *caller = (MirInstrCall *)stack_get_ra(vm, MAIN_THREAD_STACK_INDEX)->caller;

	if (caller) {
		TSmallArray_InstrPtr *arg_values = caller->args;
		BL_ASSERT(arg_values);
		MirInstr * curr_arg_value = arg_values->data[arg->i];
		VMStackPtr arg_ptr        = NULL;

		if (curr_arg_value->value.is_comptime) {
			/* Push pointer do comptime data to the
			 * stack. */
			arg_ptr = fetch_comptime_tmp(vm, &curr_arg_value->value);
		} else {
			/* Arguments are located in reverse order
			 * right before return address on the stack
			 * so we can find them inside loop adjusting
			 * address up on the stack. */
			MirInstr *arg_value = NULL;
			/* starting point */
			arg_ptr = (VMStackPtr)stack_get_ra(vm, MAIN_THREAD_STACK_INDEX);
			for (u32 i = 0; i <= arg->i; ++i) {
				arg_value = arg_values->data[i];
				BL_ASSERT(arg_value);
				if (arg_value->value.is_comptime) continue;
				arg_ptr -=
				    stack_alloc_size(arg_value->value.type->store_size_bytes);
			}
		}

		push_stack(vm, MAIN_THREAD_STACK_INDEX, (VMStackPtr)arg_ptr, arg->base.value.type);

		return;
	}

	/* Caller instruction not specified!!! */
	MirFn *fn = arg->base.owner_block->owner_fn;
	BL_ASSERT(fn && "Arg instruction cannot determinate current function");

	/* All arguments must be already on the stack in reverse
	 * order. */
	TSmallArray_ArgPtr *args = fn->type->data.fn.args;
	BL_ASSERT(args && "Function has no arguments");

	/* starting point */
	VMStackPtr arg_ptr = (VMStackPtr)stack_get_ra(vm, MAIN_THREAD_STACK_INDEX);
	for (u32 i = 0; i <= arg->i; ++i) {
		arg_ptr -= stack_alloc_size(args->data[i]->type->store_size_bytes);
	}

	push_stack(vm, MAIN_THREAD_STACK_INDEX, (VMStackPtr)arg_ptr, arg->base.value.type);
}

void
interp_instr_cond_br(VM *vm, MirInstrCondBr *br)
{
	BL_ASSERT(br->cond);
	MirType *type = br->cond->value.type;

	/* pop condition from stack */
	VMStackPtr cond = fetch_value(vm, br->cond);
	BL_ASSERT(cond);

	MirConstValueData tmp = {0};
	read_value(&tmp, cond, type);

	/* Set previous block. */
	stack_set_prev_block(vm, MAIN_THREAD_STACK_INDEX, br->base.owner_block);
	if (tmp.v_s64) {
		set_pc(vm, MAIN_THREAD_STACK_INDEX, br->then_block->entry_instr);
	} else {
		set_pc(vm, MAIN_THREAD_STACK_INDEX, br->else_block->entry_instr);
	}
}

void
interp_instr_decl_ref(VM *vm, MirInstrDeclRef *ref)
{
	ScopeEntry *entry = ref->scope_entry;
	BL_ASSERT(entry);

	switch (entry->kind) {
	case SCOPE_ENTRY_VAR: {
		MirVar *var = entry->data.var;
		BL_ASSERT(var);

		if (var->is_comptime) {
			BL_UNIMPLEMENTED;
		} else {
			const bool use_static_segment = var->is_in_gscope;
			VMStackPtr ptr                = read_stack_ptr(
                            vm, MAIN_THREAD_STACK_INDEX, var->rel_stack_ptr, use_static_segment);
			push_stack(vm, MAIN_THREAD_STACK_INDEX, &ptr, ref->base.value.type);
		}

		break;
	}

	default:
		BL_ABORT("Invalid runtime declaration reference.");
	}
}

void
interp_instr_decl_direct_ref(VM *vm, MirInstrDeclDirectRef *ref)
{
	BL_ASSERT(ref->ref->kind == MIR_INSTR_DECL_VAR);
	MirVar *var = ((MirInstrDeclVar *)ref->ref)->var;
	BL_ASSERT(var);

	const bool use_static_segment = var->is_in_gscope;
	VMStackPtr real_ptr           = NULL;
	real_ptr =
	    read_stack_ptr(vm, MAIN_THREAD_STACK_INDEX, var->rel_stack_ptr, use_static_segment);

	push_stack(vm, MAIN_THREAD_STACK_INDEX, &real_ptr, ref->base.value.type);
}

void
interp_instr_compound(VM *vm, VMStackPtr tmp_ptr, MirInstrCompound *cmp)
{
	if (cmp->base.value.is_comptime) {
		/* non-naked */
		if (tmp_ptr) copy_comptime_to_stack(vm, tmp_ptr, &cmp->base.value);
		return;
	}

	const bool will_push = tmp_ptr == NULL;
	if (will_push) {
		BL_ASSERT(cmp->tmp_var && "Missing temp variable for compound.");
		tmp_ptr = read_stack_ptr(vm,
		                         MAIN_THREAD_STACK_INDEX,
		                         cmp->tmp_var->rel_stack_ptr,
		                         cmp->tmp_var->is_in_gscope);
	}

	BL_ASSERT(tmp_ptr);

	MirType *  type = cmp->base.value.type;
	MirType *  elem_type;
	VMStackPtr elem_ptr = tmp_ptr;

	MirInstr *value;
	TSA_FOREACH(cmp->values, value)
	{
		elem_type = value->value.type;
		switch (type->kind) {

		case MIR_TYPE_STRING:
		case MIR_TYPE_SLICE:
		case MIR_TYPE_VARGS:
		case MIR_TYPE_STRUCT:
			elem_ptr = tmp_ptr + mir_get_struct_elem_offest(vm->assembly, type, (u32)i);
			break;

		case MIR_TYPE_ARRAY:
			elem_ptr = tmp_ptr + mir_get_array_elem_offset(type, (u32)i);
			break;

		default:
			BL_ASSERT(i == 0 && "Invalid elem count for "
			                    "non-agregate type!!!");
		}

		if (value->value.is_comptime) {
			copy_comptime_to_stack(vm, elem_ptr, &value->value);
		} else {
			if (value->kind == MIR_INSTR_COMPOUND) {
				interp_instr_compound(vm, elem_ptr, (MirInstrCompound *)value);
			} else {
				VMStackPtr value_ptr = fetch_value(vm, value);
				memcpy(elem_ptr, value_ptr, elem_type->store_size_bytes);
			}
		}
	}

	if (will_push) push_stack(vm, MAIN_THREAD_STACK_INDEX, tmp_ptr, cmp->base.value.type);
}

void
interp_instr_vargs(VM *vm, MirInstrVArgs *vargs)
{
	TSmallArray_InstrPtr *values    = vargs->values;
	MirVar *              arr_tmp   = vargs->arr_tmp;
	MirVar *              vargs_tmp = vargs->vargs_tmp;

	BL_ASSERT(vargs_tmp->value.type->kind == MIR_TYPE_VARGS);
	BL_ASSERT(vargs_tmp->rel_stack_ptr && "Unalocated vargs slice!!!");
	BL_ASSERT(values);

	VMStackPtr arr_tmp_ptr =
	    arr_tmp ? read_stack_ptr(vm, MAIN_THREAD_STACK_INDEX, arr_tmp->rel_stack_ptr, false)
	            : NULL;

	/* Fill vargs tmp array with values from stack or constants.
	 */
	{
		MirInstr * value;
		VMStackPtr value_ptr;
		TSA_FOREACH(values, value)
		{
			const usize value_size = value->value.type->store_size_bytes;
			VMStackPtr  dest       = arr_tmp_ptr + i * value_size;

			if (value->value.is_comptime) {
				copy_comptime_to_stack(vm, dest, &value->value);
			} else {
				value_ptr = fetch_value(vm, value);
				memcpy(dest, value_ptr, value_size);
			}
		}
	}

	/* Push vargs slice on the stack. */
	{
		VMStackPtr vargs_tmp_ptr =
		    read_stack_ptr(vm, MAIN_THREAD_STACK_INDEX, vargs_tmp->rel_stack_ptr, false);
		// set len
		{
			MirConstValueData len_tmp = {0};
			VMStackPtr        len_ptr =
			    vargs_tmp_ptr + mir_get_struct_elem_offest(vm->assembly,
			                                               vargs_tmp->value.type,
			                                               MIR_SLICE_LEN_INDEX);

			MirType *len_type =
			    mir_get_struct_elem_type(vargs_tmp->value.type, MIR_SLICE_LEN_INDEX);

			len_tmp.v_s64 = (s64)values->size;
			memcpy(len_ptr, &len_tmp, len_type->store_size_bytes);
		}

		// set ptr
		{
			MirConstValueData ptr_tmp = {0};
			VMStackPtr        ptr_ptr =
			    vargs_tmp_ptr + mir_get_struct_elem_offest(vm->assembly,
			                                               vargs_tmp->value.type,
			                                               MIR_SLICE_PTR_INDEX);

			MirType *ptr_type =
			    mir_get_struct_elem_type(vargs_tmp->value.type, MIR_SLICE_PTR_INDEX);

			ptr_tmp.v_ptr.data.any = arr_tmp_ptr;
			memcpy(ptr_ptr, &ptr_tmp, ptr_type->store_size_bytes);
		}

		push_stack(vm, MAIN_THREAD_STACK_INDEX, vargs_tmp_ptr, vargs_tmp->value.type);
	}
}

void
interp_instr_decl_var(VM *vm, MirInstrDeclVar *decl)
{
	BL_ASSERT(decl->base.value.type);

	MirVar *var = decl->var;
	BL_ASSERT(var);

	/* compile time known variables cannot be modified and does
	 * not need stack allocated memory, const_value is used
	 * instead
	 *
	 * already allocated variables will never be allocated again
	 * (in case declaration is inside loop body!!!)
	 */
	if (var->is_comptime) return;

	const bool use_static_segment = var->is_in_gscope;

	BL_ASSERT(var->rel_stack_ptr);

	/* initialize variable if there is some init value */
	if (decl->init) {
		VMStackPtr var_ptr = read_stack_ptr(
		    vm, MAIN_THREAD_STACK_INDEX, var->rel_stack_ptr, use_static_segment);
		BL_ASSERT(var_ptr);

		if (decl->init->kind == MIR_INSTR_COMPOUND) {
			/* used compound initialization!!! */
			interp_instr_compound(vm, var_ptr, (MirInstrCompound *)decl->init);
		} else {
			/* read initialization value if there is one
			 */
			VMStackPtr init_ptr = fetch_value(vm, decl->init);
			memcpy(var_ptr, init_ptr, var->value.type->store_size_bytes);
		}
	}
}

void
interp_instr_load(VM *vm, MirInstrLoad *load)
{
	/* pop source from stack or load directly when src is
	 * declaration, push on to stack dereferenced value of
	 * source */
	MirType *dest_type = load->base.value.type;
	BL_ASSERT(dest_type);
	BL_ASSERT(mir_is_pointer_type(load->src->value.type));

	VMStackPtr src_ptr = fetch_value(vm, load->src);

	if (!src_ptr) {
		msg_error("Dereferencing null pointer!");
		exec_abort(vm, 0);
	}

	src_ptr = deref_stack_ptr(src_ptr);
	push_stack(vm, MAIN_THREAD_STACK_INDEX, src_ptr, dest_type);
}

void
interp_instr_store(VM *vm, MirInstrStore *store)
{
	BL_ASSERT(!store->dest->value.is_comptime && "Store destination cannot be comptime value!");
	/* loads destination (in case it is not direct reference to
	 * declaration) and source from stack
	 */
	MirType *src_type = store->src->value.type;
	BL_ASSERT(src_type);

	VMStackPtr dest_ptr = fetch_value(vm, store->dest);
	VMStackPtr src_ptr  = fetch_value(vm, store->src);

	/* Deref pointer to get actual destination location. */
	dest_ptr = deref_stack_ptr(dest_ptr);

	BL_ASSERT(dest_ptr && src_ptr);

	memcpy(dest_ptr, src_ptr, src_type->store_size_bytes);
}

void
interp_instr_call(VM *vm, MirInstrCall *call)
{
	BL_ASSERT(call->callee && call->base.value.type);
	BL_ASSERT(call->callee->value.type);

	VMStackPtr        callee_ptr = fetch_value(vm, call->callee);
	MirConstValueData callee     = {0};

	read_value(&callee, callee_ptr, call->callee->value.type);

	MirFn *fn = callee.v_ptr.data.fn;
	if (fn == NULL) {
		msg_error("Function pointer not set!");
		exec_abort(vm, 0);
		return;
	}

	BL_ASSERT(fn->type);

	if (IS_FLAG(fn->flags, FLAG_EXTERN)) {
		interp_extern_call(vm, fn, call);
	} else {
		/* Push current frame stack top. (Later poped by ret
		 * instruction)*/
		push_ra(vm, MAIN_THREAD_STACK_INDEX, &call->base);
		BL_ASSERT(fn->first_block->entry_instr);

		stack_alloc_local_vars(vm, MAIN_THREAD_STACK_INDEX, fn);

		/* setup entry instruction */
		set_pc(vm, MAIN_THREAD_STACK_INDEX, fn->first_block->entry_instr);
	}
}

void
interp_instr_ret(VM *vm, MirInstrRet *ret)
{
	MirFn *fn = ret->base.owner_block->owner_fn;
	BL_ASSERT(fn);

	/* read callee from frame stack */
	MirInstrCall *caller   = (MirInstrCall *)stack_get_ra(vm, MAIN_THREAD_STACK_INDEX)->caller;
	MirType *     ret_type = fn->type->data.fn.ret_type;
	VMStackPtr    ret_data_ptr = NULL;

	/* pop return value from stack */
	if (ret->value) {
		ret_data_ptr = fetch_value(vm, ret->value);
		BL_ASSERT(ret_data_ptr);

		if (caller ? caller->base.ref_count == 1 : false) ret_data_ptr = NULL;
	}

	/* do frame stack rollback */
	MirInstr *pc = (MirInstr *)pop_ra(vm, MAIN_THREAD_STACK_INDEX);

	/* clean up all arguments from the stack */
	if (caller) {
		TSmallArray_InstrPtr *arg_values = caller->args;
		if (arg_values) {
			MirInstr *arg_value;
			TSA_FOREACH(arg_values, arg_value)
			{
				if (arg_value->value.is_comptime) continue;
				pop_stack(vm, MAIN_THREAD_STACK_INDEX, arg_value->value.type);
			}
		}
	} else {
		/* When caller was not specified we expect all
		 * arguments to be pushed on the stack so we must
		 * clear them all. Remember they were pushed in
		 * reverse order, so now we have to pop them in
		 * order they are defined. */

		TSmallArray_ArgPtr *args = fn->type->data.fn.args;
		if (args) {
			MirArg *arg;
			TSA_FOREACH(args, arg)
			{
				pop_stack(vm, MAIN_THREAD_STACK_INDEX, arg->type);
			}
		}
	}

	/* push return value on the stack if there is one */
	if (ret_data_ptr) {
		/* CLEANUP: interpreted ret will never be comptime
		 */
		/* CLEANUP: interpreted ret will never be comptime
		 */
		/* CLEANUP: interpreted ret will never be comptime
		 */
		/* CLEANUP: interpreted ret will never be comptime
		 */

		/* Determinate if caller instruction is comptime, if
		 * caller does not exist we are
		 * going to push result on the stack. */
		const bool is_caller_comptime = caller ? caller->base.value.is_comptime : false;

		if (is_caller_comptime) {
			if (ret->value->value.is_comptime) {
				caller->base.value.data = ret->value->value.data;
			} else {
				read_value(&caller->base.value.data, ret_data_ptr, ret_type);
			}
		} else {
			if (ret->value->value.is_comptime) {
				VMStackPtr dest =
				    push_stack_empty(vm, MAIN_THREAD_STACK_INDEX, ret_type);
				copy_comptime_to_stack(vm, dest, &ret->value->value);
			} else {
				push_stack(vm, MAIN_THREAD_STACK_INDEX, ret_data_ptr, ret_type);
			}
		}
	}

	/* set program counter to next instruction */
	pc = pc ? pc->next : NULL;
	set_pc(vm, MAIN_THREAD_STACK_INDEX, pc);
}

void
interp_instr_binop(VM *vm, MirInstrBinop *binop)
{
	/* binop expects lhs and rhs on stack in exact order and
	 * push result again to the stack */
	MirType *  type    = binop->lhs->value.type;
	VMStackPtr lhs_ptr = fetch_value(vm, binop->lhs);
	VMStackPtr rhs_ptr = fetch_value(vm, binop->rhs);

	MirConstValueData result = {0};
	MirConstValueData lhs    = {0};
	MirConstValueData rhs    = {0};

	read_value(&lhs, lhs_ptr, type);
	read_value(&rhs, rhs_ptr, type);

	calculate_binop(&result, &lhs, &rhs, binop->op, type);

	push_stack(vm, MAIN_THREAD_STACK_INDEX, &result, binop->base.value.type);
#undef BINOP_CASE_INT
#undef BINOP_CASE_REAL
#undef _BINOP_INT
}

void
interp_instr_unop(VM *vm, MirInstrUnop *unop)
{
	MirType *  type      = unop->expr->value.type;
	VMStackPtr value_ptr = fetch_value(vm, unop->expr);

	MirConstValueData result = {0};
	MirConstValueData value  = {0};
	read_value(&value, value_ptr, type);

	calculate_unop(&result, &value, unop->op, type);
	push_stack(vm, MAIN_THREAD_STACK_INDEX, &result, type);
}

void
eval_instr(VM *vm, MirInstr *instr)
{
	if (!instr) return;
	if (!instr->analyzed) {
		BL_ABORT("Instruction '%s' has not been analyzed!", mir_instr_name(instr));
	}

	if (!instr->value.is_comptime) {
		BL_ABORT("Evaluated '%s' instruction must be comptime!", mir_instr_name(instr));
	}

	switch (instr->kind) {
	case MIR_INSTR_COMPOUND:
		eval_instr_compound(vm, (MirInstrCompound *)instr);
		break;

	case MIR_INSTR_MEMBER_PTR:
		eval_instr_member_ptr(vm, (MirInstrMemberPtr *)instr);
		break;

	case MIR_INSTR_CALL:
		eval_instr_call(vm, (MirInstrCall *)instr);
		break;

	case MIR_INSTR_LOAD:
		eval_instr_load(vm, (MirInstrLoad *)instr);
		break;

	case MIR_INSTR_SIZEOF:
		eval_instr_sizeof(vm, (MirInstrSizeof *)instr);
		break;

	case MIR_INSTR_ALIGNOF:
		eval_instr_alignof(vm, (MirInstrAlignof *)instr);
		break;

	case MIR_INSTR_ADDROF:
		eval_instr_addrof(vm, (MirInstrAddrof *)instr);
		break;

	case MIR_INSTR_ELEM_PTR:
		eval_instr_elem_ptr(vm, (MirInstrElemPtr *)instr);
		break;

	case MIR_INSTR_BINOP:
		eval_instr_binop(vm, (MirInstrBinop *)instr);
		break;

	case MIR_INSTR_UNOP:
		eval_instr_unop(vm, (MirInstrUnop *)instr);
		break;

	case MIR_INSTR_CAST:
		eval_instr_cast(vm, (MirInstrCast *)instr);
		break;

	case MIR_INSTR_DECL_DIRECT_REF:
		eval_instr_decl_direct_ref(vm, (MirInstrDeclDirectRef *)instr);
		break;

	case MIR_INSTR_DECL_REF:
		eval_instr_decl_ref(vm, (MirInstrDeclRef *)instr);
		break;

	case MIR_INSTR_RET:
		eval_instr_ret(vm, (MirInstrRet *)instr);
		break;

	default:
		BL_ABORT("Missing evaluation for instruction '%s'.", mir_instr_name(instr));
	}
}

void
eval_instr_compound(VM *vm, MirInstrCompound *cmp)
{
	MirType *             type   = cmp->base.value.type;
	TSmallArray_InstrPtr *values = cmp->values;
	BL_ASSERT(type);

	if (cmp->is_zero_initialized) return;

	switch (type->kind) {
	case MIR_TYPE_ARRAY:
		cmp->base.value.data.v_array.elems = (TSmallArray_ConstValuePtr *)values;
		break;

	case MIR_TYPE_STRING:
	case MIR_TYPE_SLICE:
	case MIR_TYPE_VARGS:
	case MIR_TYPE_STRUCT:
		cmp->base.value.data.v_struct.members = (TSmallArray_ConstValuePtr *)values;
		break;

	default:
	        BL_ASSERT(values->size == 1);
                cmp->base.value.data = values->data[0]->value.data;
	}
}

void
eval_instr_call(VM *vm, MirInstrCall *call)
{
	MirFn *callee = get_callee(call);
	BL_ASSERT(callee && "Missing callee for comptile time known call!");
	BL_ASSERT(!IS_FLAG(callee->flags, FLAG_EXTERN) &&
	          "Cannot evaluate comptime call to external fn!");

	/* Push call into caller queue, this is similar operation to
	 * push return address but we don't use stack for comptime
	 * evaluations. */
	eval_push_caller(vm, call);
	MirInstr *entry_block = callee->first_block->entry_instr;
	BL_ASSERT(entry_block && "Missing entry block!");

	eval_instr(vm, entry_block);
}

void
eval_instr_ret(VM *vm, MirInstrRet *ret)
{
	/* Pop current caller from the caller queue so the ret
	 * instruction can fill call value with result data. */
	MirInstrCall *caller = eval_pop_caller(vm);
	BL_ASSERT(caller && "Missing caller!");
	if (!ret->value) return;

	/* Here we just copy result expression value to caller
	 * value. */
	caller->base.value.data = ret->value->value.data;
}

void
eval_instr_member_ptr(VM *vm, MirInstrMemberPtr *member_ptr)
{
	BL_ASSERT(member_ptr->target_ptr);
	MirConstValue *target_value = &member_ptr->target_ptr->value;
	MirType *      target_type  = member_ptr->target_ptr->value.type;

	if (mir_is_pointer_type(target_type)) {
		target_type  = mir_deref_type(target_type);
		target_value = target_value->data.v_ptr.data.any;
	}

	switch (target_type->kind) {
	case MIR_TYPE_TYPE: {
		/* This is valid only for enum types. We try to get
		 * one of enum's valiants. Variants
		 * are parts of the enum type passed here as the
		 * actual value of target_ptr. */
		BL_ASSERT(member_ptr->scope_entry &&
		          member_ptr->scope_entry->kind == SCOPE_ENTRY_VARIANT);

		/* Just copy the varaint const expression value. */
		MirVariant *variant         = member_ptr->scope_entry->data.variant;
		member_ptr->base.value.data = variant->value->data;
		break;
	}

	case MIR_TYPE_VARGS:
	case MIR_TYPE_STRING:
	case MIR_TYPE_SLICE:
	case MIR_TYPE_STRUCT: {
		BL_ASSERT(member_ptr->scope_entry &&
		          member_ptr->scope_entry->kind == SCOPE_ENTRY_MEMBER);

		MirMember *member = member_ptr->scope_entry->data.member;
		BL_ASSERT(member);
		const s64 index = member->index;

		MirConstValue *result = target_value->data.v_struct.members->data[index];
		mir_set_const_ptr(&member_ptr->base.value.data.v_ptr, result, MIR_CP_VALUE);
		break;
	}

	default:
		BL_ABORT("Invalid member_ptr target type!");
	}
}

void
eval_instr_elem_ptr(VM *vm, MirInstrElemPtr *elem_ptr)
{
	/* This is more complicated, element pointer is constant
	 * expression only when target pointer and index are
	 * constants. */
	BL_ASSERT(mir_is_pointer_type(elem_ptr->arr_ptr->value.type));
	BL_ASSERT(elem_ptr->index->value.is_comptime && "Array index must be comptime!");

	MirType *      arr_type      = mir_deref_type(elem_ptr->arr_ptr->value.type);
	MirConstValue *arr_ptr_value = &elem_ptr->arr_ptr->value;
	arr_ptr_value                = mir_get_const_ptr(
            MirConstValue *, &arr_ptr_value->data.v_ptr, MIR_CP_VALUE | MIR_CP_VAR);
	const s64 index = elem_ptr->index->value.data.v_s64;

	if (elem_ptr->target_is_slice) {
		BL_UNIMPLEMENTED;
	}

	/* Array */
	MirType *elem_type = arr_type->data.array.elem_type;
	BL_ASSERT(elem_type);

	MirConstValue *result = arr_ptr_value->data.v_array.elems->data[index];
	mir_set_const_ptr(&elem_ptr->base.value.data.v_ptr, result, MIR_CP_VALUE);
}

void
eval_instr_load(VM *vm, MirInstrLoad *load)
{
	MirConstValue *src_value = &load->src->value;
	src_value =
	    mir_get_const_ptr(MirConstValue *, &src_value->data.v_ptr, MIR_CP_VALUE | MIR_CP_VAR);

	load->base.value.data = src_value->data;
}

void
eval_instr_sizeof(VM *vm, MirInstrSizeof *szof)
{
	MirType *type = szof->expr->value.type;
	BL_ASSERT(type);

	if (type->kind == MIR_TYPE_TYPE) {
		type = mir_get_const_ptr(MirType *, &szof->expr->value.data.v_ptr, MIR_CP_TYPE);
		BL_ASSERT(type);
	}

	szof->base.value.data.v_u64 = type->store_size_bytes;
}

void
eval_instr_alignof(VM *vm, MirInstrAlignof *alof)
{
	MirType *type = alof->expr->value.type;
	BL_ASSERT(type);

	if (type->kind == MIR_TYPE_TYPE) {
		type = mir_get_const_ptr(MirType *, &alof->expr->value.data.v_ptr, MIR_CP_TYPE);
		BL_ASSERT(type);
	}

	alof->base.value.data.v_s32 = type->alignment;
}

void
eval_instr_addrof(VM *vm, MirInstrAddrof *addrof)
{
	MirConstValue *val      = &addrof->src->value;
	addrof->base.value.data = val->data;
}

void
eval_instr_binop(VM *vm, MirInstrBinop *binop)
{
	MirType *type = binop->lhs->value.type;

	calculate_binop(&binop->base.value.data,
	                &binop->lhs->value.data,
	                &binop->rhs->value.data,
	                binop->op,
	                type);
}

void
eval_instr_unop(VM *vm, MirInstrUnop *unop)
{
	MirType *type = unop->expr->value.type;
	calculate_unop(&unop->base.value.data, &unop->expr->value.data, unop->op, type);
}

void
eval_instr_cast(VM *vm, MirInstrCast *cast)
{
	MirType *src_type  = cast->expr->value.type;
	MirType *dest_type = cast->base.value.type;

	make_cast(&cast->base.value.data, &cast->expr->value.data, cast->op, dest_type, src_type);
}

void
eval_instr_decl_direct_ref(VM *vm, MirInstrDeclDirectRef *ref)
{
	BL_ASSERT(ref->ref->kind == MIR_INSTR_DECL_VAR);
	MirVar *var = ((MirInstrDeclVar *)ref->ref)->var;
	BL_ASSERT(var);
	mir_set_const_ptr(&ref->base.value.data.v_ptr, &var->value, MIR_CP_VAR);
}

void
eval_instr_decl_ref(VM *vm, MirInstrDeclRef *ref)
{
	ScopeEntry *entry = ref->scope_entry;
	BL_ASSERT(entry && "Missing scope entry for declref");

	switch (entry->kind) {
	case SCOPE_ENTRY_FN:
		mir_set_const_ptr(&ref->base.value.data.v_ptr, entry->data.fn, MIR_CP_FN);
		break;

	case SCOPE_ENTRY_TYPE:
		mir_set_const_ptr(&ref->base.value.data.v_ptr, entry->data.type, MIR_CP_TYPE);
		break;

	case SCOPE_ENTRY_VARIANT:
		mir_set_const_ptr(
		    &ref->base.value.data.v_ptr, entry->data.variant->value, MIR_CP_VALUE);
		break;

	case SCOPE_ENTRY_VAR: {
		MirVar *var = entry->data.var;
		mir_set_const_ptr(&ref->base.value.data.v_ptr, var, MIR_CP_VAR);
		fetch_comptime_tmp(vm, &var->value);
		ref->base.value.comptime_alloc = (VMStackPtr)&var->value.comptime_alloc;
		break;
	}

	default:
		BL_ABORT("invalid scope entry kind");
	}
}

VMStack *
stack_new(usize stack_size)
{
	VMStack *stack = bl_malloc(sizeof(char) * stack_size);
	if (!stack) BL_ABORT("bad alloc");
#if BL_DEBUG
	memset(stack, 0, stack_size);
#endif

	stack->allocated_bytes = stack_size;
	stack->pc              = NULL;
	stack->ra              = NULL;
	stack->prev_block      = NULL;
	stack->aborted         = false;
	const usize size       = stack_alloc_size(sizeof(VMStack));
	stack->used_bytes      = size;
	stack->top_ptr         = (u8 *)stack + size;

	return stack;
}

/* public */
void
vm_init(VM *vm, Assembly *assembly, usize stack_size)
{
	if (stack_size == 0) BL_ABORT("invalid frame stack size");

	vm->stack[TMP_STACK_INDEX]         = stack_new(TMP_STACK_SIZE);
	vm->stack[MAIN_THREAD_STACK_INDEX] = stack_new(stack_size);
	vm->assembly                       = assembly;

	tsa_init(&vm->dyncall_sig_tmp);
	tsa_init(&vm->eval_caller_queue);
}

void
vm_terminate(VM *vm)
{
	tsa_terminate(&vm->eval_caller_queue);
	tsa_terminate(&vm->dyncall_sig_tmp);

	for (usize i = 0; i < TARRAY_SIZE(vm->stack); ++i)
		bl_free(vm->stack[i]);
}

void
vm_execute_instr(VM *vm, MirInstr *instr)
{
	interp_instr(vm, instr);
}

void
vm_eval_comptime_instr(VM *vm, struct MirInstr *instr)
{
	eval_instr(vm, instr);
}

bool
vm_execute_fn(VM *vm, MirFn *fn, VMStackPtr *out_ptr)
{
	vm->stack[MAIN_THREAD_STACK_INDEX]->aborted = false;
	return execute_fn_impl_top_level(vm, fn, NULL, out_ptr);
}

bool
vm_execute_instr_top_level_call(VM *vm, MirInstrCall *call)
{
	BL_ASSERT(call && call->base.analyzed);

	assert(call->base.value.is_comptime && "Top level call is expected to be comptime.");
	if (call->args)
		BL_ABORT("exec call top level has not implemented "
		         "passing of arguments");

	return execute_fn_top_level(vm, &call->base, NULL);
}

VMStackPtr
vm_create_global(VM *vm, struct MirInstrDeclVar *decl)
{
	MirVar *var = decl->var;
	BL_ASSERT(var);
	BL_ASSERT(var->is_in_gscope && "Allocated variable is supposed to be global "
	                               "variable.");

	VMRelStackPtr var_ptr = stack_alloc_var(vm, MAIN_THREAD_STACK_INDEX, var);
	interp_instr_decl_var(vm, decl);

	/* HACK: we can ignore relative pointers for globals. */
	return (VMStackPtr)var_ptr;
}

VMStackPtr
vm_create_implicit_global(VM *vm, struct MirVar *var)
{
	BL_ASSERT(var);
	BL_ASSERT(var->is_in_gscope && "Allocated variable is supposed to be global "
	                               "variable.");

	/* HACK: we can ignore relative pointers for globals. */
	VMStackPtr var_ptr = (VMStackPtr)stack_alloc_var(vm, MAIN_THREAD_STACK_INDEX, var);
	copy_comptime_to_stack(vm, var_ptr, &var->value);
	return var_ptr;
}

void
vm_read_stack_value(MirConstValue *dest, VMStackPtr src)
{
	assert(dest->type);
	memset(&dest->data, 0, sizeof(dest->data));
	read_value(&dest->data, src, dest->type);
}
