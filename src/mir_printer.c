//************************************************************************************************
// bl
//
// File:   mir_printer.c
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

#include "mir_printer.h"
#include "assembly.h"
#include "ast.h"

static void
print_comptime_value_or_id(MirInstr *instr, FILE *stream);

static inline void
print_type(MirType *type, bool aligned, FILE *stream, bool prefer_name)
{
	char tmp[256];
	mir_type_to_str(tmp, TARRAY_SIZE(tmp), type, prefer_name);
	if (aligned)
		fprintf(stream, "%16s", tmp);
	else
		fprintf(stream, "%s", tmp);
}

static inline void
print_instr_head(MirInstr *instr, FILE *stream, const char *name)
{
	if (!instr) return;

#if BL_DEBUG
	if (instr->ref_count == -1) {
		fprintf(stream, "  %%%-6llu (-)", (unsigned long long)instr->id);
	} else {
		fprintf(stream, "  %%%-6llu (%d)", (unsigned long long)instr->id, instr->ref_count);
	}
#else
	fprintf(stream, "  %%%-6llu", (unsigned long long)instr->id);
#endif
	print_type(instr->value.type, true, stream, true);
	fprintf(stream, " %s ", name);
}

static inline void
print_flags(u32 flags, FILE *stream)
{
	if (flags == 0) return;

	if (IS_FLAG(flags, FLAG_EXTERN)) fprintf(stream, "#extern");
	if (IS_FLAG(flags, FLAG_COMPILER)) fprintf(stream, " #compiler");
	if (IS_FLAG(flags, FLAG_TEST)) fprintf(stream, " #test");
	if (IS_FLAG(flags, FLAG_INLINE)) fprintf(stream, " #inline");
	if (IS_FLAG(flags, FLAG_NO_INLINE)) fprintf(stream, " #noinline");
	if (IS_FLAG(flags, FLAG_PRIVATE)) fprintf(stream, " #private");

	fprintf(stream, " ");
}

