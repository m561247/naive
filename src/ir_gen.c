#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "array.h"
#include "ir.h"
#include "parse.h"
#include "util.h"

typedef enum CTypeType
{
	VOID_TYPE,
	INTEGER_TYPE,
	FUNCTION_TYPE,
	STRUCT_TYPE,
	POINTER_TYPE,
	ARRAY_TYPE,
} CTypeType;

typedef struct CType
{
	CTypeType t;

	struct CType *cached_pointer_type;

	union
	{
		struct
		{
			enum
			{
				CHAR,
				SHORT,
				INT,
				LONG,
				LONGLONG,
			} type;
			bool is_signed;
		} integer;
		struct
		{
			struct CType *return_type;
			struct CType **arg_type_array;
			u32 arity;
		} function;
		struct
		{
			Array(CDecl) fields;
			IrType *ir_type;
		} strukt;
		struct CType *pointee_type;
		struct
		{
			struct CType *elem_type;
			u64 size;
			IrType *ir_type;
		} array;
	} u;
} CType;

static IrType c_type_to_ir_type(CType *ctype)
{
	switch (ctype->t) {
	case VOID_TYPE:
		return (IrType) { .t = IR_VOID };
	case INTEGER_TYPE: {
		u32 bit_width;
		switch (ctype->u.integer.type) {
		case CHAR: bit_width = 8; break;
		case INT: bit_width = 32; break;
		default: UNIMPLEMENTED;
		}

		return (IrType) {
			.t = IR_INT,
			.u.bit_width = bit_width,
		};
	}
	case POINTER_TYPE:
		return (IrType) { .t = IR_POINTER };
	case ARRAY_TYPE:
		return *ctype->u.array.ir_type;
	case FUNCTION_TYPE:
		return (IrType) { .t = FUNCTION_TYPE };
	case STRUCT_TYPE:
		return *ctype->u.strukt.ir_type;
	}
}

static u8 rank(CType *type)
{
	assert(type->t == INTEGER_TYPE);
	switch (type->u.integer.type) {
	case CHAR: return 1;
	case SHORT: return 2;
	case INT: return 3;
	case LONG: return 4;
	case LONGLONG: return 5;
	}
}

typedef struct Term
{
	CType *ctype;
	IrValue value;
} Term;

typedef struct Binding
{
	char *name;
	Term term;
} Binding;

typedef struct Scope
{
	Array(Binding) bindings;
	struct Scope *parent_scope;
} Scope;

Binding *binding_for_name(Scope *scope, char *name)
{
	for (u32 i = 0; i < scope->bindings.size; i++) {
		Binding *binding = ARRAY_REF(&scope->bindings, Binding, i);
		if (streq(binding->name, name))
			return binding;
	}

	if (scope->parent_scope != NULL) {
		return binding_for_name(scope->parent_scope, name);
	} else {
		return NULL;
	}
}

typedef struct TypeEnvEntry
{
	char *name;
	CType type;
} TypeEnvEntry;

typedef struct TypeEnv
{
	Pool pool;
	Array(TypeEnvEntry *) struct_types;
	Array(TypeEnvEntry *) union_types;
	Array(TypeEnvEntry *) enum_types;
	Array(TypeEnvEntry *) typedef_types;

	CType void_type;
	CType char_type;
	CType int_type;
	CType unsigned_int_type;
} TypeEnv;

static void init_type_env(TypeEnv *type_env)
{
	pool_init(&type_env->pool, 512);
	ARRAY_INIT(&type_env->struct_types, TypeEnvEntry *, 10);
	ARRAY_INIT(&type_env->union_types, TypeEnvEntry *, 10);
	ARRAY_INIT(&type_env->enum_types, TypeEnvEntry *, 10);
	ARRAY_INIT(&type_env->typedef_types, TypeEnvEntry *, 10);

	type_env->void_type = (CType) {
		.t = VOID_TYPE,
		.cached_pointer_type = NULL,
	};
	type_env->int_type = (CType) {
		.t = INTEGER_TYPE,
		.cached_pointer_type = NULL,
		.u.integer.type = INT,
		.u.integer.is_signed = true,
	};
	type_env->unsigned_int_type = (CType) {
		.t = INTEGER_TYPE,
		.cached_pointer_type = NULL,
		.u.integer.type = INT,
		.u.integer.is_signed = false,
	};
	type_env->char_type = (CType) {
		.t = INTEGER_TYPE,
		.cached_pointer_type = NULL,
		.u.integer.type = CHAR,
		// System V x86-64 says "char" == "signed char"
		.u.integer.is_signed = true,
	};
}

static CType *search(Array(TypeEnvEntry *) *types, char *name)
{
	for (u32 i = 0; i < types->size; i++) {
		TypeEnvEntry *entry = *ARRAY_REF(types, TypeEnvEntry *, i);
		if (streq(entry->name, name)) {
			return &entry->type;
		}
	}

	assert(false);
}

typedef struct CDecl
{
	char *name;
	CType *type;
} CDecl;

static void decl_to_cdecl(IrBuilder *builder, TypeEnv *type_env,
		ASTDeclSpecifier *decl_specifier_list, ASTDeclarator *declarator,
		CDecl *cdecl);

static bool matches_sequence(ASTDeclSpecifier *decl_specifier_list, int length, ...)
{
	va_list args;
	va_start(args, length);

	while (length != 0) {
		if (decl_specifier_list == NULL) {
			va_end(args);
			return false;
		}

		char *str = va_arg(args, char *);
		length--;

		assert(decl_specifier_list->t == TYPE_SPECIFIER);
		ASTTypeSpecifier *type_spec = decl_specifier_list->u.type_specifier;
		if (type_spec->t != NAMED_TYPE_SPECIFIER
				|| !streq(type_spec->u.name, str)) {
			va_end(args);
			return false;
		}

		decl_specifier_list = decl_specifier_list->next;
	}

	va_end(args);
	return true;
}

