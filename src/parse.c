#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "array.h"
#include "diagnostics.h"
#include "misc.h"
#include "parse.h"
#include "pool.h"
#include "tokenise.h"

typedef struct TypeTableEntry
{
	char *type_name;
} TypeTableEntry;

typedef struct TypeTable
{
	Array(TypeTableEntry) entries;
} TypeTable;

static char *builtin_types[] = {
	"void", "char", "short", "int", "long", "float", "double",
	"signed", "unsigned", "_Bool", "_Complex",
};

static void type_table_add_entry(TypeTable *table, TypeTableEntry entry)
{
	*ARRAY_APPEND(&table->entries, TypeTableEntry) = entry;
}

static void type_table_init(TypeTable *type_table)
{
	ARRAY_INIT(&type_table->entries, TypeTableEntry,
			STATIC_ARRAY_LENGTH(builtin_types));
	for (u32 i = 0; i < STATIC_ARRAY_LENGTH(builtin_types); i++) {
		TypeTableEntry entry = { .type_name = builtin_types[i] };
		type_table_add_entry(type_table, entry);
	}
}

static bool type_table_look_up_name(
		TypeTable *type_table, char *name, TypeTableEntry *out)
{
	for (u32 i = 0; i < type_table->entries.size; i++) {
		TypeTableEntry *entry = ARRAY_REF(&type_table->entries, TypeTableEntry, i);
		if (streq(entry->type_name, name)) {
			*out = *entry;
			return true;
		}
	}

	return false;
}

typedef struct Parser
{
	Pool *pool;

	Array(SourceToken) *tokens;
	u32 position;

	TypeTable defined_types;
} Parser;

// @TODO: Move the functions in this file that are only used by generated code. 
// They could either just be directly in the header produced by peg.py, or in
// a separate "support" file which is #included by the header.

static Token *read_token(Parser *parser)
{
	SourceToken *token = ARRAY_REF(parser->tokens, SourceToken, parser->position);
	parser->position++;

	return (Token *)token;
}

static inline void back_up(Parser *parser)
{
    parser->position--;
}

typedef struct ParserResult
{
	void *result;
	bool success;
} ParserResult;

static inline ParserResult success(void *result)
{
	return (ParserResult) { .result = result, .success = true };
}

static ParserResult failure = { .result = NULL, .success = false };

static inline ParserResult revert(Parser *parser, u32 position)
{
    parser->position = position;
    return failure;
}

static inline Token *current_token(Parser *parser)
{
	return (Token *)ARRAY_REF(parser->tokens, SourceToken, parser->position);
}

static inline SourceLoc *token_context(Token *token)
{
	return &((SourceToken *)token)->source_loc;
}


typedef struct WhichResult
{
	u32 which;
	void *result;
} WhichResult;

// @TODO: A lot of these build_* functions could probably be autogenerated too.

static inline void *middle(Parser *parser, void *a, void *b, void *c)
{
	IGNORE(parser); IGNORE(a); IGNORE(c);

	return b;
}

static inline void *first(Parser *parser, void *a, void *b)
{
	IGNORE(parser); IGNORE(b);

	return a;
}

static inline void *second(Parser *parser, void *a, void *b)
{
	IGNORE(parser); IGNORE(a);

	return b;
}

static inline void *ignore(Parser *parser, ...)
{
	IGNORE(parser);

	return NULL;
}

static ASTExpr *build_identifier(Parser *parser, Token *identifier_token)
{
	ASTExpr *identifier = pool_alloc(parser->pool, sizeof *identifier);
	identifier->type = IDENTIFIER_EXPR;
	identifier->val.identifier = identifier_token->val.symbol_or_string_literal;

	return identifier;
}

static ASTExpr *build_constant(Parser *parser, Token *token)
{
	ASTExpr *expr = pool_alloc(parser->pool, sizeof *expr);
	switch (token->type) {
	case TOK_INT_LITERAL:
		expr->type = INT_LITERAL_EXPR;
		expr->val.int_literal = token->val.int_literal;
		break;
	default:
		expr->type = STRING_LITERAL_EXPR;
		expr->val.string_literal = token->val.symbol_or_string_literal;
		break;
	}

	return expr;
}

static ASTExpr *build_postfix_expr(Parser *parser,
		ASTExpr *curr, WhichResult *which)
{
	ASTExpr *next = pool_alloc(parser->pool, sizeof *next);
	switch (which->which) {
	case 0:
		next->type = INDEX_EXPR;
		next->val.binary_op.arg1 = curr;
		next->val.binary_op.arg2 = which->result;
		return next;
	case 1:
		next->type = FUNCTION_CALL;
		next->val.function_call.callee = curr;
		next->val.function_call.args = which->result;
		return next;
	case 2:
		next->type = STRUCT_DOT_FIELD_EXPR;
		next->val.struct_field.struct_value = curr;
		next->val.struct_field.field_name = which->result;
		return next;
	case 3:
		next->type = STRUCT_ARROW_FIELD_EXPR;
		next->val.struct_field.struct_value = curr;
		next->val.struct_field.field_name = which->result;
		return next;
	case 4:
		next->type = POST_INCREMENT_EXPR;
		next->val.unary_arg = curr;
		return next;
	case 5:
		next->type = POST_DECREMENT_EXPR;
		next->val.unary_arg = curr;
		return next;
	default:
		UNREACHABLE;
	}

	return NULL;
}

static ASTExpr *build_compound_initializer(Parser *parser,
		void *a, void *b, void *c, void *d, void *e, void *f, void *g)
{
	/// @TODO
	IGNORE(parser);
	IGNORE(a); IGNORE(b); IGNORE(c); IGNORE(d); IGNORE(e); IGNORE(f); IGNORE(g);
	return NULL;
}