static inline void
print_const_value(MirConstValue *value, FILE *stream)
{
	MirType *          type = value->type;
	MirConstValueData *data = &value->data;
	if (!type) return;

#define print_case(format, T)                                                                      \
	case sizeof(data->T):                                                                      \
		fprintf(stream, format, data->T);                                                  \
		break;

	BL_ASSERT(type);

	switch (type->kind) {
	case MIR_TYPE_INT: {
		const usize s = type->store_size_bytes;
		if (type->data.integer.is_signed) {
			switch (s) {
			case sizeof(data->v_s8):
				fprintf(stream, "%d", data->v_s8);
				break;
			case sizeof(data->v_s16):
				fprintf(stream, "%d", data->v_s16);
				break;
			case sizeof(data->v_s32):
				fprintf(stream, "%d", data->v_s32);
				break;
			case sizeof(data->v_s64):
				fprintf(stream, "%lld", (long long)data->v_s64);
				break;
			default:
				fprintf(stream, "<cannot read value>");
				break;
			}
		} else {
			switch (s) {
			case sizeof(data->v_s8):
				fprintf(stream, "%u", data->v_u8);
				break;
			case sizeof(data->v_s16):
				fprintf(stream, "%u", data->v_u16);
				break;
			case sizeof(data->v_s32):
				fprintf(stream, "%u", data->v_u32);
				break;
			case sizeof(data->v_s64):
				fprintf(stream, "%llu", (unsigned long long)data->v_u64);
				break;
			default:
				fprintf(stream, "<cannot read value>");
				break;
			}
		}

		break;
	}
	case MIR_TYPE_REAL:
		if (type->store_size_bytes == sizeof(f32)) {
			fprintf(stream, "%f", data->v_f32);
		} else {
			fprintf(stream, "%f", data->v_f64);
		}
		break;
	case MIR_TYPE_BOOL:
		fprintf(stream, "%s", data->v_bool ? "true" : "false");
		break;
	case MIR_TYPE_TYPE:
		print_type(data->v_ptr.data.type, false, stream, false);
		break;
	case MIR_TYPE_ENUM:
		fprintf(stream, "%lld", (long long)data->v_s64);
		break;
	case MIR_TYPE_PTR: {
		MirType *deref_type = mir_deref_type(type);
		/* pointers to u8 is printed like strings */
		if (deref_type->kind == MIR_TYPE_INT && deref_type->data.integer.bitcount == 8 &&
		    deref_type->data.integer.is_signed == false) {
			if (data->v_ptr.data.str == NULL) {
				fprintf(stream, "<null>");
				break;
			}

			char *tmp = strdup(data->v_ptr.data.str);
			if (strtok(tmp, "\n")) {
				fprintf(stream, "\"%s", strtok(tmp, "\n"));
			} else {
				fprintf(stream, "\"\"");
				break;
			}
			char *next = strtok(NULL, "\n");
			if (next && strlen(next)) fprintf(stdout, "...");
			fprintf(stream, "\"");
			free(tmp);
		} else if (deref_type->kind == MIR_TYPE_FN) {
			/* Pointer to function. */
			MirFn *fn = data->v_ptr.data.any
			                ? data->v_ptr.data.value->data.v_ptr.data.fn
			                : NULL;
			if (fn) {
				fprintf(stream,
				        "&%s",
				        fn->linkage_name ? fn->linkage_name : fn->id->str);
			} else {
				fprintf(stream, "<invalid>");
			}
		} else {
			fprintf(stream, "%p", data->v_ptr.data.any);
		}
		break;
	}
	case MIR_TYPE_NULL:
		fprintf(stream, "null_");
		print_type(type->data.null.base_type, false, stream, true);
		break;
	case MIR_TYPE_STRING:
	case MIR_TYPE_SLICE:
	case MIR_TYPE_VARGS:
	case MIR_TYPE_STRUCT: {
		TSmallArray_ConstValuePtr *members             = data->v_struct.members;
		const bool                 is_zero_initializer = data->v_struct.is_zero_initializer;

		if (is_zero_initializer) {
			fprintf(stream, "{zero initialized}");
		} else if (!members) {
			fprintf(stream, "{<null>}");
		} else {
			fprintf(stream, "{");

			MirConstValue *member;
			const usize    memc = members->size;

			for (usize i = 0; i < memc; ++i) {
				member = members->data[i];
				print_const_value(member, stream);
				if (i + 1 < memc) fprintf(stream, ", ");
			}

			fprintf(stream, "}");
		}
		break;
	}
	case MIR_TYPE_ARRAY: {
		TSmallArray_ConstValuePtr *elems               = data->v_array.elems;
		const bool                 is_zero_initializer = data->v_array.is_zero_initializer;

		if (is_zero_initializer) {
			fprintf(stream, "{zero initialized}");
		} else {
			fprintf(stream, "{");

			if (elems) {
				MirConstValue *elem;
				const usize    elc = elems->size;

				for (usize i = 0; i < elc; ++i) {
					elem = elems->data[i];
					print_const_value(elem, stream);
					if (i + 1 < elc) fprintf(stream, ", ");
				}
			} else {
				fprintf(stream, "<cannot read value>");
			}

			fprintf(stream, "}");
		}
		break;
	}
	default:
		fprintf(stream, "<cannot read value>");
	}
}

static void
print_instr_toany(MirInstrToAny *toany, FILE *stream);

static void
print_instr_phi(MirInstrPhi *phi, FILE *stream);

static void
print_instr_cast(MirInstrCast *cast, FILE *stream);

static void
print_instr_sizeof(MirInstrSizeof *szof, FILE *stream);

static void
print_instr_type_info(MirInstrTypeInfo *type_info, FILE *stream);

static void
print_instr_alignof(MirInstrAlignof *szof, FILE *stream);

static void
print_instr_load(MirInstrLoad *load, FILE *stream);

static void
print_instr_addrof(MirInstrAddrof *addrof, FILE *stream);

static void
print_instr_elem_ptr(MirInstrElemPtr *elem_ptr, FILE *stream);

static void
print_instr_member_ptr(MirInstrMemberPtr *member_ptr, FILE *stream);

static void
print_instr_cond_br(MirInstrCondBr *cond_br, FILE *stream);

static void
print_instr_compound(MirInstrCompound *init, FILE *stream);

static void
print_instr_vargs(MirInstrVArgs *vargs, FILE *stream);

static void
print_instr_br(MirInstrBr *br, FILE *stream);

static void
print_instr_switch(MirInstrSwitch *sw, FILE *stream);

static void
print_instr_unreachable(MirInstrUnreachable *unr, FILE *stream);

static void
print_instr_fn_proto(MirInstrFnProto *fn_proto, FILE *stream);

static void
print_instr_type_fn(MirInstrTypeFn *type_fn, FILE *stream);