static CType *decl_specifier_list_to_c_type(IrBuilder *builder, TypeEnv *type_env,
		ASTDeclSpecifier *decl_specifier_list)
{
	assert(decl_specifier_list->t == TYPE_SPECIFIER);
	ASTTypeSpecifier *type_spec = decl_specifier_list->u.type_specifier;

	switch (type_spec->t) {
	case NAMED_TYPE_SPECIFIER: {
		if (matches_sequence(decl_specifier_list, 1, "void")) {
			return &type_env->void_type;
		}
		if (matches_sequence(decl_specifier_list, 1, "unsigned")
				|| matches_sequence(decl_specifier_list, 2, "unsigned", "int")) {
			return &type_env->unsigned_int_type;
		}
		if (matches_sequence(decl_specifier_list, 1, "int")
				|| matches_sequence(decl_specifier_list, 1, "signed")
				|| matches_sequence(decl_specifier_list, 2, "signed", "int")) {
			return &type_env->int_type;
		}
		if (matches_sequence(decl_specifier_list, 1, "char")) {
			return &type_env->char_type;
		}

		return search(&type_env->typedef_types, type_spec->u.name);
	}
	case STRUCT_TYPE_SPECIFIER: {
		ASTFieldDecl *field_list = type_spec->u.struct_or_union_specifier.field_list;
		char *name = type_spec->u.struct_or_union_specifier.name;

		if (field_list == NULL) {
			assert(name != NULL);
			return search(&type_env->struct_types, name);
		}

		TypeEnvEntry *struct_type = pool_alloc(&type_env->pool, sizeof *struct_type);
		*ARRAY_APPEND(&type_env->struct_types, TypeEnvEntry *) = struct_type;
		if (name == NULL)
			name = "<anonymous struct>";
		struct_type->name = name;

		CType *type = &struct_type->type;
		type->t = STRUCT_TYPE;
		type->cached_pointer_type = NULL;

		Array(CDecl) *fields = &type->u.strukt.fields;
		ARRAY_INIT(fields, CDecl, 5);
		while (field_list != NULL) {
			ASTDeclSpecifier *decl_specs = field_list->decl_specifier_list;
			ASTFieldDeclarator *field_declarator = field_list->field_declarator_list;
			while (field_declarator != NULL) {
				assert(field_declarator->t == NORMAL_FIELD_DECLARATOR);
				ASTDeclarator *declarator = field_declarator->u.declarator;

				CDecl *cdecl = ARRAY_APPEND(fields, CDecl);
				decl_to_cdecl(builder, type_env, decl_specs, declarator, cdecl);

				field_declarator = field_declarator->next;
			}

			field_list = field_list->next;
		}

		IrType *ir_struct =
			trans_unit_add_struct(builder->trans_unit, name, fields->size);
		u32 current_offset = 0;
		u32 max_field_align = 0;
		for (u32 i = 0; i < fields->size; i++) {
			CDecl *field = ARRAY_REF(fields, CDecl, i);
			IrType field_type = c_type_to_ir_type(field->type);
			u32 field_size = size_of_ir_type(field_type);
			u32 field_align = align_of_ir_type(field_type);
			max_field_align = max(max_field_align, field_align);

			current_offset = align_to(current_offset, field_align);

			ir_struct->u.strukt.fields[i].type = field_type;
			ir_struct->u.strukt.fields[i].offset = current_offset;

			current_offset += field_size;
		}
		ir_struct->u.strukt.total_size = align_to(current_offset, max_field_align);
		ir_struct->u.strukt.alignment = max_field_align;
		type->u.strukt.ir_type = ir_struct;

		return &struct_type->type;

		break;
	}
	default: UNIMPLEMENTED;
	}
}

typedef struct Env
{
	Scope *scope;
	TypeEnv type_env;
	IrBlock *break_target;
	IrBlock *continue_target;
} Env;

static CType *pointer_type(TypeEnv *type_env, CType *type)
{
	if (type->cached_pointer_type != NULL)
		return type->cached_pointer_type;

	CType *pointer_type = pool_alloc(&type_env->pool, sizeof *pointer_type);
	pointer_type->t = POINTER_TYPE;
	pointer_type->cached_pointer_type = NULL;
	pointer_type->u.pointee_type = type;

	type->cached_pointer_type = pointer_type;

	return pointer_type;
}

static CType *array_type(IrBuilder *builder, TypeEnv *type_env, CType *type,
		u64 size)
{
	CType *array_type = pool_alloc(&type_env->pool, sizeof *array_type);
	array_type->t = ARRAY_TYPE;
	array_type->cached_pointer_type = NULL;
	array_type->u.array.elem_type = type;
	array_type->u.array.size = size;

	IrType *ir_array_type =
		pool_alloc(&builder->trans_unit->pool, sizeof *ir_array_type);
	ir_array_type->t = IR_ARRAY;

	IrType elem_type = c_type_to_ir_type(type);
	IrType *ir_elem_type;

	if (elem_type.t == IR_ARRAY) {
		size *= elem_type.u.array.size;
		ir_elem_type = elem_type.u.array.elem_type;
	} else {
		ir_elem_type =
			pool_alloc(&builder->trans_unit->pool, sizeof *ir_elem_type);
		*ir_elem_type = c_type_to_ir_type(type);
	}
	ir_array_type->u.array.elem_type = ir_elem_type;
	ir_array_type->u.array.size = size;

	array_type->u.array.ir_type = ir_array_type;

	return array_type;
}

static u64 eval_constant_expr(ASTExpr *constant_expr)
{
	switch (constant_expr->t) {
	case INT_LITERAL_EXPR:
		return constant_expr->u.int_literal;
	default:
		UNIMPLEMENTED;
	}
}

static ASTParameterDecl *params_for_function_declarator(ASTDeclarator *declarator)
{
	while (declarator->t != DIRECT_DECLARATOR) {
		assert(declarator->t == POINTER_DECLARATOR);
		declarator = declarator->u.pointer_declarator.pointee;
	}

	assert(declarator->t == DIRECT_DECLARATOR);
	ASTDirectDeclarator* direct_declarator = declarator->u.direct_declarator;
	assert(direct_declarator->t == FUNCTION_DECLARATOR);

	return direct_declarator->u.function_declarator.parameters;
}

static void direct_declarator_to_cdecl(IrBuilder *builder, TypeEnv *type_env,
		ASTDeclSpecifier *decl_specifier_list,
		ASTDirectDeclarator *direct_declarator, CDecl *cdecl) {
	switch (direct_declarator->t) {
	case FUNCTION_DECLARATOR: {
		ASTDirectDeclarator *function_declarator =
			direct_declarator->u.function_declarator.declarator;
		assert(function_declarator->t == IDENTIFIER_DECLARATOR);

		cdecl->name = function_declarator->u.name;

		CType *return_c_type =
			decl_specifier_list_to_c_type(builder, type_env, decl_specifier_list);

		ASTParameterDecl *first_param =
			direct_declarator->u.function_declarator.parameters;
		ASTParameterDecl *params = first_param;

		u32 arity = 0;
		while (params != NULL) {
			arity++;
			params = params->next;
		}

		params = first_param;

		CType **arg_c_types = pool_alloc(
				&builder->trans_unit->pool,
				sizeof(*arg_c_types) * arity);

		for (u32 i = 0; i < arity; i++) {
			CDecl cdecl;
			decl_to_cdecl(builder, type_env, params->decl_specifier_list,
					params->declarator, &cdecl);
			arg_c_types[i] = cdecl.type;

			params = params->next;
		}

		CType *ctype = pool_alloc(&builder->trans_unit->pool, sizeof *ctype);
		ctype->t = FUNCTION_TYPE;
		ctype->u.function.arity = arity;
		ctype->u.function.arg_type_array = arg_c_types;
		ctype->u.function.return_type = return_c_type;

		cdecl->type = ctype;
		break;
	}
	case IDENTIFIER_DECLARATOR: {
		cdecl->name = direct_declarator->u.name;
		cdecl->type = decl_specifier_list_to_c_type(builder, type_env, decl_specifier_list);
		break;
	}
	case ARRAY_DECLARATOR: {
		ASTDirectDeclarator *elem_declarator =
			direct_declarator->u.array_declarator.element_declarator;
		ASTExpr *array_length_expr =
			direct_declarator->u.array_declarator.array_length;

		CDecl elem_cdecl;
		direct_declarator_to_cdecl(builder, type_env, decl_specifier_list,
				elem_declarator, &elem_cdecl);
		cdecl->name = elem_cdecl.name;
		cdecl->type = array_type(builder, type_env, elem_cdecl.type,
				eval_constant_expr(array_length_expr));
		break;
	} 
	default:
		UNIMPLEMENTED;
	}
}