static ASTExpr *build_unary_expr(Parser *parser, Token *token,
		ASTExpr *arg)
{
	ASTExpr *next = pool_alloc(parser->pool, sizeof *next);
	next->val.unary_arg = arg;
	switch (token->type) {
	case TOK_INCREMENT: next->type = PRE_INCREMENT_EXPR; break;
	case TOK_DECREMENT: next->type = PRE_DECREMENT_EXPR; break;
	case TOK_AMPERSAND: next->type = ADDRESS_OF_EXPR; break;
	case TOK_ASTERISK: next->type = DEREF_EXPR; break;
	case TOK_PLUS: next->type = UNARY_PLUS_EXPR; break;
	case TOK_MINUS: next->type = UNARY_MINUS_EXPR; break;
	case TOK_BIT_NOT: next->type = BIT_NOT_EXPR; break;
	case TOK_LOGICAL_NOT: next->type = LOGICAL_NOT_EXPR; break;
	default: UNREACHABLE;
	}

	return next;
}

static ASTExpr *build_sizeof_expr(
		Parser *parser, Token *tok_sizeof, ASTExpr *arg)
{
	IGNORE(tok_sizeof);

	ASTExpr *sizeof_expr = pool_alloc(parser->pool, sizeof *sizeof_expr);
	sizeof_expr->type = SIZEOF_EXPR_EXPR;
	sizeof_expr->val.unary_arg = arg;

	return sizeof_expr;
}

static ASTExpr *build_sizeof_type(Parser *parser, Token *tok_sizeof,
		Token *lround, ASTTypeName *type, Token *rround)
{
	IGNORE(tok_sizeof);
	IGNORE(lround);
	IGNORE(rround);

	ASTExpr *sizeof_type = pool_alloc(parser->pool, sizeof *sizeof_type);
	sizeof_type->type = SIZEOF_TYPE_EXPR;
	sizeof_type->val.type = type;

	return sizeof_type;
}

static ASTExpr *build_cast_expr(Parser *parser, Token *lround,
		ASTTypeName *type, Token *rround, ASTExpr *arg)
{
	IGNORE(lround); IGNORE(rround);

	ASTExpr *cast_expr = pool_alloc(parser->pool, sizeof *cast_expr);
	cast_expr->type = CAST_EXPR;
	cast_expr->val.cast.cast_type = type;
	cast_expr->val.cast.arg = arg;

	return cast_expr;
}

typedef struct BinaryTail
{
	Token *operator;
	ASTExpr *tail_expr;
} BinaryTail;

static BinaryTail *build_binary_tail(Parser *parser,
		Token *operator, ASTExpr *tail_expr)
{
	BinaryTail *binary_tail = pool_alloc(parser->pool, sizeof *binary_tail);
	binary_tail->operator = operator;
	binary_tail->tail_expr = tail_expr;

	return binary_tail;
}

#define CASE2(token, ast_type) \
	case TOK_##token: expr->type = ast_type##_EXPR; break;
#define CASE1(operator) CASE2(operator, operator)

static ASTExpr *build_binary_head(Parser *parser, ASTExpr *curr,
		BinaryTail *tail)
{
	ASTExpr *expr = pool_alloc(parser->pool, sizeof *expr);
	expr->val.binary_op.arg1 = curr;
	expr->val.binary_op.arg2 = tail->tail_expr;

	switch (tail->operator->type) {
	CASE2(ASTERISK, MULTIPLY)
	CASE2(DIVIDE, DIVIDE)
	CASE1(MODULO)
	CASE2(PLUS, ADD)
	CASE1(MINUS)
	CASE1(LEFT_SHIFT)
	CASE1(RIGHT_SHIFT)
	CASE1(LESS_THAN)
	CASE1(GREATER_THAN)
	CASE1(LESS_THAN_OR_EQUAL)
	CASE1(GREATER_THAN_OR_EQUAL)
	CASE1(EQUAL)
	CASE1(NOT_EQUAL)
	CASE2(AMPERSAND, BIT_AND)
	CASE1(BIT_XOR)
	CASE1(BIT_OR)
	CASE1(LOGICAL_AND)
	CASE1(LOGICAL_OR)
	CASE1(ASSIGN)
	CASE1(MULT_ASSIGN)
	CASE1(DIVIDE_ASSIGN)
	CASE1(MODULO_ASSIGN)
	CASE1(PLUS_ASSIGN)
	CASE1(MINUS_ASSIGN)
	CASE1(LEFT_SHIFT_ASSIGN)
	CASE1(RIGHT_SHIFT_ASSIGN)
	CASE1(BIT_AND_ASSIGN)
	CASE1(BIT_XOR_ASSIGN)
	CASE1(BIT_OR_ASSIGN)
	CASE1(COMMA)

	default: UNREACHABLE;
	}

	return expr;
}

#undef CASE1
#undef CASE2

// @TODO: We actually want to use a fold for this, so we need
// build_ternary_head and build_ternary_tail.
static ASTExpr *build_conditional_expr(Parser *parser,
		ASTExpr *condition, Token *q, ASTExpr *then_expr,
		Token *colon, ASTExpr *else_expr)
{
	IGNORE(q);
	IGNORE(colon);

	ASTExpr *expr = pool_alloc(parser->pool, sizeof *expr);
	expr->type = CONDITIONAL_EXPR;
	expr->val.ternary_op.arg1 = condition;
	expr->val.ternary_op.arg2 = then_expr;
	expr->val.ternary_op.arg3 = else_expr;

	return expr;
	
}