static void
print_instr_type_struct(MirInstrTypeStruct *type_struct, FILE *stream);

static void
print_instr_type_enum(MirInstrTypeEnum *type_enum, FILE *stream);

static void
print_instr_type_ptr(MirInstrTypePtr *type_ptr, FILE *stream);

static void
print_instr_type_array(MirInstrTypeArray *type_array, FILE *stream);

static void
print_instr_type_slice(MirInstrTypeSlice *type_slice, FILE *stream);

static void
print_instr_type_vargs(MirInstrTypeVArgs *type_vargs, FILE *stream);

static void
print_instr_block(MirInstrBlock *block, FILE *stream);

static void
print_instr_decl_var(MirInstrDeclVar *decl, FILE *stream);

static void
print_instr_decl_member(MirInstrDeclMember *decl, FILE *stream);

static void
print_instr_decl_variant(MirInstrDeclVariant *var, FILE *stream);

static void
print_instr_decl_arg(MirInstrDeclArg *decl, FILE *stream);

static void
print_instr_const(MirInstrConst *ci, FILE *stream);

static void
print_instr_ret(MirInstrRet *ret, FILE *stream);

static void
print_instr_store(MirInstrStore *store, FILE *stream);

static void
print_instr_binop(MirInstrBinop *binop, FILE *stream);

static void
print_instr_call(MirInstrCall *call, FILE *stream);

static void
print_instr_decl_ref(MirInstrDeclRef *ref, FILE *stream);

static void
print_instr_unop(MirInstrUnop *unop, FILE *stream);

static void
print_instr_arg(MirInstrArg *arg, FILE *stream);

/* impl */
void
print_comptime_value_or_id(MirInstr *instr, FILE *stream)
{
	if (!instr) {
		fprintf(stream, "<invalid>");
		return;
	}

	if (instr->kind == MIR_INSTR_COMPOUND && !((MirInstrCompound *)instr)->is_naked) {
		print_const_value(&instr->value, stream);
		return;
	}

	if (instr->eval_mode != MIR_VEM_COMPTIME || !instr->analyzed) {
		fprintf(stream, "%%%llu", (unsigned long long)instr->id);
		return;
	}

	/* Value is compile time known constant. */

	if (instr->kind == MIR_INSTR_DECL_REF) {
		fprintf(stream, "%s", ((MirInstrDeclRef *)instr)->rid->str);
		return;
	}

	/* Comptime pointer */
	if (instr->value.type->kind == MIR_TYPE_PTR) {
		fprintf(stream, "%%%llu /* comptime */", (unsigned long long)instr->id);
		return;
	}

	print_const_value(&instr->value, stream);
}

void
print_instr_type_fn(MirInstrTypeFn *type_fn, FILE *stream)
{
	print_instr_head(&type_fn->base, stream, "const fn");
	fprintf(stream, "(");
	if (type_fn->args) {
		MirInstr *tmp;
		TSA_FOREACH(type_fn->args, tmp)
		{
			fprintf(stream, "%%%llu", (unsigned long long)tmp->id);
			if (i + 1 < type_fn->args->size) fprintf(stream, ", ");
		}
	}

	fprintf(stream, ")");

	if (type_fn->ret_type)
		fprintf(stream, " %%%llu", (unsigned long long)type_fn->ret_type->id);
}

void
print_instr_phi(MirInstrPhi *phi, FILE *stream)
{
	print_instr_head(&phi->base, stream, "phi");

	if (phi->incoming_blocks->size != phi->incoming_values->size) {
		fprintf(stream, "<value_count_does_not_match_block_count>");
		return;
	}

	MirInstr *     value;
	MirInstrBlock *block;
	const usize    c = phi->incoming_values->size;

	if (c == 0) {
		fprintf(stream, "<empty incomes>");
	}

	for (usize i = 0; i < c; ++i) {
		value = phi->incoming_values->data[i];
		block = (MirInstrBlock *)phi->incoming_blocks->data[i];

		fprintf(stream, "[");
		print_comptime_value_or_id(value, stream);
		fprintf(stream, ", ");
		fprintf(stream, "%%%s_%llu", block->name, (unsigned long long)block->base.id);
		fprintf(stream, "] ");
	}
}

void
print_instr_toany(MirInstrToAny *toany, FILE *stream)
{
	print_instr_head(&toany->base, stream, "toany");
	print_comptime_value_or_id(toany->expr, stream);
}

