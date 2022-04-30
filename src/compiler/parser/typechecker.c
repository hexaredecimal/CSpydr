#include "typechecker.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ast/ast.h"
#include "error/error.h"
#include "optimizer/constexpr.h"
#include "parser/validator.h"
#include "utils.h"
#include "codegen/codegen_utils.h"
#include "ast/ast_iterator.h"

static ASTNode_T* typecheck_arg_pass(ASTType_T* expected, ASTNode_T* received);
static void typecheck_call(ASTNode_T* call, va_list args);
static void typecheck_explicit_cast(ASTNode_T* cast, va_list args);
static void typecheck_assignment(ASTNode_T* assignment, va_list args);
static void typecheck_array_lit(ASTNode_T* a_lit, va_list args);

static const ASTIteratorList_T iterator = {
    .node_end_fns = {
        [ND_CALL] = typecheck_call,
        [ND_CAST] = typecheck_explicit_cast,
        [ND_ASSIGN] = typecheck_assignment,
        [ND_ARRAY] = typecheck_array_lit
    }
};

void run_typechecker(ASTProg_T* ast)
{
    ast_iterate(&iterator, ast);
}

static void typecheck_call(ASTNode_T* call, va_list args)
{
    ASTType_T* call_type = unpack(call->expr->data_type);
    size_t expected_arg_num = call_type->arg_types->size;
    for(size_t i = 0; i < expected_arg_num; i++)
        call->args->items[i] = typecheck_arg_pass(call_type->arg_types->items[i], call->args->items[i]);
}

static void typecheck_assignment(ASTNode_T* assignment, va_list args)
{
    if(types_equal(assignment->left->data_type, assignment->right->data_type))
        return;

    if(implicitly_castable(assignment->tok, assignment->right->data_type, assignment->left->data_type))
    {
        assignment->right = implicit_cast(assignment->tok, assignment->right, assignment->left->data_type);
        return;
    }

    if(unpack(assignment->right->data_type)->kind == TY_ARRAY &&
        unpack(assignment->left->data_type)->kind == TY_VLA)
    {
        
    }

    char buf1[BUFSIZ] = {};
    char buf2[BUFSIZ] = {};
    throw_error(ERR_TYPE_ERROR_UNCR, assignment->tok, "assignment type missmatch: cannot assign `%s` to `%s`", 
        ast_type_to_str(buf1, assignment->right->data_type, LEN(buf1)),
        ast_type_to_str(buf2, assignment->left->data_type, LEN(buf2))    
    );
}

static ASTNode_T* typecheck_arg_pass(ASTType_T* expected, ASTNode_T* received)
{
    if(types_equal(expected, received->data_type))
        return received;
    
    if(implicitly_castable(received->tok, received->data_type, expected))
        return implicit_cast(received->tok, received, expected);
    
    char buf1[BUFSIZ] = {};
    char buf2[BUFSIZ] = {};
    throw_error(ERR_TYPE_ERROR_UNCR, received->tok, "cannot implicitly cast from `%s` to `%s`", 
        ast_type_to_str(buf1, received->data_type, LEN(buf1)),
        ast_type_to_str(buf2, expected, LEN(buf2))    
    );

    return received;
}

static void typecheck_explicit_cast(ASTNode_T* cast, va_list args)
{
    // Buffer for warnings and errors
    char buf1[BUFSIZ] = {};

    if(types_equal(cast->left->data_type, cast->data_type))
    {
        throw_error(ERR_TYPE_CAST_WARN, cast->tok, "unnecessary type cast: expression is already of type `%s`",
            ast_type_to_str(buf1, cast->data_type, LEN(buf1))
        );
        return;
    }

    ASTType_T* from = unpack(cast->left->data_type);
    ASTType_T* to = unpack(cast->data_type);

    if(from->kind == TY_VOID)
        throw_error(ERR_TYPE_ERROR_UNCR, cast->tok, "cannot cast from `void` to `%s`", 
            ast_type_to_str(buf1, cast->data_type, LEN(buf1))
        );
    
    if(to->kind == TY_VOID)
        throw_error(ERR_TYPE_ERROR_UNCR, cast->tok, "cannot cast from `%s` to `void`", 
            ast_type_to_str(buf1, cast->left->data_type, LEN(buf1))
        );
}