ASTStatement *build_labeled_statement(Parser *parser, Token *label,
		Token *colon, ASTStatement *statement)
{
	IGNORE(colon);

	ASTStatement *labeled_statement =
		pool_alloc(parser->pool, sizeof *labeled_statement);
	labeled_statement->type = LABELED_STATEMENT;
	labeled_statement->val.labeled_statement.label_name =
		label->val.symbol_or_string_literal;
	labeled_statement->val.labeled_statement.statement = statement;
	return labeled_statement;
}

ASTStatement *build_case_statement(Parser *parser, Token *case_keyword,
		ASTExpr *case_value, Token *colon, ASTStatement *statement)
{
	IGNORE(case_keyword);
	IGNORE(colon);

	ASTStatement *case_statement = pool_alloc(parser->pool, sizeof *case_statement);
	case_statement->type = CASE_STATEMENT;
	case_statement->val.expr_and_statement.expr = case_value;
	case_statement->val.expr_and_statement.statement = statement;

	return case_statement;
}

ASTStatement *build_compound_statement(Parser *parser, Token *lcurly,
		ASTBlockItem *block_items, Token *rcurly)
{
	IGNORE(lcurly);
	IGNORE(rcurly);

	ASTStatement *compound_statement =
		pool_alloc(parser->pool, sizeof *compound_statement);
	compound_statement->type = COMPOUND_STATEMENT;
	compound_statement->val.block_items = block_items;

	return compound_statement;
}

ASTBlockItem *build_block_item(Parser *parser, WhichResult *decl_or_statement)
{
	ASTBlockItem *result = pool_alloc(parser->pool, sizeof *result);
	switch (decl_or_statement->which) {
	case 0:
		result->type = BLOCK_ITEM_DECL;
		result->val.decl = decl_or_statement->result;
		break;
	case 1:
		result->type = BLOCK_ITEM_STATEMENT;
		result->val.statement = decl_or_statement->result;
		break;
	default:
		UNREACHABLE;
	}

	return result;
}

ASTStatement *build_expr_statement(
		Parser *parser, ASTExpr *opt_expr, Token *semicolon)
{
	IGNORE(semicolon);

	ASTStatement *statement = pool_alloc(parser->pool, sizeof *statement);
	if (opt_expr == NULL) {
		statement->type = EMPTY_STATEMENT;
		return statement;
	}

	statement->type = EXPR_STATEMENT;
	statement->val.expr = opt_expr;
	return statement;
}

ASTStatement *build_if_statement(Parser *parser, Token *if_token, Token *lround,
		ASTExpr *condition, Token *rround, ASTStatement *then_statement,
		ASTStatement *opt_else_statement)
{
	IGNORE(if_token); IGNORE(lround); IGNORE(rround);

	ASTStatement *if_statement = pool_alloc(parser->pool, sizeof *if_statement);
	if_statement->type = IF_STATEMENT;
	if_statement->val.if_statement.condition = condition;
	if_statement->val.if_statement.then_statement = then_statement;
	// Potentially NULL if no else clause. This is fine as this is exactly what
	// a NULL 'else_statement' field indicates.
	if_statement->val.if_statement.else_statement = opt_else_statement;
	return if_statement;
}

ASTStatement *build_switch_statement(Parser *parser, Token *switch_token,
		Token *lround, ASTExpr *switch_expr, Token *rround, ASTStatement *body)
{
	IGNORE(switch_token); IGNORE(lround); IGNORE(rround);

	ASTStatement *switch_statement = pool_alloc(parser->pool, sizeof *switch_statement);
	switch_statement->type = SWITCH_STATEMENT;
	switch_statement->val.expr_and_statement.expr = switch_expr;
	switch_statement->val.expr_and_statement.statement = body;
	return switch_statement;
}

ASTStatement *build_while_statement(Parser *parser, Token *tok_while,
		Token *lround, ASTExpr *condition, Token *rround, ASTStatement *body)
{
	IGNORE(tok_while);
	IGNORE(lround);
	IGNORE(rround);

	ASTStatement *while_statement = pool_alloc(parser->pool, sizeof *while_statement);
	while_statement->type = WHILE_STATEMENT;
	while_statement->val.expr_and_statement.expr = condition;
	while_statement->val.expr_and_statement.statement = body;
	return while_statement;
}

ASTStatement *build_do_while_statement(Parser *parser, Token *tok_do,
		ASTStatement *body, Token *tok_while,
		Token *lround, ASTExpr *condition, Token *rround, Token *semi)
{
	IGNORE(tok_do); IGNORE(tok_while); IGNORE(lround); IGNORE(rround); IGNORE(semi);

	ASTStatement *do_while_statement =
		pool_alloc(parser->pool, sizeof *do_while_statement);
	do_while_statement->type = DO_WHILE_STATEMENT;
	do_while_statement->val.expr_and_statement.expr = condition;
	do_while_statement->val.expr_and_statement.statement = body;
	return do_while_statement;
}

ASTStatement *build_for_statement(Parser *parser, Token *keyword_for, Token *lround,
		ASTExpr *opt_init, Token *semi1, ASTExpr *opt_condition, Token *semi2,
		ASTExpr *opt_update, Token *rround, ASTStatement *body)
{
	IGNORE(keyword_for); IGNORE(lround); IGNORE(semi1); IGNORE(semi2); IGNORE(rround);

	ASTStatement *for_statement = pool_alloc(parser->pool, sizeof *for_statement);
	for_statement->type = FOR_STATEMENT;
	for_statement->val.for_statement.init_type = FOR_INIT_EXPR;
	for_statement->val.for_statement.init.expr = opt_init;
	for_statement->val.for_statement.condition = opt_condition;
	for_statement->val.for_statement.update_expr = opt_update;
	for_statement->val.for_statement.body = body;
	return for_statement;
}