static void decl_to_cdecl(IrBuilder *builder, TypeEnv *type_env,
		ASTDeclSpecifier *decl_specifier_list, ASTDeclarator *declarator,
		CDecl *cdecl)
{
	if (declarator == NULL) {
		cdecl->type =
			decl_specifier_list_to_c_type(builder, type_env, decl_specifier_list);
	} else if (declarator->t == POINTER_DECLARATOR) {
		CDecl pointee_cdecl;
		assert(declarator->u.pointer_declarator.decl_specifier_list == NULL);
		decl_to_cdecl(builder, type_env, decl_specifier_list,
				declarator->u.pointer_declarator.pointee, &pointee_cdecl);
		cdecl->name = pointee_cdecl.name;

		if (pointee_cdecl.type->t == FUNCTION_TYPE) {
			CType **return_type_ptr = &pointee_cdecl.type->u.function.return_type;
			CType *return_type = *return_type_ptr;
			*return_type_ptr = pointer_type(type_env, return_type);
			cdecl->type = pointee_cdecl.type;
		} else {
			cdecl->type = pointer_type(type_env, pointee_cdecl.type);
		}
	} else {
		assert(declarator->t == DIRECT_DECLARATOR);
		ASTDirectDeclarator *direct_declarator = declarator->u.direct_declarator;

		direct_declarator_to_cdecl(builder, type_env, decl_specifier_list,
				direct_declarator, cdecl);
	}
}

static void cdecl_to_binding(IrBuilder *builder, CDecl *cdecl, Binding *binding)
{
	IrType ir_type = c_type_to_ir_type(cdecl->type);

	binding->name = cdecl->name;
	binding->term.ctype = cdecl->type;
	binding->term.value = build_local(builder, ir_type);
}

static inline IrBlock *add_block(IrBuilder *builder, char *name)
{
	return add_block_to_function(builder->trans_unit, builder->current_function, name);
}

static IrGlobal *ir_global_for_decl(IrBuilder *builder, TypeEnv *type_env,
		ASTDeclSpecifier *decl_specifier_list, ASTDeclarator *declarator,
		CType **result_c_type)
{
	CDecl cdecl;
	decl_to_cdecl(builder, type_env, decl_specifier_list, declarator, &cdecl);
	CType *ctype = cdecl.type;
	if (ctype->t == FUNCTION_TYPE) {
		u32 arity = ctype->u.function.arity;
		IrType *arg_ir_types = malloc(sizeof(*arg_ir_types) * arity);
		
		for (u32 i = 0; i < arity; i++) {
			CType *arg_c_type = cdecl.type->u.function.arg_type_array[i];
			arg_ir_types[i] = c_type_to_ir_type(arg_c_type);
		}

		IrGlobal *global = NULL;
		Array(IrGlobal *) *globals = &builder->trans_unit->globals;
		for (u32 i = 0; i < globals->size; i++) {
			IrGlobal *curr_global = *ARRAY_REF(globals, IrGlobal *, i);
			if (streq(curr_global->name, cdecl.name)) {
				// @TODO: Check C type matches
				global = curr_global;
				break;
			}
		}
		
		if (global == NULL) {
			global = trans_unit_add_function(
					builder->trans_unit, cdecl.name,
					c_type_to_ir_type(ctype->u.function.return_type),
					arity, arg_ir_types);
		}

		assert(global->type.t == IR_FUNCTION);
		*result_c_type = ctype;

		return global;
	} else {
		CDecl cdecl;
		decl_to_cdecl(builder, type_env, decl_specifier_list, declarator, &cdecl);

		IrGlobal *global = NULL;
		Array(IrGlobal *) *globals = &builder->trans_unit->globals;
		for (u32 i = 0; i < globals->size; i++) {
			IrGlobal *curr_global = *ARRAY_REF(globals, IrGlobal *, i);
			if (streq(curr_global->name, cdecl.name)) {
				// @TODO: Check C type matches
				global = curr_global;
				break;
			}
		}

		if (global == NULL) {
			global = trans_unit_add_var(
					builder->trans_unit,
					cdecl.name,
					c_type_to_ir_type(ctype));
		}

		*result_c_type = cdecl.type;

		return global;
	}
}

static void ir_gen_statement(IrBuilder *builder, Env *env, ASTStatement *statement);

typedef enum ExprContext
{
	LVALUE_CONTEXT,
	RVALUE_CONTEXT,
} ExprContext;
static Term ir_gen_expression(IrBuilder *builder, Env *env, ASTExpr *expr,
		ExprContext context);

static IrInit *zero_initializer(IrBuilder *builder, CType *ctype)
{
	switch (ctype->t) {
	case INTEGER_TYPE:
		return add_int_init(builder, c_type_to_ir_type(ctype), 0);
	case ARRAY_TYPE: {
		// @TODO: This allocates unnecessarily by calling zero_initializer
		// recursively and then copying the result into array_elems.
		IrInit *init = add_array_init(builder, c_type_to_ir_type(ctype));
		for (u32 i = 0; i < ctype->u.array.size; i++) {
			init->u.array_elems[i] =
				*zero_initializer(builder, ctype->u.array.elem_type);
		}
		return init;
	}
	default:
		UNIMPLEMENTED;
	}
}