void
print_instr_type_struct(MirInstrTypeStruct *type_struct, FILE *stream)
{
	print_instr_head(&type_struct->base, stream, "const struct");
	fprintf(stream, "{");

	TSmallArray_InstrPtr *members = type_struct->members;
	MirInstr *            member;
	TSA_FOREACH(members, member)
	{
		print_comptime_value_or_id(member, stream);
		if (i + 1 < members->size) fprintf(stream, ", ");
	}

	fprintf(stream, "}");
}

void
print_instr_type_enum(MirInstrTypeEnum *type_enum, FILE *stream)
{
	print_instr_head(&type_enum->base, stream, "const enum");
	fprintf(stream, "{");

	TSmallArray_InstrPtr *variants = type_enum->variants;
	MirInstr *            variant;
	TSA_FOREACH(variants, variant)
	{
		fprintf(stream, "%%%llu", (unsigned long long)variant->id);
		if (i + 1 < variants->size) fprintf(stream, ", ");
	}

	fprintf(stream, "}");
}

void
print_instr_type_ptr(MirInstrTypePtr *type_ptr, FILE *stream)
{
	print_instr_head(&type_ptr->base, stream, "const");
	fprintf(stream, "*%%%llu", (unsigned long long)type_ptr->type->id);
}

void
print_instr_type_array(MirInstrTypeArray *type_array, FILE *stream)
{
	print_instr_head(&type_array->base, stream, "const");
	fprintf(stream,
	        "[%%%llu]%%%llu",
	        (unsigned long long)type_array->len->id,
	        (unsigned long long)type_array->elem_type->id);
}

void
print_instr_type_slice(MirInstrTypeSlice *type_slice, FILE *stream)
{
	print_instr_head(&type_slice->base, stream, "const");
	fprintf(stream, "[]%%%llu", (unsigned long long)type_slice->elem_type->id);
}

void
print_instr_type_vargs(MirInstrTypeVArgs *type_vargs, FILE *stream)
{
	print_instr_head(&type_vargs->base, stream, "const");
	if (!type_vargs->elem_type) return;
	fprintf(stream, "...%%%llu", (unsigned long long)type_vargs->elem_type->id);
}

void
print_instr_cast(MirInstrCast *cast, FILE *stream)
{
	switch (cast->op) {
	case MIR_CAST_NONE:
		print_instr_head(&cast->base, stream, "nocast");
		break;
	case MIR_CAST_BITCAST:
		print_instr_head(&cast->base, stream, "bitcast");
		break;
	case MIR_CAST_SEXT:
		print_instr_head(&cast->base, stream, "sext");
		break;
	case MIR_CAST_ZEXT:
		print_instr_head(&cast->base, stream, "zext");
		break;
	case MIR_CAST_TRUNC:
		print_instr_head(&cast->base, stream, "trunc");
		break;
	case MIR_CAST_FPTOSI:
		print_instr_head(&cast->base, stream, "fptosi");
		break;
	case MIR_CAST_FPTOUI:
		print_instr_head(&cast->base, stream, "fptoui");
		break;
	case MIR_CAST_FPTRUNC:
		print_instr_head(&cast->base, stream, "fptrunc");
		break;
	case MIR_CAST_FPEXT:
		print_instr_head(&cast->base, stream, "fpext");
		break;
	case MIR_CAST_SITOFP:
		print_instr_head(&cast->base, stream, "sitofp");
		break;
	case MIR_CAST_UITOFP:
		print_instr_head(&cast->base, stream, "uitofp");
		break;
	case MIR_CAST_PTRTOINT:
		print_instr_head(&cast->base, stream, "ptrtoint");
		break;
	case MIR_CAST_INTTOPTR:
		print_instr_head(&cast->base, stream, "inttoptr");
		break;
	case MIR_CAST_INVALID:
		print_instr_head(&cast->base, stream, "<invalid cast>");
		break;
	}

	fprintf(stream, "%%%llu", (unsigned long long)cast->expr->id);
}

void
print_instr_compound(MirInstrCompound *init, FILE *stream)
{
	print_instr_head(&init->base, stream, "compound");
	if (init->type) {
		print_comptime_value_or_id(init->type, stream);
	} else {
		print_type(init->base.value.type, false, stream, true);
	}

	fprintf(stream, " {");
	TSmallArray_InstrPtr *values = init->values;
	if (values) {
		MirInstr *value;
		TSA_FOREACH(values, value)
		{
			print_comptime_value_or_id(value, stream);
			if (i < values->size - 1) fprintf(stream, ", ");
		}
	} else {
		fprintf(stream, "<invalid values>");
	}
	fprintf(stream, "}");

	if (init->is_naked) fprintf(stream, " /* naked */");
}