// @TODO
ASTStatement *build_for_decl_statement(Parser *parser, Token *keyword_for,
		Token *lround, ASTDecl *init_decl, ASTExpr *opt_condition,
		Token *semi2, ASTExpr *opt_update, Token *rround, ASTStatement *body)
{
	IGNORE(keyword_for); IGNORE(lround); IGNORE(semi2); IGNORE(rround);
	ASTStatement *for_statement = pool_alloc(parser->pool, sizeof *for_statement);
	for_statement->type = FOR_STATEMENT;
	for_statement->val.for_statement.init_type = FOR_INIT_DECL;
	for_statement->val.for_statement.init.decl = init_decl;
	for_statement->val.for_statement.condition = opt_condition;
	for_statement->val.for_statement.update_expr = opt_update;
	for_statement->val.for_statement.body = body;

	return for_statement;
}

ASTStatement *build_goto_statement(Parser *parser, Token *tok_goto, Token *label)
{
	IGNORE(tok_goto);

	ASTStatement *goto_statement = pool_alloc(parser->pool, sizeof *goto_statement);
	goto_statement->type = GOTO_STATEMENT;
	goto_statement->val.goto_label = label->val.symbol_or_string_literal;
	return goto_statement;
}

ASTStatement *build_continue_statement(Parser *parser, Token *tok_cont, Token *semi)
{
	IGNORE(tok_cont);
	IGNORE(semi);

	ASTStatement *continue_statement =
		pool_alloc(parser->pool, sizeof *continue_statement);
	continue_statement->type = CONTINUE_STATEMENT;
	return continue_statement;
}

ASTStatement *build_break_statement(Parser *parser, Token *tok_break, Token *semi)
{
	IGNORE(tok_break);
	IGNORE(semi);

	ASTStatement *break_statement =
		pool_alloc(parser->pool, sizeof *break_statement);
	break_statement->type = BREAK_STATEMENT;
	return break_statement;
}

ASTStatement *build_return_statement(Parser *parser, Token *tok_return,
		ASTExpr *opt_expr, Token *semi)
{
	IGNORE(tok_return);
	IGNORE(semi);

	ASTStatement *return_statement =
		pool_alloc(parser->pool, sizeof *return_statement);
	return_statement->type = RETURN_STATEMENT;
	return_statement->val.expr = opt_expr;
	return return_statement;
}


static ASTToplevel *build_toplevel(Parser *parser, WhichResult *function_def_or_decl)
{
	ASTToplevel *toplevel = pool_alloc(parser->pool, sizeof *toplevel);
	switch (function_def_or_decl->which) {
	case 0:
		toplevel->type = FUNCTION_DEF;
		toplevel->val.function_def = function_def_or_decl->result;
		break;
	case 1:
		toplevel->type = DECL;
		toplevel->val.decl = function_def_or_decl->result;
		break;
	default:
		UNREACHABLE;
	}

	return toplevel;
}

static ASTDeclSpecifier *build_storage_class_specifier(Parser *parser, WhichResult *keyword)
{
	ASTDeclSpecifier *result = pool_alloc(parser->pool, sizeof *result);
	result->type = STORAGE_CLASS_SPECIFIER;

	ASTStorageClassSpecifier specifier;
	switch (keyword->which) {
	case 0: specifier = TYPEDEF_SPECIFIER; break;
	case 1: specifier = EXTERN_SPECIFIER; break;
	case 2: specifier = STATIC_SPECIFIER; break;
	case 3: specifier = AUTO_SPECIFIER; break;
	case 4: specifier = REGISTER_SPECIFIER; break;
	default: UNREACHABLE;
	}
	result->val.storage_class_specifier = specifier;

	return result;
}

static ASTDeclSpecifier *build_type_qualifier(Parser *parser, WhichResult *keyword)
{
	ASTDeclSpecifier *result = pool_alloc(parser->pool, sizeof *result);
	result->type = TYPE_QUALIFIER;

	ASTTypeQualifier qualifier;
	switch (keyword->which) {
	case 0: qualifier = CONST_QUALIFIER; break;
	case 1: qualifier = RESTRICT_QUALIFIER; break;
	case 2: qualifier = VOLATILE_QUALIFIER; break;
	default: UNREACHABLE;
	}
	result->val.type_qualifier = qualifier;

	return result;
}

static ASTDeclSpecifier *build_function_specifier(Parser *parser, Token *keyword)
{
	IGNORE(keyword);

	ASTDeclSpecifier *result = pool_alloc(parser->pool, sizeof *result);
	result->type = FUNCTION_SPECIFIER;
	result->val.function_specifier = INLINE_SPECIFIER;

	return result;
}

// @TODO: We currently don't add anything to the type table apart from builtin
// types. We need to add typedefs and named tagged types as we go.
static ParserResult named_type(Parser *parser)
{
	Token *token = read_token(parser);
	if (token->type != TOK_SYMBOL) {
		back_up(parser);
		return failure;
	}

	char *name = token->val.symbol_or_string_literal;
	TypeTableEntry entry;
	if (!type_table_look_up_name(&parser->defined_types, name, &entry)) {
		back_up(parser);
		return failure;
	}

	return success(token);
}