void ir_gen_toplevel(IrBuilder *builder, ASTToplevel *toplevel)
{
	Scope global_scope;
	global_scope.parent_scope = NULL;
	Array(Binding)* global_bindings = &global_scope.bindings;
	ARRAY_INIT(global_bindings, Binding, 10);

	Env env;
	init_type_env(&env.type_env);
	env.scope = &global_scope;
	env.break_target = NULL;
	env.continue_target = NULL;

	while (toplevel != NULL) {
		IrGlobal *global = NULL;
		CType *global_type;
		ZERO_STRUCT(&global_type);

		switch (toplevel->t) {
		case FUNCTION_DEF: {
			ASTFunctionDef *func = toplevel->u.function_def;
			ASTDeclSpecifier *decl_specifier_list = func->decl_specifier_list;
			ASTDeclarator *declarator = func->declarator;

			global = ir_global_for_decl(builder, &env.type_env,
					decl_specifier_list, declarator, &global_type);
			IrInit *init = add_init_to_function(builder->trans_unit, global);
			IrFunction *function = &init->u.function;

			builder->current_function = function;
			builder->current_block = *ARRAY_REF(&function->blocks, IrBlock *, 0);

			Scope scope;
			scope.parent_scope = &global_scope;
			Array(Binding) *param_bindings = &scope.bindings;
			ARRAY_INIT(param_bindings, Binding, 5);
			env.scope = &scope;

			ASTParameterDecl *param = params_for_function_declarator(declarator);
			for (u32 i = 0; param != NULL; i++, param = param->next) {
				Binding *binding = ARRAY_APPEND(param_bindings, Binding);

				CDecl cdecl;
				decl_to_cdecl(builder, &env.type_env, param->decl_specifier_list,
						param->declarator, &cdecl);
				cdecl_to_binding(builder, &cdecl, binding);

				build_store(builder,
						binding->term.value,
						value_arg(i, global->type.u.function.arg_types[i]),
						c_type_to_ir_type(binding->term.ctype));
			}

			ir_gen_statement(builder, &env, func->body);

			env.scope = env.scope->parent_scope;
			array_free(param_bindings);

			break;
		}
		case DECL: {
			ASTDecl *decl = toplevel->u.decl;
			ASTDeclSpecifier *decl_specifier_list = decl->decl_specifier_list;
			ASTInitDeclarator *init_declarator = decl->init_declarators;
			assert(decl_specifier_list != NULL);


			if (decl_specifier_list->t == STORAGE_CLASS_SPECIFIER &&
					decl_specifier_list->u.storage_class_specifier == TYPEDEF_SPECIFIER) {
				assert(init_declarator != NULL);
				decl_specifier_list = decl_specifier_list->next;

				while (init_declarator != NULL) {
					assert(init_declarator->initializer == NULL);
					CDecl cdecl;
					decl_to_cdecl(builder, &env.type_env, decl_specifier_list,
							init_declarator->declarator, &cdecl);

					TypeEnvEntry *new_type_alias =
						pool_alloc(&env.type_env.pool, sizeof *new_type_alias);
					*ARRAY_APPEND(&env.type_env.typedef_types, TypeEnvEntry *)
						= new_type_alias;
					new_type_alias->name = cdecl.name;
					new_type_alias->type = *cdecl.type;

					init_declarator = init_declarator->next;
				}
			} else {
				ASTDeclSpecifier *type_specs = decl_specifier_list;
				while (type_specs->t == STORAGE_CLASS_SPECIFIER) {
					type_specs = type_specs->next;
				}

				if (init_declarator == NULL) {
					decl_specifier_list_to_c_type(builder, &env.type_env, type_specs);
				} else {
					assert(init_declarator->initializer == NULL);
					assert(init_declarator->next == NULL);
					ASTDeclarator *declarator = init_declarator->declarator;

					// @TODO: Multiple declarators in one global decl.
					global = ir_global_for_decl(builder, &env.type_env,
							type_specs, declarator, &global_type);
					bool is_extern = global_type->t == FUNCTION_TYPE;

					global->linkage = IR_GLOBAL_LINKAGE;
					while (decl_specifier_list != type_specs) {
						assert(decl_specifier_list->t == STORAGE_CLASS_SPECIFIER);
						ASTStorageClassSpecifier storage_class =
							decl_specifier_list->u.storage_class_specifier;
						if (storage_class == STATIC_SPECIFIER)
							global->linkage = IR_LOCAL_LINKAGE;
						else if (storage_class == EXTERN_SPECIFIER)
							is_extern = true;
						else
							UNIMPLEMENTED;

						decl_specifier_list = decl_specifier_list->next;
					}

					if (!is_extern)
						global->initializer = zero_initializer(builder, global_type);
				}
			}

			break;
		}
		}

		if (global != NULL) {
			Binding *binding = ARRAY_APPEND(global_bindings, Binding);
			binding->name = global->name;
			binding->term.ctype = global_type;
			binding->term.value = value_global(global);
		}

		toplevel = toplevel->next;
	}

	pool_free(&env.type_env.pool);
	array_free(&env.type_env.struct_types);
	array_free(&env.type_env.union_types);
	array_free(&env.type_env.enum_types);
	array_free(&env.type_env.typedef_types);
	array_free(global_bindings);
}

static void add_decl_to_scope(IrBuilder *builder, Env *env, ASTDecl *decl)
{
	ASTInitDeclarator *init_declarator = decl->init_declarators;
	while (init_declarator != NULL) {
		CDecl cdecl;
		decl_to_cdecl(builder, &env->type_env, decl->decl_specifier_list,
				init_declarator->declarator,
				&cdecl);

		Binding *binding = ARRAY_APPEND(&env->scope->bindings, Binding);
		cdecl_to_binding(builder, &cdecl, binding);

		ASTInitializer *initializer = init_declarator->initializer;
		if (initializer != NULL) {
			assert(initializer->t == EXPR_INITIALIZER);
			Term init_term = ir_gen_expression(builder, env,
					initializer->u.expr, RVALUE_CONTEXT);
			build_store(
				builder,
				binding->term.value,
				init_term.value,
				c_type_to_ir_type(binding->term.ctype));
		}

		init_declarator = init_declarator->next;
	}
}