void
print_instr_vargs(MirInstrVArgs *vargs, FILE *stream)
{
	print_instr_head(&vargs->base, stream, "vargs");
	print_type(vargs->type, false, stream, true);

	fprintf(stream, " {");
	TSmallArray_InstrPtr *values = vargs->values;
	if (values) {
		MirInstr *value;
		TSA_FOREACH(values, value)
		{
			print_comptime_value_or_id(value, stream);
			if (i < values->size - 1) fprintf(stream, ", ");
		}
	} else {
		fprintf(stream, "<invalid values>");
	}
	fprintf(stream, "}");
}

void
print_instr_sizeof(MirInstrSizeof *szof, FILE *stream)
{
	print_instr_head(&szof->base, stream, "sizeof");
	fprintf(stream, " ");
	print_comptime_value_or_id(szof->expr, stream);
}

void
print_instr_type_info(MirInstrTypeInfo *type_info, FILE *stream)
{
	print_instr_head(&type_info->base, stream, "typeinfo");
	print_comptime_value_or_id(type_info->expr, stream);
}

void
print_instr_alignof(MirInstrAlignof *szof, FILE *stream)
{
	print_instr_head(&szof->base, stream, "alignof");
	fprintf(stream, " ");
	print_comptime_value_or_id(szof->expr, stream);
}

void
print_instr_elem_ptr(MirInstrElemPtr *elem_ptr, FILE *stream)
{
	print_instr_head(&elem_ptr->base, stream, "elemptr");
	fprintf(stream, "%%%llu[", (unsigned long long)elem_ptr->arr_ptr->id);
	print_comptime_value_or_id(elem_ptr->index, stream);
	fprintf(stream, "]");
}

void
print_instr_member_ptr(MirInstrMemberPtr *member_ptr, FILE *stream)
{
	print_instr_head(&member_ptr->base, stream, "memberptr");
	if (!member_ptr->target_ptr) {
		fprintf(stream, "<unknown>.");
	} else {
		print_comptime_value_or_id(member_ptr->target_ptr, stream);
		fprintf(stream, ".");
	}

	if (member_ptr->builtin_id == MIR_BUILTIN_ID_NONE) {
		if (member_ptr->member_ident) {
			fprintf(stream, "%s", member_ptr->member_ident->data.ident.id.str);
		} else {
			fprintf(stream, "<unknown>");
		}
	} else {
		switch (member_ptr->builtin_id) {
		case MIR_BUILTIN_ID_ARR_LEN:
			fprintf(stream, "len");
			break;
		case MIR_BUILTIN_ID_ARR_PTR:
			fprintf(stream, "ptr");
			break;

		default:
			fprintf(stream, "<unknown>");
		}
	}
}

void
print_instr_unop(MirInstrUnop *unop, FILE *stream)
{
	print_instr_head(&unop->base, stream, "unop");

	const char *op = ast_unop_to_str(unop->op);
	fprintf(stream, "%s", op);
	print_comptime_value_or_id(unop->expr, stream);
}

void
print_instr_cond_br(MirInstrCondBr *cond_br, FILE *stream)
{
	print_instr_head(&cond_br->base, stream, "br");
	print_comptime_value_or_id(cond_br->cond, stream);
	fprintf(stream,
	        " ? %%%s_%llu : %%%s_%llu",
	        cond_br->then_block->name,
	        (unsigned long long)cond_br->then_block->base.id,
	        cond_br->else_block->name,
	        (unsigned long long)cond_br->else_block->base.id);
}

void
print_instr_arg(MirInstrArg *arg, FILE *stream)
{
	print_instr_head(&arg->base, stream, "arg");
	fprintf(stream, "$%u", arg->i);
}

void
print_instr_unreachable(MirInstrUnreachable *unr, FILE *stream)
{
	print_instr_head(&unr->base, stream, "unreachable");
}

void
print_instr_br(MirInstrBr *br, FILE *stream)
{
	print_instr_head(&br->base, stream, "br");
	fprintf(
	    stream, "%%%s_%llu", br->then_block->name, (unsigned long long)br->then_block->base.id);
}