ASTTypeSpecifier *build_struct_or_union_tagged_named_type(
		Parser *parser, WhichResult *keyword, Token *name)
{
	ASTTypeSpecifier *tagged_type = pool_alloc(parser->pool, sizeof *tagged_type);
	tagged_type->type = keyword->which == 0 ?
		STRUCT_TYPE_SPECIFIER :
		UNION_TYPE_SPECIFIER;
	tagged_type->val.struct_or_union_specifier.name = name->val.symbol_or_string_literal;
	tagged_type->val.struct_or_union_specifier.fields = NULL;

	return tagged_type;
}

ASTTypeSpecifier *build_enum_tagged_named_type(
		Parser *parser, Token *keyword, Token *name)
{
	IGNORE(keyword);

	ASTTypeSpecifier *tagged_type = pool_alloc(parser->pool, sizeof *tagged_type);
	tagged_type->type = ENUM_TYPE_SPECIFIER;
	tagged_type->val.enum_specifier.name = name->val.symbol_or_string_literal;
	tagged_type->val.enum_specifier.enumerators = NULL;

	return tagged_type;
}

ASTTypeSpecifier *build_struct_or_union(Parser *parser, WhichResult *keyword,
		Token *opt_name, Token *lcurly, ASTFieldDecl *fields, Token *rcurly)
{
	IGNORE(lcurly);
	IGNORE(rcurly);

	ASTTypeSpecifier *result = pool_alloc(parser->pool, sizeof *result);
	result->type = keyword->which == 0 ?
		STRUCT_TYPE_SPECIFIER :
		UNION_TYPE_SPECIFIER;
	if (opt_name == NULL) {
		result->val.struct_or_union_specifier.name = NULL;
	} else {
		result->val.struct_or_union_specifier.name =
			opt_name->val.symbol_or_string_literal;
	}
	result->val.struct_or_union_specifier.fields = fields;

	return result;
}

ASTTypeSpecifier *build_enum(Parser *parser, Token *keyword, char *opt_name,
		Token *lcurly, ASTEnumerator *enumerators, Token *opt_comma, Token *rcurly)
{
	IGNORE(keyword); IGNORE(lcurly); IGNORE(opt_comma); IGNORE(rcurly);

	ASTTypeSpecifier *result = pool_alloc(parser->pool, sizeof *result);
	result->type = ENUM_TYPE_SPECIFIER;
	result->val.enum_specifier.name = opt_name;
	result->val.enum_specifier.enumerators = enumerators;

	return result;
}

// @TODO: This feels unnecessary. Couldn't we just have the parser keep
// wrapping the next thing in the input? This is complicated a bit because
// 'pointer' is currently a separate parser to the thing after it.
typedef struct PointerResult
{
	ASTDeclarator *first;
	ASTDeclarator *last;
} PointerResult;

PointerResult *build_next_pointer(Parser *parser, PointerResult *pointers,
		ASTDeclarator *pointer)
{
	IGNORE(parser);

	pointers->last->val.pointer_declarator.pointee = pointer;
	pointers->last = pointer;

	return pointers;
}

ASTDeclarator *build_pointee_declarator(Parser *parser, PointerResult *opt_pointer,
		ASTDirectDeclarator *declarator)
{
	ASTDeclarator *result = pool_alloc(parser->pool, sizeof *result);
	result->type = DIRECT_DECLARATOR;
	result->val.direct_declarator = declarator;

	if (opt_pointer == NULL)
		return result;

	opt_pointer->last->val.pointer_declarator.pointee = result;

	return opt_pointer->first;
}

ASTDeclarator *build_terminal_pointer(Parser *parser, PointerResult *pointer_result)
{
	IGNORE(parser);

	pointer_result->last->val.pointer_declarator.pointee = NULL;
	return pointer_result->first;
}

ASTDirectDeclarator *build_sub_declarator(Parser *parser,
		ASTDirectDeclarator *declarator,
		WhichResult *function_or_array_declarator)
{
	ASTDirectDeclarator *result = pool_alloc(parser->pool, sizeof *result);
	switch (function_or_array_declarator->which) {
	case 0:
		result->type = ARRAY_DECLARATOR;
		result->val.array_declarator.element_declarator = declarator;
		result->val.array_declarator.array_length = function_or_array_declarator->result;
		break;
	case 1:
		result->type = FUNCTION_DECLARATOR;
		result->val.function_declarator.declarator = declarator;
		result->val.function_declarator.parameters = function_or_array_declarator->result;
		break;
	default: UNREACHABLE;
	}

	return result;
}

#include "parse.inc"



// The input array consists of SourceTokens, but we treat them as Tokens most
// of the time.
ASTToplevel *parse_toplevel(Array(SourceToken) *tokens, Pool *ast_pool)
{
	Parser parser = { ast_pool, tokens, 0, { ARRAY_ZEROED } };
	type_table_init(&parser.defined_types);

	ParserResult result = translation_unit(&parser);
	if (parser.position != tokens->size) {
		if (_unexpected_token.type != TOK_INVALID) {
			issue_error(&_longest_parse_pos, "Unexpected token %s",
					token_type_names[_unexpected_token.type]);
		} else {
			SourceLoc s = { "<unknown>", 0, 0 };
			issue_error(&s, "Unknown error while parsing");
		}

		return NULL;
	}

	return result.result;
}


static int indent_level = 0;

static inline void print_indent(void)
{
	for (int n = 0; n < indent_level; n++)
		fputs("    ", stdout);
}