static void ir_gen_statement(IrBuilder *builder, Env *env, ASTStatement *statement)
{
	switch (statement->t) {
	case COMPOUND_STATEMENT: {
		Scope block_scope;
		block_scope.parent_scope = env->scope;
		ARRAY_INIT(&block_scope.bindings, Binding, 5);
		env->scope = &block_scope;

		ASTBlockItem *block_item_list = statement->u.block_item_list;
		while (block_item_list != NULL) {
			switch (block_item_list->t) {
			case BLOCK_ITEM_DECL: {
				add_decl_to_scope(builder, env, block_item_list->u.decl);
				break;
			}
			case BLOCK_ITEM_STATEMENT:
				ir_gen_statement(builder, env, block_item_list->u.statement);
				break;
			}

			block_item_list = block_item_list->next;
		}

		env->scope = env->scope->parent_scope;

		break;
	}
	case EXPR_STATEMENT: {
		ir_gen_expression(builder, env, statement->u.expr, RVALUE_CONTEXT);
		break;
	}
	case RETURN_STATEMENT: {
		if (statement->u.expr == NULL) {
			build_nullary_instr(builder, OP_RET_VOID, (IrType) { .t = IR_VOID });
		} else {
			Term term = ir_gen_expression(builder, env,
					statement->u.expr, RVALUE_CONTEXT);
			build_unary_instr(builder, OP_RET, term.value);
		}
		break;
	}
	case IF_STATEMENT: {
		IrBlock *initial_block = builder->current_block;
		IrBlock *then_block = add_block(builder, "if.then");
		IrBlock *after_block = add_block(builder, "if.after");

		ASTStatement *then_statement = statement->u.if_statement.then_statement;
		builder->current_block = then_block;
		ir_gen_statement(builder, env, then_statement);
		build_branch(builder, after_block);

		ASTStatement *else_statement = statement->u.if_statement.else_statement;
		IrBlock *else_block = NULL;
		if (else_statement != NULL) {
			else_block = add_block(builder, "if.else");
			builder->current_block = else_block;
			ir_gen_statement(builder, env, else_statement);
			build_branch(builder, after_block);
		}

		builder->current_block = initial_block;
		ASTExpr *condition_expr = statement->u.if_statement.condition;
		Term condition_term =
			ir_gen_expression(builder, env, condition_expr, RVALUE_CONTEXT);
		assert(condition_term.ctype->t == INTEGER_TYPE);

		if (else_statement == NULL) {
			build_cond(builder, condition_term.value, then_block, after_block);
		} else {
			build_cond(builder, condition_term.value, then_block, else_block);
		}

		builder->current_block = after_block;
		break;
	}
	case WHILE_STATEMENT: {
		IrBlock *pre_header = add_block(builder, "while.ph");
		IrBlock *body = add_block(builder, "while.body");
		IrBlock *after = add_block(builder, "while.after");

		ASTExpr *condition_expr = statement->u.expr_and_statement.expr;
		ASTStatement *body_statement = statement->u.expr_and_statement.statement;

		build_branch(builder, pre_header);
		builder->current_block = pre_header;
		Term condition_term =
			ir_gen_expression(builder, env, condition_expr, RVALUE_CONTEXT);

		assert(condition_term.ctype->t == INTEGER_TYPE);
		build_cond(builder, condition_term.value, body, after);

		IrBlock *prev_break_target = env->break_target;
		IrBlock *prev_continue_target = env->continue_target;
		env->break_target = after;
		env->continue_target = pre_header;
		builder->current_block = body;

		ir_gen_statement(builder, env, body_statement);

		build_branch(builder, pre_header);
		env->break_target = prev_break_target;
		env->continue_target = prev_continue_target;

		builder->current_block = after;

		break;
	}
	case FOR_STATEMENT: {
		IrBlock *pre_header = add_block(builder, "for.ph");
		IrBlock *body = add_block(builder, "for.body");
		IrBlock *update = add_block(builder, "for.update");
		IrBlock *after = add_block(builder, "for.after");

		Scope init_scope;
		Scope *prev_scope = env->scope;

		ASTForStatement *f = &statement->u.for_statement;
		if (f->init_type == FOR_INIT_DECL) {
			init_scope.parent_scope = env->scope;
			ARRAY_INIT(&init_scope.bindings, Binding, 1);
			env->scope = &init_scope;
			add_decl_to_scope(builder, env, f->init.decl);
			env->scope = env->scope->parent_scope;

			env->scope = &init_scope;
		} else {
			assert(f->init_type == FOR_INIT_EXPR);
			if (f->init.expr != NULL)
				ir_gen_expression(builder, env, f->init.expr, RVALUE_CONTEXT);
		}

		build_branch(builder, pre_header);
		builder->current_block = pre_header;
		Term condition_term =
			ir_gen_expression(builder, env, f->condition, RVALUE_CONTEXT);

		assert(condition_term.ctype->t == INTEGER_TYPE);
		build_cond(builder, condition_term.value, body, after);

		builder->current_block = body;
		IrBlock *prev_break_target = env->break_target;
		IrBlock *prev_continue_target = env->continue_target;
		env->break_target = after;
		env->continue_target = update;

		ir_gen_statement(builder, env, f->body);
		build_branch(builder, update);
		builder->current_block = update;

		env->break_target = prev_break_target;
		env->continue_target = prev_continue_target;

		if (f->update_expr != NULL)
			ir_gen_expression(builder, env, f->update_expr, RVALUE_CONTEXT);

		build_branch(builder, pre_header);

		env->scope = prev_scope;
		builder->current_block = after;

		break;
	}
	case BREAK_STATEMENT:
		assert(env->break_target != NULL);
		build_branch(builder, env->break_target);
		break;
	case CONTINUE_STATEMENT:
		assert(env->continue_target != NULL);
		build_branch(builder, env->continue_target);
		break;
	default:
		UNIMPLEMENTED;
	}
}

static Term ir_gen_struct_field(IrBuilder *builder, Term struct_term,
		char *field_name, ExprContext context)
{
		assert(struct_term.value.type.t == IR_POINTER);

		CType *ctype = struct_term.ctype;
		if (struct_term.ctype->t == POINTER_TYPE) {
			ctype = ctype->u.pointee_type;
		}

		assert(ctype->t == STRUCT_TYPE);
		Array(CDecl) *fields = &ctype->u.strukt.fields;
		CDecl *selected_field = NULL;
		u32 field_number;
		for (u32 i = 0; i < fields->size; i++) {
			CDecl *field = ARRAY_REF(fields, CDecl, i);
			if (streq(field->name, field_name)) {
				selected_field = field;
				field_number = i;
				break;
			}
		}
		assert(selected_field != NULL);

		IrValue value = build_field(builder, struct_term.value,
				*ctype->u.strukt.ir_type, field_number);
		IrType *struct_ir_type = ctype->u.strukt.ir_type;
		assert(struct_ir_type->t == IR_STRUCT);
		IrType field_type = struct_ir_type->u.strukt.fields[field_number].type;

		if (context == RVALUE_CONTEXT
				&& selected_field->type->t != STRUCT_TYPE
				&& selected_field->type->t != ARRAY_TYPE) {
			value = build_load(builder, value, field_type);
		}

		return (Term) { .ctype = selected_field->type, .value = value };
}

static CType *decay_to_pointer(TypeEnv *type_env, CType *type)
{
	assert(type->t == ARRAY_TYPE);
	return pointer_type(type_env, type->u.array.elem_type);
}