void
print_instr_switch(MirInstrSwitch *sw, FILE *stream)
{
	print_instr_head(&sw->base, stream, "switch");
	print_comptime_value_or_id(sw->value, stream);
	fprintf(stream, " {");

	MirSwitchCase *c;
	for (usize i = 0; i < sw->cases->size; ++i) {
		c = &sw->cases->data[i];

		print_comptime_value_or_id(c->on_value, stream);
		fprintf(
		    stream, ": %%%s_%llu", c->block->name, (unsigned long long)c->block->base.id);

		if (i < sw->cases->size - 1) fprintf(stream, "; ");
	}

	fprintf(stream,
	        "} else %%%s_%llu",
	        sw->default_block->name,
	        (unsigned long long)sw->default_block->base.id);
}

void
print_instr_load(MirInstrLoad *load, FILE *stream)
{
	print_instr_head(&load->base, stream, "load");
	print_comptime_value_or_id(load->src, stream);
}

void
print_instr_addrof(MirInstrAddrof *addrof, FILE *stream)
{
	print_instr_head(&addrof->base, stream, "addrof");
	fprintf(stream, "%%%llu", (unsigned long long)addrof->src->id);
}

void
print_instr_decl_var(MirInstrDeclVar *decl, FILE *stream)
{
	MirVar *var = decl->var;
	BL_ASSERT(var);

	const char *name = var->llvm_name ? var->llvm_name : "<unknown>";

	if (var->is_in_gscope) {
		/* global scope variable */
		fprintf(stream, "\n@%s : ", name);
		print_type(var->value.type, false, stream, true);
		fprintf(stream, " %s ", var->is_mutable ? "=" : ":");
		if (decl->init) {
			print_comptime_value_or_id(decl->init, stream);
		} else {
			fprintf(stream, "<uninitialized>");
		}
	} else {
		/* local scope variable */
		print_instr_head(&decl->base, stream, "decl");

		fprintf(stream, "%s : ", name);
		print_type(var->value.type, false, stream, true);
		if (decl->init) {
			fprintf(stream, " %s ", var->is_mutable ? "=" : ":");
			print_comptime_value_or_id(decl->init, stream);
		}
	}

	print_flags(var->flags, stream);
}

void
print_instr_decl_variant(MirInstrDeclVariant *var, FILE *stream)
{
	print_instr_head(&var->base, stream, "declvariant");
	BL_ASSERT(var->variant);

	MirVariant *variant = var->variant;
	BL_ASSERT(variant);

	fprintf(stream, "%s", variant->id->str);

	if (var->value) {
		fprintf(stream, " :: ");
		print_comptime_value_or_id(var->value, stream);
	}
}

void
print_instr_decl_arg(MirInstrDeclArg *decl, FILE *stream)
{
	print_instr_head(&decl->base, stream, "declarg");

	MirArg *arg = decl->arg;
	BL_ASSERT(arg)

	fprintf(stream, "%s : ", arg->id ? arg->id->str : "-");
	print_comptime_value_or_id(decl->type, stream);
}

void
print_instr_decl_member(MirInstrDeclMember *decl, FILE *stream)
{
	print_instr_head(&decl->base, stream, "declmember");

	MirMember *member = decl->member;
	BL_ASSERT(member);

	fprintf(stream, "%s : ", member->id->str);
	print_comptime_value_or_id(decl->type, stream);
}

void
print_instr_decl_ref(MirInstrDeclRef *ref, FILE *stream)
{
	print_instr_head(&ref->base, stream, "declref");

	const char *name = ref->rid->str;
	fprintf(stream, "%s", name);
	if (ref->accept_incomplete_type) fprintf(stream, " /* accept incomplete */");
}

void
print_instr_decl_direct_ref(MirInstrDeclDirectRef *ref, FILE *stream)
{
	print_instr_head(&ref->base, stream, "declref");

	print_comptime_value_or_id(ref->ref, stream);
	fprintf(stream, " /* direct */");
}

void
print_instr_const(MirInstrConst *cnst, FILE *stream)
{
	print_instr_head(&cnst->base, stream, "const");
	print_const_value(&cnst->base.value, stream);
}