static void pretty_printf(char *fmt, ...)
{
	va_list varargs;
	va_start(varargs, fmt);

	for (int i = 0; fmt[i] != '\0'; i++) {
		char c = fmt[i];
		switch (c) {
		case '%':
			i++;
			assert(fmt[i] != '\0');

			switch (fmt[i]) {
			case 's':;
				// @NOTE: We assume we never need to do any formatting of
				// stuff printed by '%s', as usually this is just identifiers
				// and stuff, no control characters we'd indent based on.
				char *str = va_arg(varargs, char *);
				fputs(str, stdout);
				break;
			case '8':;
				uint64_t x = va_arg(varargs, uint64_t);
				printf("%" PRIu64, x);
				break;
			default:
				UNIMPLEMENTED;
			}
			break;
		case '(':
			puts("(");
			indent_level++;
			print_indent();

			break;
		case ',':
			puts(",");
			print_indent();
			break;
		case ')':
			putchar('\n');
			indent_level--;
			print_indent();
			putchar(')');

			break;
		default:
			putchar(c);
			break;
		}

	}

	va_end(varargs);
}



static void dump_decl_specifiers(ASTDeclSpecifier *specifiers);
static void dump_declarator(ASTDeclarator *declarator);
static void dump_expr(ASTExpr *expr);

static void dump_type_name(ASTTypeName *type_name)
{
	pretty_printf("TYPE_NAME(");
	dump_decl_specifiers(type_name->decl_specifiers);
	pretty_printf(",");
	dump_declarator(type_name->declarator);
	pretty_printf(")");
}

static void dump_args(ASTArgument *args)
{
	while (args != NULL) {
		dump_expr(args->expr);
		pretty_printf(",");

		args = args->next;
	}
}

#define X(x) #x
static char *expr_type_names[] = {
	AST_EXPR_TYPES
};
#undef X

static void dump_expr(ASTExpr *expr)
{
	pretty_printf("%s(", expr_type_names[expr->type]);
	switch (expr->type) {
	case INT_LITERAL_EXPR:
		pretty_printf("%8", expr->val.int_literal);
		break;
	case STRING_LITERAL_EXPR:
		pretty_printf("%s", expr->val.string_literal);
		break;
	case IDENTIFIER_EXPR:
		pretty_printf("%s", expr->val.identifier);
		break;
	case STRUCT_DOT_FIELD_EXPR: case STRUCT_ARROW_FIELD_EXPR:
		dump_expr(expr->val.struct_field.struct_value);
		pretty_printf(",%s", expr->val.struct_field.field_name);
		break;
	case INDEX_EXPR: case POST_INCREMENT_EXPR: case POST_DECREMENT_EXPR:
	case PRE_INCREMENT_EXPR: case PRE_DECREMENT_EXPR: case ADDRESS_OF_EXPR:
	case DEREF_EXPR: case UNARY_PLUS_EXPR: case UNARY_MINUS_EXPR:
	case BIT_NOT_EXPR: case LOGICAL_NOT_EXPR: case SIZEOF_EXPR_EXPR:
		dump_expr(expr->val.unary_arg);
		break;
	case FUNCTION_CALL:
		dump_expr(expr->val.function_call.callee);
		pretty_printf(",ARGS(");
		dump_args(expr->val.function_call.args);
		pretty_printf(")");
		break;
	case CAST_EXPR:
		dump_type_name(expr->val.cast.cast_type);
		pretty_printf(",");
		dump_expr(expr->val.cast.arg);
		break;
	case SIZEOF_TYPE_EXPR:
		dump_type_name(expr->val.type);
		break;
	case MULTIPLY_EXPR: case DIVIDE_EXPR: case MODULO_EXPR: case ADD_EXPR:
	case MINUS_EXPR: case LEFT_SHIFT_EXPR: case RIGHT_SHIFT_EXPR:
	case LESS_THAN_EXPR: case GREATER_THAN_EXPR: case LESS_THAN_OR_EQUAL_EXPR:
	case GREATER_THAN_OR_EQUAL_EXPR: case EQUAL_EXPR: case NOT_EQUAL_EXPR:
	case BIT_AND_EXPR: case BIT_XOR_EXPR: case BIT_OR_EXPR: case LOGICAL_AND_EXPR:
	case LOGICAL_OR_EXPR: case ASSIGN_EXPR: case MULT_ASSIGN_EXPR:
	case DIVIDE_ASSIGN_EXPR: case MODULO_ASSIGN_EXPR: case PLUS_ASSIGN_EXPR:
	case MINUS_ASSIGN_EXPR: case LEFT_SHIFT_ASSIGN_EXPR:
	case RIGHT_SHIFT_ASSIGN_EXPR: case BIT_AND_ASSIGN_EXPR:
	case BIT_XOR_ASSIGN_EXPR: case BIT_OR_ASSIGN_EXPR: case COMMA_EXPR:
		dump_expr(expr->val.binary_op.arg1);
		pretty_printf(",");
		dump_expr(expr->val.binary_op.arg2);
		break;
	case CONDITIONAL_EXPR:
		dump_expr(expr->val.ternary_op.arg1);
		pretty_printf(",");
		dump_expr(expr->val.ternary_op.arg2);
		pretty_printf(",");
		dump_expr(expr->val.ternary_op.arg3);
		break;
	default:
		printf("\n\nGot unknown expr type %d\n", expr->type);
		UNREACHABLE;
	}

	pretty_printf(")");
}

#define X(x) #x
static char *statement_type_names[] = {
	AST_STATEMENT_TYPES
};
#undef X

static void dump_decls(ASTDecl *decls);