static void typecheck_array_lit(ASTNode_T* a_lit, va_list args)
{
    ASTType_T* base_ty = unpack(a_lit->data_type->base);
    
    char buf1[BUFSIZ] = {'\0'};
    char buf2[BUFSIZ] = {'\0'};

    for(size_t i = 0; i < a_lit->args->size; i++)
    {
        ASTNode_T* arg = a_lit->args->items[i];
        if(implicitly_castable(arg->tok, arg->data_type, base_ty))
            a_lit->args->items[i] = implicit_cast(arg->tok, arg, base_ty);
        else
            throw_error(ERR_TYPE_ERROR_UNCR, arg->tok, "cannot implicitly cast from `%s` to `%s`",
                ast_type_to_str(buf1, arg->data_type, BUFSIZ),
                buf2[0] == '\0' ? buf2 : ast_type_to_str(buf2, base_ty, BUFSIZ)
            );
    }
}

bool types_equal(ASTType_T* a, ASTType_T* b)
{
    if(!a || !b || a->kind != b->kind || a->is_constant != b->is_constant)
        return false;
    
    switch(a->kind)
    {
        case TY_C_ARRAY:
        case TY_PTR:
            return types_equal(a->base, b->base);
        
        case TY_STRUCT:
            if(a->members->size != b->members->size || a->is_union != b->is_union)
                return false;
            for(size_t i = 0; i < a->members->size; i++)
            {
                ASTNode_T* am = a->members->items[i];
                ASTNode_T* bm = b->members->items[i];
                if(!identifiers_equal(am->id, bm->id) || !types_equal(am->data_type, bm->data_type))
                    return false;
            }
            return true;
        
        case TY_ENUM:
            if(a->members->size != b->members->size)
                return false;
            for(size_t i = 0; i < a->members->size; i++)
            {
                ASTObj_T* am = a->members->items[i];
                ASTObj_T* bm = b->members->items[i];
                if(!identifiers_equal(am->id, bm->id) || const_i64(am->value) != const_i64(bm->value))
                    return false;
            }
            return true;

        case TY_UNDEF:
            return identifiers_equal(a->id, b->id);
        
        case TY_FN:
            if(a->arg_types->size != b->arg_types->size || !types_equal(a->base, b->base))
                return false;
            for(size_t i = 0; i < a->arg_types->size; i++)
                if(!types_equal(a->arg_types->items[i], b->arg_types->items[i]))
                    return false;
            return true;

        default:
            return true;
    }
}

bool implicitly_castable(Token_T* tok, ASTType_T* from, ASTType_T* to)
{
    from = unpack(from);
    to = unpack(to);

    if(!from || !to)
        return false;
    
    // Buffer for warnings and errors
    char buf1[BUFSIZ] = {};
    char buf2[BUFSIZ] = {};

    if(is_integer(from) && is_integer(to))
    {
        //if(from->size > to->size)
        //    throw_error(ERR_TYPE_CAST_WARN, tok, "implicitly casting from `%s` to `%s`: possible data loss",
        //        ast_type_to_str(buf1, from, LEN(buf1)),
        //        ast_type_to_str(buf2, to, LEN(buf2))
        //    );
        return true;
    }
    if(is_flonum(from) && is_flonum(to))
        return true;
    if(is_integer(from) && is_flonum(to))
        return true;
    if(is_flonum(from) && is_integer(to))
    {
        throw_error(ERR_TYPE_CAST_WARN, tok, "implicitly casting from `%s` to `%s`",
            ast_type_to_str(buf1, from, LEN(buf1)),
            ast_type_to_str(buf2, to, LEN(buf2))
        );
        return true;
    }
    if((from->kind == TY_PTR || from->kind == TY_C_ARRAY) && to->kind == TY_PTR)
        return true;
    if(from->kind == TY_ARRAY && to->kind == TY_VLA)
        return true;
    if(from->kind == TY_PTR && unpack(from->base)->kind == TY_ARRAY && to->kind == TY_VLA)
        return types_equal(unpack(from->base)->base, to->base);
    return false;
}

ASTNode_T* implicit_cast(Token_T* tok, ASTNode_T* expr, ASTType_T* to)
{
    if(unpack(expr->data_type)->kind == TY_ARRAY && to->kind == TY_VLA)
    {
        ASTNode_T* ref = init_ast_node(ND_REF, tok);
        ref->data_type = to;
        ref->right = expr;
        return ref;
    }

    ASTNode_T* cast = init_ast_node(ND_CAST, tok);
    cast->data_type = to;
    cast->left = expr;
    return cast;
}