void
print_instr_call(MirInstrCall *call, FILE *stream)
{
	print_instr_head(&call->base, stream, "call");

	const char *callee_name = call->callee->value.data.v_ptr.data.fn
	                              ? call->callee->value.data.v_ptr.data.fn->linkage_name
	                              : NULL;
	if (callee_name)
		fprintf(stream, "@%s", callee_name);
	else
		fprintf(stream, "%%%llu", (unsigned long long)call->callee->id);

	fprintf(stream, "(");
	if (call->args) {
		MirInstr *tmp;
		TSA_FOREACH(call->args, tmp)
		{
			print_comptime_value_or_id(tmp, stream);
			if (i < call->args->size - 1) fprintf(stream, ", ");
		}
	}
	fprintf(stream, ")");
}

void
print_instr_ret(MirInstrRet *ret, FILE *stream)
{
	print_instr_head(&ret->base, stream, "ret");
	if (ret->value) print_comptime_value_or_id(ret->value, stream);
	if (ret->infer_type) fprintf(stream, " /* infer */");
}

void
print_instr_store(MirInstrStore *store, FILE *stream)
{
	print_instr_head(&store->base, stream, "store");
	BL_ASSERT(store->src && store->src);
	print_comptime_value_or_id(store->src, stream);
	fprintf(stream, " -> %%%llu", (unsigned long long)store->dest->id);
	// print_comptime_value_or_id(store->dest, stream);
}

void
print_instr_binop(MirInstrBinop *binop, FILE *stream)
{
	print_instr_head(&binop->base, stream, "binop");
	BL_ASSERT(binop->lhs && binop->rhs);
	const char *op = ast_binop_to_str(binop->op);
	print_comptime_value_or_id(binop->lhs, stream);
	fprintf(stream, " %s ", op);
	print_comptime_value_or_id(binop->rhs, stream);
}

void
print_instr_block(MirInstrBlock *block, FILE *stream)
{
	if (block->base.prev) fprintf(stream, "\n");
#if BL_DEBUG
	fprintf(stream,
	        "%%%s_%llu (%u):",
	        block->name,
	        (unsigned long long)block->base.id,
	        block->base.ref_count);
#else
	fprintf(stream, "%%%s_%llu:", block->name, (unsigned long long)block->base.id);
#endif
	if (!block->base.ref_count)
		fprintf(stream, " /* NEVER REACHED */\n");
	else
		fprintf(stream, "\n");

	MirInstr *tmp = block->entry_instr;

	while (tmp) {
		mir_print_instr(tmp, stream);
		tmp = tmp->next;
	}
}

void
print_instr_fn_proto(MirInstrFnProto *fn_proto, FILE *stream)
{
	MirFn *fn = fn_proto->base.value.data.v_ptr.data.fn;
	BL_ASSERT(fn);

	fprintf(stream, "\n");

	if (fn_proto->base.analyzed) fprintf(stream, "/* analyzed */\n");
	if (!fn->emit_llvm) fprintf(stream, "/* no LLVM */\n");

	if (fn->linkage_name)
		fprintf(stream, "@%s ", fn->linkage_name);
	else
		fprintf(stream, "@%llu ", (unsigned long long)fn_proto->base.id);

	fprintf(stream, ": ");
	print_type(fn->type, false, stream, false);
	fprintf(stream, " : ");

	print_flags(fn->flags, stream);

	MirInstrBlock *tmp = fn->first_block;
	if (!tmp) return;
	fprintf(stream, "{\n");
	while (tmp) {
		print_instr_block(tmp, stream);
		tmp = (MirInstrBlock *)tmp->base.next;
	}
	fprintf(stream, "}");
}