static void dump_statement(ASTStatement *statement)
{
	pretty_printf("%s(", statement_type_names[statement->type]);
	switch (statement->type) {
	case EMPTY_STATEMENT:
	case CONTINUE_STATEMENT:
	case BREAK_STATEMENT:
		break;
	case LABELED_STATEMENT:
		pretty_printf("%s,", statement->val.labeled_statement.label_name);
		break;
	case COMPOUND_STATEMENT: {
		ASTBlockItem *block_item = statement->val.block_items;
		while (block_item != NULL) {
			switch (block_item->type) {
			case BLOCK_ITEM_STATEMENT:
				pretty_printf("BLOCK_ITEM_STATEMENT(");
				dump_statement(block_item->val.statement);
				break;
			case BLOCK_ITEM_DECL:
				pretty_printf("BLOCK_ITEM_DECL(");
				dump_decls(block_item->val.decl);
				break;
			}
			pretty_printf(")");

			if (block_item->next != NULL)
				pretty_printf(",");
			block_item = block_item->next;
		}
		break;
	}
	case EXPR_STATEMENT:
	case RETURN_STATEMENT:
		dump_expr(statement->val.expr);
		break;
	case IF_STATEMENT:
		dump_expr(statement->val.if_statement.condition);
		pretty_printf(",");
		dump_statement(statement->val.if_statement.then_statement);
		if (statement->val.if_statement.else_statement != NULL) {
			pretty_printf(",");
			dump_statement(statement->val.if_statement.else_statement);
		}
		break;
	case CASE_STATEMENT:
	case SWITCH_STATEMENT:
	case WHILE_STATEMENT:
	case DO_WHILE_STATEMENT:
		dump_expr(statement->val.expr_and_statement.expr);
		pretty_printf(",");
		dump_statement(statement->val.expr_and_statement.statement);
		break;
	case FOR_STATEMENT:
		switch (statement->val.for_statement.init_type) {
		case FOR_INIT_EXPR:
			if (statement->val.for_statement.init.expr != NULL)
				dump_expr(statement->val.for_statement.init.expr);
			break;
		case FOR_INIT_DECL:
			dump_decls(statement->val.for_statement.init.decl);
			break;
		}
		pretty_printf(",");
		if (statement->val.for_statement.condition)
			dump_expr(statement->val.for_statement.condition);
		pretty_printf(",");
		if (statement->val.for_statement.update_expr != NULL)
			dump_expr(statement->val.for_statement.update_expr);
		break;
	case GOTO_STATEMENT:
		pretty_printf(statement->val.goto_label);
		break;
	default:
		UNIMPLEMENTED;
	}

	pretty_printf(")");
}

static void dump_field_declarators(ASTFieldDeclarator *field_declarators)
{
	while (field_declarators != NULL) {
		switch (field_declarators->type) {
		case NORMAL_FIELD_DECLARATOR:
			pretty_printf("NORMAL_FIELD_DECLARATOR(");
			dump_declarator(field_declarators->val.declarator);
			pretty_printf(")");
			break;
		case BITFIELD_FIELD_DECLARATOR:
			pretty_printf("BITFIELD_DECLARATOR(");
			dump_declarator(field_declarators->val.bitfield.declarator);
			pretty_printf(",");
			dump_expr(field_declarators->val.bitfield.width);
			pretty_printf(")");
			break;
		}

		if (field_declarators->next != NULL)
			pretty_printf(",");
		field_declarators = field_declarators->next;
	}
}

static void dump_struct_or_union_fields(ASTFieldDecl *fields)
{
	while (fields != NULL) {
		pretty_printf("FIELD(");
		if (fields->decl_specifiers != NULL) {
			dump_decl_specifiers(fields->decl_specifiers);
			pretty_printf(",");
		}

		pretty_printf("FIELD_DECLARATORS(");
		dump_field_declarators(fields->field_declarators);
		pretty_printf("))");

		if (fields->next != NULL)
			pretty_printf(",");
		fields = fields->next;
	}
}

static void dump_type_specifier(ASTTypeSpecifier *type_specifier)
{
	switch (type_specifier->type) {
	case NAMED_TYPE_SPECIFIER:
		pretty_printf("NAMED_TYPE_SPECIFIER(%s", type_specifier->val.name);
		break;
	case STRUCT_TYPE_SPECIFIER:
		pretty_printf("STRUCT_TYPE_SPECIFIER(");

		char *name = type_specifier->val.struct_or_union_specifier.name;
		if (name != NULL)
			pretty_printf("%s,", name);

		pretty_printf("STRUCT_FIELDS(");
		dump_struct_or_union_fields(type_specifier->val.struct_or_union_specifier.fields);
		pretty_printf(")");
		break;
	default:
		UNIMPLEMENTED;
	}

	pretty_printf(")");
}

static void dump_decl_specifiers(ASTDeclSpecifier *specifiers)
{
	pretty_printf("DECL_SPECIFIER(");

#define CASE(x) case x: pretty_printf(#x); break;
	while (specifiers != NULL) {
		switch (specifiers->type) {
		case STORAGE_CLASS_SPECIFIER:
			switch (specifiers->val.storage_class_specifier) {
			CASE(TYPEDEF_SPECIFIER) CASE(EXTERN_SPECIFIER)
			CASE(STATIC_SPECIFIER) CASE(AUTO_SPECIFIER) CASE(REGISTER_SPECIFIER)
			}
			break;
		case TYPE_QUALIFIER:
			switch (specifiers->val.type_qualifier) {
			CASE(CONST_QUALIFIER) CASE(RESTRICT_QUALIFIER) CASE(VOLATILE_QUALIFIER)
			}
			break;
#undef CASE
		case FUNCTION_SPECIFIER:
			assert(specifiers->val.function_specifier == INLINE_SPECIFIER);
			pretty_printf("INLINE_SPECIFIER");
			break;
		case TYPE_SPECIFIER:
			dump_type_specifier(specifiers->val.type_specifier);
			break;
		}

		if (specifiers->next != NULL)
			pretty_printf(",");

		specifiers = specifiers->next;
	}

	pretty_printf(")");
}