static Term ir_gen_add(IrBuilder *builder, Env *env, Term left, Term right)
{
	if (left.ctype->t == ARRAY_TYPE) {
		left.ctype = decay_to_pointer(&env->type_env, left.ctype);
	}
	if (right.ctype->t == ARRAY_TYPE) {
		right.ctype = decay_to_pointer(&env->type_env, right.ctype);
	}

	bool left_is_pointer = left.ctype->t == POINTER_TYPE;
	bool right_is_pointer = right.ctype->t == POINTER_TYPE;

	if (left.ctype->t == INTEGER_TYPE && right.ctype->t == INTEGER_TYPE) {
		IrValue value = build_binary_instr(builder, OP_ADD, left.value, right.value);

		// @TODO: Determine type correctly
		return (Term) {
			.ctype = left.ctype,
			.value = value,
		};
	} else if (left_is_pointer ^ right_is_pointer) {
		Term pointer = left_is_pointer ? left : right;
		Term other = left_is_pointer ? right : left;
		assert(other.ctype->t == INTEGER_TYPE);

		CType *result_type = pointer.ctype;
		CType *pointee_type = result_type->u.pointee_type;

		// @TODO: Determine type correctly
		// @TODO: Don't hardcode in the size of a pointer!
		IrType pointer_int_type = (IrType) { .t = IR_INT, .u.bit_width = 64 };

		IrValue zext =
			build_type_instr(builder, OP_ZEXT, other.value, pointer_int_type);
		IrValue ptr_to_int =
			build_type_instr(builder, OP_CAST, pointer.value, pointer_int_type);
		IrValue addend = build_binary_instr(
				builder,
				OP_MUL,
				zext,
				value_const(pointer_int_type,
					size_of_ir_type(c_type_to_ir_type(pointee_type))));

		IrValue sum = build_binary_instr(builder, OP_ADD, ptr_to_int, addend);
		IrValue int_to_ptr = build_type_instr(builder, OP_CAST, sum,
				c_type_to_ir_type(result_type));

		return (Term) {
			.ctype = result_type,
			.value = int_to_ptr,
		};
	} else {
		UNIMPLEMENTED;
	}
}

static Term ir_gen_sub(IrBuilder *builder, Env *env, Term left, Term right)
{
	if (left.ctype->t == ARRAY_TYPE) {
		left.ctype = decay_to_pointer(&env->type_env, left.ctype);
	}
	if (right.ctype->t == ARRAY_TYPE) {
		right.ctype = decay_to_pointer(&env->type_env, right.ctype);
	}

	bool left_is_pointer = left.ctype->t == POINTER_TYPE;
	bool right_is_pointer = right.ctype->t == POINTER_TYPE;

	if (left.ctype->t == INTEGER_TYPE && right.ctype->t == INTEGER_TYPE) {
		IrValue value = build_binary_instr(builder, OP_SUB, left.value, right.value);

		// @TODO: Determine type correctly
		return (Term) {
			.ctype = left.ctype,
			.value = value,
		};
	} else if (left_is_pointer && right_is_pointer) {
		CType *pointee_type = left.ctype->u.pointee_type;

		// @TODO: Determine type correctly
		// @TODO: Don't hardcode in the size of a pointer!
		IrType pointer_int_type = { .t = IR_INT, .u.bit_width = 64 };

		// @TODO: This should be ptrdiff_t
		CType *result_c_type = &env->type_env.int_type;

		IrValue left_int =
			build_type_instr(builder, OP_CAST, left.value, pointer_int_type);
		IrValue right_int =
			build_type_instr(builder, OP_CAST, right.value, pointer_int_type);
		IrValue diff = build_binary_instr(builder, OP_SUB, left_int, right_int);
		IrValue cast = build_type_instr(
				builder, OP_CAST, diff, c_type_to_ir_type(result_c_type));
		IrValue scaled = build_binary_instr(
				builder,
				OP_DIV,
				cast,
				value_const(cast.type,
					size_of_ir_type(c_type_to_ir_type(pointee_type))));

		return (Term) {
			.ctype = result_c_type,
			.value = scaled,
		};
	} else if (left_is_pointer && (right.ctype->t == INTEGER_TYPE)) {
		// @TODO: This block is almost identical to the corresponding block in
		// ir_gen_add, except for OP_SUB instead of OP_ADD. Factor out?
		assert(right.ctype->t == INTEGER_TYPE);

		CType *result_type = left.ctype;
		CType *pointee_type = result_type->u.pointee_type;

		// @TODO: Determine type correctly
		// @TODO: Don't hardcode in the size of a pointer!
		IrType pointer_int_type = (IrType) { .t = IR_INT, .u.bit_width = 64 };

		IrValue zext =
			build_type_instr(builder, OP_ZEXT, right.value, pointer_int_type);
		IrValue ptr_to_int =
			build_type_instr(builder, OP_CAST, left.value, pointer_int_type);
		IrValue subtrahend = build_binary_instr(
				builder,
				OP_MUL,
				zext,
				value_const(pointer_int_type,
					size_of_ir_type(c_type_to_ir_type(pointee_type))));

		IrValue sum = build_binary_instr(builder, OP_SUB, ptr_to_int, subtrahend);
		IrValue int_to_ptr = build_type_instr(builder, OP_CAST, sum,
				c_type_to_ir_type(result_type));

		return (Term) {
			.ctype = result_type,
			.value = int_to_ptr,
		};
	} else {
		UNIMPLEMENTED;
	}
}

static Term ir_gen_binary_operator(IrBuilder *builder, Env *env, Term left,
		Term right, IrOp ir_op)
{
	if (ir_op == OP_ADD)
		return ir_gen_add(builder, env, left, right);
	if (ir_op == OP_SUB)
		return ir_gen_sub(builder, env, left, right);

	if (left.ctype->t == ARRAY_TYPE) {
		left.ctype = decay_to_pointer(&env->type_env, left.ctype);
	}
	if (right.ctype->t == ARRAY_TYPE) {
		right.ctype = decay_to_pointer(&env->type_env, right.ctype);
	}

	// @TODO: Determine type correctly.
	CType *result_type = &env->type_env.int_type;

	if (!(ir_op == OP_EQ
				&& left.ctype->t == POINTER_TYPE
				&& right.ctype->t == POINTER_TYPE)) {
		// @TODO: Proper arithmetic conversions
		assert(left.ctype->t == INTEGER_TYPE && right.ctype->t == INTEGER_TYPE);
		if (rank(left.ctype) != rank(right.ctype)) {
			Term *to_convert = rank(left.ctype) < rank(right.ctype) ? &left : &right;
			CType *conversion_type =
				rank(left.ctype) < rank(right.ctype) ? right.ctype : left.ctype;
			IrType ir_type = c_type_to_ir_type(conversion_type);
			IrValue converted;
			if (to_convert->ctype->u.integer.is_signed) {
				converted =
					build_type_instr(builder, OP_SEXT, to_convert->value, ir_type);
			} else {
				converted =
					build_type_instr(builder, OP_ZEXT, to_convert->value, ir_type);
			}

			to_convert->value = converted;
			to_convert->ctype = conversion_type;
		}
	}

	IrValue value = build_binary_instr(builder, ir_op, left.value, right.value);

	return (Term) { .ctype = result_type, .value = value };
}

static Term ir_gen_binary_expr(IrBuilder *builder, Env *env, ASTExpr *expr,
		IrOp ir_op)
{
	return ir_gen_binary_operator(
			builder,
			env,
			ir_gen_expression(builder, env, expr->u.binary_op.arg1, RVALUE_CONTEXT),
			ir_gen_expression(builder, env, expr->u.binary_op.arg2, RVALUE_CONTEXT),
			ir_op);
}