/* public */
void
mir_print_instr(MirInstr *instr, FILE *stream)
{
	switch (instr->kind) {
	case MIR_INSTR_BLOCK:
		break;

	case MIR_INSTR_INVALID:
		fprintf(stream, RED("INVALID"));
		break;
	case MIR_INSTR_UNREACHABLE:
		print_instr_unreachable((MirInstrUnreachable *)instr, stream);
		break;
	case MIR_INSTR_DECL_VAR:
		print_instr_decl_var((MirInstrDeclVar *)instr, stream);
		break;
	case MIR_INSTR_DECL_VARIANT:
		print_instr_decl_variant((MirInstrDeclVariant *)instr, stream);
		break;
	case MIR_INSTR_DECL_MEMBER:
		print_instr_decl_member((MirInstrDeclMember *)instr, stream);
		break;
	case MIR_INSTR_DECL_ARG:
		print_instr_decl_arg((MirInstrDeclArg *)instr, stream);
		break;
	case MIR_INSTR_CONST:
		print_instr_const((MirInstrConst *)instr, stream);
		break;
	case MIR_INSTR_LOAD:
		print_instr_load((MirInstrLoad *)instr, stream);
		break;
	case MIR_INSTR_STORE:
		print_instr_store((MirInstrStore *)instr, stream);
		break;
	case MIR_INSTR_RET:
		print_instr_ret((MirInstrRet *)instr, stream);
		break;
	case MIR_INSTR_BINOP:
		print_instr_binop((MirInstrBinop *)instr, stream);
		break;
	case MIR_INSTR_CALL:
		print_instr_call((MirInstrCall *)instr, stream);
		break;
	case MIR_INSTR_FN_PROTO:
		print_instr_fn_proto((MirInstrFnProto *)instr, stream);
		break;
	case MIR_INSTR_DECL_REF:
		print_instr_decl_ref((MirInstrDeclRef *)instr, stream);
		break;
	case MIR_INSTR_TYPE_FN:
		print_instr_type_fn((MirInstrTypeFn *)instr, stream);
		break;
	case MIR_INSTR_TYPE_STRUCT:
		print_instr_type_struct((MirInstrTypeStruct *)instr, stream);
		break;
	case MIR_INSTR_TYPE_ARRAY:
		print_instr_type_array((MirInstrTypeArray *)instr, stream);
		break;
	case MIR_INSTR_TYPE_SLICE:
		print_instr_type_slice((MirInstrTypeSlice *)instr, stream);
		break;
	case MIR_INSTR_TYPE_VARGS:
		print_instr_type_vargs((MirInstrTypeVArgs *)instr, stream);
		break;
	case MIR_INSTR_TYPE_ENUM:
		print_instr_type_enum((MirInstrTypeEnum *)instr, stream);
		break;
	case MIR_INSTR_COND_BR:
		print_instr_cond_br((MirInstrCondBr *)instr, stream);
		break;
	case MIR_INSTR_BR:
		print_instr_br((MirInstrBr *)instr, stream);
		break;
	case MIR_INSTR_SWITCH:
		print_instr_switch((MirInstrSwitch *)instr, stream);
		break;
	case MIR_INSTR_UNOP:
		print_instr_unop((MirInstrUnop *)instr, stream);
		break;
	case MIR_INSTR_ARG:
		print_instr_arg((MirInstrArg *)instr, stream);
		break;
	case MIR_INSTR_ELEM_PTR:
		print_instr_elem_ptr((MirInstrElemPtr *)instr, stream);
		break;
	case MIR_INSTR_TYPE_PTR:
		print_instr_type_ptr((MirInstrTypePtr *)instr, stream);
		break;
	case MIR_INSTR_ADDROF:
		print_instr_addrof((MirInstrAddrof *)instr, stream);
		break;
	case MIR_INSTR_MEMBER_PTR:
		print_instr_member_ptr((MirInstrMemberPtr *)instr, stream);
		break;
	case MIR_INSTR_CAST:
		print_instr_cast((MirInstrCast *)instr, stream);
		break;
	case MIR_INSTR_SIZEOF:
		print_instr_sizeof((MirInstrSizeof *)instr, stream);
		break;
	case MIR_INSTR_ALIGNOF:
		print_instr_alignof((MirInstrAlignof *)instr, stream);
		break;
	case MIR_INSTR_COMPOUND:
		print_instr_compound((MirInstrCompound *)instr, stream);
		break;
	case MIR_INSTR_VARGS:
		print_instr_vargs((MirInstrVArgs *)instr, stream);
		break;
	case MIR_INSTR_TYPE_INFO:
		print_instr_type_info((MirInstrTypeInfo *)instr, stream);
		break;
	case MIR_INSTR_PHI:
		print_instr_phi((MirInstrPhi *)instr, stream);
		break;
	case MIR_INSTR_TOANY:
		print_instr_toany((MirInstrToAny *)instr, stream);
		break;
	case MIR_INSTR_DECL_DIRECT_REF:
		print_instr_decl_direct_ref((MirInstrDeclDirectRef *)instr, stream);
		break;
	}

	if (instr->eval_mode == MIR_VEM_COMPTIME) fprintf(stream, " /* comptime */");
	if (instr->unrechable) fprintf(stream, " /* unrechable */");

	fprintf(stream, "\n");
}

void
mir_print_assembly(Assembly *assembly, FILE *stream)
{
	MirInstr *instr;
	TARRAY_FOREACH(MirInstr *, &assembly->MIR.global_instrs, instr)
	{
		mir_print_instr(instr, stream);
	}
}