static void dump_declarator(ASTDeclarator *declarator);

static void dump_parameter_decls(ASTParameterDecl *param_decls)
{
	pretty_printf("PARAM_DECLS(");
	while (param_decls != NULL) {
		pretty_printf("PARAM(");
		dump_decl_specifiers(param_decls->decl_specifiers);
		pretty_printf(",");
		dump_declarator(param_decls->declarator);
		pretty_printf("),");

		param_decls = param_decls->next;
	}

	pretty_printf(")");
}

static void dump_direct_declarator(ASTDirectDeclarator *declarator)
{
	switch (declarator->type) {
	case DECLARATOR:
		pretty_printf("DECLARATOR(");
		dump_declarator(declarator->val.declarator);
		break;
	case IDENTIFIER_DECLARATOR:
		pretty_printf("IDENTIFIER_DECLARATOR(%s", declarator->val.name);
		break;
	case FUNCTION_DECLARATOR:
		pretty_printf("FUNCTION_DECLARATOR(");
		dump_direct_declarator(declarator->val.function_declarator.declarator);
		pretty_printf(",");
		dump_parameter_decls(declarator->val.function_declarator.parameters);
		break;
	case ARRAY_DECLARATOR:
		pretty_printf("ARRAY_DECLARATOR(");
		dump_direct_declarator(declarator->val.array_declarator.element_declarator);
		
		if (declarator->val.array_declarator.array_length != NULL) {
			pretty_printf(",");
			dump_expr(declarator->val.array_declarator.array_length);
		}
		break;
	}

	pretty_printf(")");
}

static void dump_declarator(ASTDeclarator *declarator)
{
	switch (declarator->type) {
	case POINTER_DECLARATOR:
		pretty_printf("POINTER_DECLARATOR(");
		dump_decl_specifiers(declarator->val.pointer_declarator.decl_specifiers);
		pretty_printf(",");
		if (declarator->val.pointer_declarator.pointee != NULL)
			dump_declarator(declarator->val.pointer_declarator.pointee);
		break;
	case DIRECT_DECLARATOR:
		pretty_printf("DIRECT_DECLARATOR(");
		dump_direct_declarator(declarator->val.direct_declarator);
		break;
	}

	pretty_printf(")");
}

static void dump_designators(ASTDesignator *designators)
{
	while (designators != NULL) {
		switch (designators->type) {
		case INDEX_DESIGNATOR:
			pretty_printf("INDEX_DESIGNATOR(");
			dump_expr(designators->val.index_expr);
			break;
		case FIELD_DESIGNATOR:
			pretty_printf("FIELD_DESIGNATOR(%s", designators->val.field_name);
			break;
		}
		pretty_printf(")");

		if (designators->next != NULL)
			pretty_printf(",");

		designators = designators->next;
	}

}

static void dump_initializer(ASTInitializer *initializer);

static void dump_initializer_elements(ASTInitializerElement *elements)
{
	while (elements != NULL) {
		pretty_printf("INITIALIZER_ELEMENT(");
		pretty_printf("DESIGNATORS(");
		dump_designators(elements->designators);
		pretty_printf("),INITIALIZER(");
		dump_initializer(elements->initializer);
		pretty_printf("))");

		if (elements->next != NULL)
			pretty_printf(",");

		elements = elements->next;
	}
}

static void dump_initializer(ASTInitializer *initializer)
{
	switch (initializer->type) {
	case EXPR_INITIALIZER:
		pretty_printf("EXPR_INITIALIZER(");
		dump_expr(initializer->val.expr);
		break;
	case BRACE_INITIALIZER:
		pretty_printf("BRACE_INITIALIZER(");
		dump_initializer_elements(initializer->val.initializer_elements);
		break;
	}

	pretty_printf(")");
}

static void dump_init_declarators(ASTInitDeclarator *init_declarators)
{
	while (init_declarators != NULL) {
		pretty_printf("INIT_DECLARATOR(");
		dump_declarator(init_declarators->declarator);

		if (init_declarators->initializer != NULL) {
			pretty_printf(",");
			dump_initializer(init_declarators->initializer);
		}

		pretty_printf(")");
		if (init_declarators->next != NULL)
			pretty_printf(",");

		init_declarators = init_declarators->next;
	}
}

static void dump_decls(ASTDecl *decls)
{
	while (decls != NULL) {
		pretty_printf("DECL(");
		dump_decl_specifiers(decls->decl_specifiers);
		pretty_printf(",");
		dump_init_declarators(decls->init_declarators);
		pretty_printf(")");

		if (decls->next != NULL)
			pretty_printf(",");

		decls = decls->next;
	}
}

void dump_toplevel(ASTToplevel *ast)
{
	assert(indent_level == 0);

	while (ast != NULL) {
		switch (ast->type) {
		case FUNCTION_DEF:
			pretty_printf("FUNCTION_DEF(");
			dump_decl_specifiers(ast->val.function_def->specifiers);
			pretty_printf(",");
			dump_declarator(ast->val.function_def->declarator);
			pretty_printf(",");
			dump_decls(ast->val.function_def->old_style_param_decls);
			pretty_printf(",");
			dump_statement(ast->val.function_def->body);
			break;
		case DECL:
			pretty_printf("DECLS(");
			dump_decls(ast->val.decl);
			break;

		default:
			UNIMPLEMENTED;
		}

		pretty_printf(")\n");

		ast = ast->next;
	}

	assert(indent_level == 0);
}