static Term ir_gen_assign_op(IrBuilder *builder, Env *env, Term left,
		Term right, IrOp ir_op, Term *pre_assign_value)
{
	Term load = (Term) {
		.ctype = left.ctype,
		.value = build_load(builder, left.value, c_type_to_ir_type(left.ctype)),
	};
	Term result = ir_gen_binary_operator(builder, env, load, right, ir_op);
	build_store(builder,
			left.value,
			result.value,
			c_type_to_ir_type(result.ctype));

	if (pre_assign_value != NULL)
		*pre_assign_value = load;

	return result;
}

static Term ir_gen_assign_expr(IrBuilder *builder, Env *env, ASTExpr *expr,
		IrOp ir_op)
{
	Term left = ir_gen_expression(builder, env, expr->u.binary_op.arg1, LVALUE_CONTEXT);
	Term right = ir_gen_expression(builder, env, expr->u.binary_op.arg2, RVALUE_CONTEXT);
	return ir_gen_assign_op(builder, env, left, right, ir_op, NULL);
}

static Term ir_gen_deref(IrBuilder *builder, Term pointer, ExprContext context)
{
	// @TODO: Function which does this. Use for IDENTIFIER_EXPR as well.
	assert(pointer.ctype->t == POINTER_TYPE
			|| pointer.ctype->t == ARRAY_TYPE);
	CType *pointee_type = pointer.ctype->u.pointee_type;

	IrValue value;
	if (context == LVALUE_CONTEXT) {
		value = pointer.value;
	} else {
		assert(context == RVALUE_CONTEXT);

		value = build_load(
				builder,
				pointer.value,
				c_type_to_ir_type(pointee_type));
	}

	return (Term) { .ctype = pointee_type, .value = value };
}

static Term ir_gen_expression(IrBuilder *builder, Env *env, ASTExpr *expr,
		ExprContext context)
{
	IGNORE(builder);

	if (context == LVALUE_CONTEXT) {
		ASTExprType t = expr->t;
		assert(t == IDENTIFIER_EXPR
				|| t == STRUCT_DOT_FIELD_EXPR
				|| t == STRUCT_ARROW_FIELD_EXPR
				|| t == INDEX_EXPR
				|| t == DEREF_EXPR);
	}

	switch (expr->t) {
	case IDENTIFIER_EXPR: {
		Binding *binding = binding_for_name(env->scope, expr->u.identifier);
		assert(binding != NULL);
		IrValue value;
		IrType ir_type = c_type_to_ir_type(binding->term.ctype);

		// Functions, arrays, and structs implicitly have their address taken.
		if (context == LVALUE_CONTEXT
				|| binding->term.ctype->t == FUNCTION_TYPE
				|| binding->term.ctype->t == ARRAY_TYPE
				|| binding->term.ctype->t == STRUCT_TYPE) {
			value = binding->term.value;
		} else {
			assert(context == RVALUE_CONTEXT);
			value = build_load(
					builder,
					binding->term.value,
					ir_type);
		}

		return (Term) { .ctype = binding->term.ctype, .value = value };
	}
	case STRUCT_ARROW_FIELD_EXPR: {
		ASTExpr *struct_expr = expr->u.struct_field.struct_expr;
		Term struct_term =
			ir_gen_expression(builder, env, struct_expr, RVALUE_CONTEXT);
		assert(struct_term.ctype->t == POINTER_TYPE);
		assert(struct_term.ctype->u.pointee_type->t == STRUCT_TYPE);

		return ir_gen_struct_field(builder, struct_term,
				expr->u.struct_field.field_name, context);
	}
	case STRUCT_DOT_FIELD_EXPR: {
		ASTExpr *struct_expr = expr->u.struct_field.struct_expr;
		Term struct_term =
			ir_gen_expression(builder, env, struct_expr, RVALUE_CONTEXT);
		assert(struct_term.ctype->t == STRUCT_TYPE);

		return ir_gen_struct_field(builder, struct_term,
				expr->u.struct_field.field_name, context);
	}
	case ADDRESS_OF_EXPR: {
		ASTExpr *inner_expr = expr->u.unary_arg;
		Term ptr = ir_gen_expression(builder, env, inner_expr, LVALUE_CONTEXT);
		ptr.ctype = pointer_type(&env->type_env, ptr.ctype);

		return ptr;
	}
	case DEREF_EXPR: {
		ASTExpr *inner_expr = expr->u.unary_arg;
		Term pointer = ir_gen_expression(builder, env, inner_expr, RVALUE_CONTEXT);
		return ir_gen_deref(builder, pointer, context);
	}
	case INDEX_EXPR: {
		Term pointer = ir_gen_add(
				builder,
				env,
				ir_gen_expression(builder, env, expr->u.binary_op.arg1, RVALUE_CONTEXT),
				ir_gen_expression(builder, env, expr->u.binary_op.arg2, RVALUE_CONTEXT));
		assert(pointer.ctype->t == POINTER_TYPE);
		return ir_gen_deref(builder, pointer, context);
	}
	case INT_LITERAL_EXPR: {
		// @TODO: Determine types of constants correctly.
		CType *result_type = &env->type_env.int_type;

		IrValue value = value_const(
				(IrType) { .t = IR_INT, .u.bit_width = 32 },
				expr->u.int_literal);

		return (Term) { .ctype = result_type, .value = value };
	}
	// @TODO: Deduplicate identical string literals
	case STRING_LITERAL_EXPR: {
		char fmt[] = "__string_literal_%x";

		// - 2 adjusts down for the "%x" which isn't present in the output
		// sizeof(u32) * 2 is the max length of globals.size in hex
		// + 1 for the null terminator
		u32 name_max_length = sizeof fmt - 2 + sizeof(u32) * 2 + 1;
		char *name = pool_alloc(&builder->trans_unit->pool, name_max_length);
		snprintf(name, name_max_length, fmt, builder->trans_unit->globals.size);

		char *string = expr->u.string_literal;
		u32 length = strlen(string) + 1;
		CType *result_type =
			array_type(builder, &env->type_env, &env->type_env.char_type, length);
		IrType ir_type = c_type_to_ir_type(result_type);
		IrGlobal *global = trans_unit_add_var(builder->trans_unit, name, ir_type);
		global->linkage = IR_LOCAL_LINKAGE;

		IrInit *init = add_array_init(builder, ir_type);
		IrType ir_char_type = c_type_to_ir_type(&env->type_env.char_type);
		for (u32 i = 0; i < length; i++) {
			init->u.array_elems[i] = (IrInit) {
				.type = ir_char_type,
				.u.integer = string[i],
			};
		}

		init->type = ir_type;

		global->initializer = init;

		return (Term) { .ctype = result_type, .value = value_global(global) };
	}
	case ADD_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_ADD);
	case MINUS_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_SUB);
	case BIT_XOR_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_BIT_XOR);
	case BIT_AND_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_BIT_AND);
	case BIT_OR_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_BIT_OR);
	case BIT_NOT_EXPR: {
		// @TODO: Determine type correctly.
		CType *result_type = &env->type_env.int_type;
		Term term = ir_gen_expression(builder, env, expr->u.unary_arg, RVALUE_CONTEXT);

		return (Term) {
			.value = build_unary_instr(builder, OP_BIT_NOT, term.value),
			.ctype = result_type,
		};
	}
	case LOGICAL_NOT_EXPR: {
		CType *result_type = &env->type_env.int_type;
		Term term = ir_gen_expression(builder, env, expr->u.unary_arg, RVALUE_CONTEXT);

		return (Term) {
			.value = build_unary_instr(builder, OP_LOG_NOT, term.value),
			.ctype = result_type,
		};
	}
	case MULTIPLY_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_MUL);
	case DIVIDE_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_DIV);
	case EQUAL_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_EQ);
	case NOT_EQUAL_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_NEQ);
	case GREATER_THAN_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_GT);
	case GREATER_THAN_OR_EQUAL_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_GTE);
	case LESS_THAN_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_LT);
	case LESS_THAN_OR_EQUAL_EXPR: return ir_gen_binary_expr(builder, env, expr, OP_LTE);
	case ADD_ASSIGN_EXPR: return ir_gen_assign_expr(builder, env, expr, OP_ADD);
	case MINUS_ASSIGN_EXPR: return ir_gen_assign_expr(builder, env, expr, OP_SUB);
	case PRE_INCREMENT_EXPR: case POST_INCREMENT_EXPR: {
		Term ptr = ir_gen_expression(builder, env, expr->u.unary_arg, LVALUE_CONTEXT);
		// @TODO: Correct type
		CType *one_type = &env->type_env.int_type;
		Term one = (Term) {
			.value = value_const(c_type_to_ir_type(one_type), 1),
			.ctype = one_type,
		};
		Term pre_assign_value;
		Term incremented =
			ir_gen_assign_op(builder, env, ptr, one, OP_ADD, &pre_assign_value);

		if (expr->t == PRE_INCREMENT_EXPR) {
			return incremented;
		} else {
			return pre_assign_value;
		}
	}
	case BIT_XOR_ASSIGN_EXPR: return ir_gen_assign_expr(builder, env, expr, OP_BIT_XOR);
	case BIT_AND_ASSIGN_EXPR: return ir_gen_assign_expr(builder, env, expr, OP_BIT_AND);
	case BIT_OR_ASSIGN_EXPR: return ir_gen_assign_expr(builder, env, expr, OP_BIT_OR);
	case MULTIPLY_ASSIGN_EXPR: return ir_gen_assign_expr(builder, env, expr, OP_MUL);
	case DIVIDE_ASSIGN_EXPR: return ir_gen_assign_expr(builder, env, expr, OP_DIV);
	case FUNCTION_CALL_EXPR: {
		Term callee = ir_gen_expression(builder, env,
				expr->u.function_call.callee, RVALUE_CONTEXT);

		u32 arity = 0;
		ASTArgument *arg = expr->u.function_call.arg_list;
		while (arg != NULL) {
			arity++;
			arg = arg->next;
		}

		assert(callee.ctype->t == FUNCTION_TYPE);

		CType *return_type = callee.ctype->u.function.return_type;
		IrValue *arg_array = pool_alloc(&builder->trans_unit->pool,
				arity * sizeof(*arg_array));

		arg = expr->u.function_call.arg_list;
		for (u32 i = 0; arg != NULL; i++, arg = arg->next) {
			Term arg_term = ir_gen_expression(builder, env, arg->expr, RVALUE_CONTEXT);
			arg_array[i] = arg_term.value;
		}

		IrValue value = build_call(
				builder,
				callee.value,
				c_type_to_ir_type(return_type),
				arity,
				arg_array);

		return (Term) { .ctype = return_type, .value = value, };
	}
	case ASSIGN_EXPR: {
		ASTExpr *lhs = expr->u.binary_op.arg1;
		ASTExpr *rhs = expr->u.binary_op.arg2;

		Term lhs_ptr = ir_gen_expression(builder, env, lhs, LVALUE_CONTEXT);
		Term rhs_term = ir_gen_expression(builder, env, rhs, RVALUE_CONTEXT);

		build_store(
				builder,
				lhs_ptr.value,
				rhs_term.value,
				c_type_to_ir_type(lhs_ptr.ctype));
		return rhs_term;
	}
	case COMMA_EXPR:
		ir_gen_expression(builder, env, expr->u.binary_op.arg1, RVALUE_CONTEXT);
		return ir_gen_expression(builder, env, expr->u.binary_op.arg2, RVALUE_CONTEXT);
	case SIZEOF_TYPE_EXPR: {
		ASTDeclSpecifier *decl_specifier_list = expr->u.type->decl_specifier_list;
		ASTDeclarator *declarator = expr->u.type->declarator;

		CDecl cdecl;
		decl_to_cdecl(builder, &env->type_env, decl_specifier_list, declarator, &cdecl);

		// @TODO: This should be a size_t
		CType *result_type = &env->type_env.int_type;

		IrValue value = value_const(c_type_to_ir_type(result_type),
				size_of_ir_type(c_type_to_ir_type(cdecl.type)));

		return (Term) { .ctype = result_type, .value = value };
	}
	case SIZEOF_EXPR_EXPR: {
		ASTExpr *sizeof_expr = expr->u.unary_arg;

		u64 size;

		// @TODO: Do this more generally. We probably want a third ExprContext,
		// SIZEOF_CONTEXT. This would behave like RVALUE_CONTEXT, but not
		// actually generate any IR, just determine types.
		if (sizeof_expr->t == IDENTIFIER_EXPR) {
			Term term = ir_gen_expression(builder, env, sizeof_expr, LVALUE_CONTEXT);
			size = size_of_ir_type(c_type_to_ir_type(term.ctype));
		} else if (sizeof_expr->t == DEREF_EXPR) {
			ASTExpr *inner_expr = sizeof_expr->u.unary_arg;
			if (inner_expr->t != IDENTIFIER_EXPR)
				UNIMPLEMENTED;
			Term term = ir_gen_expression(builder, env, inner_expr, LVALUE_CONTEXT);
			assert(term.ctype->t == POINTER_TYPE);
			size = size_of_ir_type(c_type_to_ir_type(term.ctype->u.pointee_type));
		} else {
			UNIMPLEMENTED;
		}

		// @TODO: This should be a size_t
		CType *result_type = &env->type_env.int_type;
		IrValue value = value_const(c_type_to_ir_type(result_type), size);
		return (Term) { .ctype = result_type, .value = value };
	}
	default:
		printf("%d\n", expr->t);
		UNIMPLEMENTED;
	}
}
