#include "parser.h"

#include "ast/ast.h"
#include "../util.h"
#include "config.h"
#include "error/error.h"
#include "lexer/token.h"
#include "list.h"
#include "validator.h"
#include "../io/log.h"
#include "../io/io.h"
#include "../ast/types.h"
#include "../mem/mem.h"
#include "../platform/platform_bindings.h"
#include "../lexer/lexer.h"
#include "../preprocessor/preprocessor.h"
#include "../toolchain.h"
#include "../codegen/codegen_utils.h"

#include <limits.h>
#include <string.h>
#include <float.h>

typedef struct PARSER_STRUCT
{
    List_T* tokens;
    size_t token_i;
    ASTProg_T* root_ref;
    Token_T* tok;
    ASTNode_T* cur_block;
    ASTObj_T* cur_fn;

    size_t cur_lambda_id;
    size_t cur_tuple_id;

    bool holes_enabled;
} Parser_T;

/////////////////////////////////
// expression parsing settings //
/////////////////////////////////

typedef ASTNode_T* (*PrefixParseFn_T)(Parser_T* parser);
typedef ASTNode_T* (*InfixParseFn_T)(Parser_T* parser, ASTNode_T* left);

typedef enum 
{
    LOWEST = 0,

    ASSIGN, // x = y
    PIPE, // x |> y |> z
    LOGIC_OR,  // x || y
    LOGIC_AND,  // x && y
    INFIX_CALL,  // x `y` z
    BIT_OR,  // x | y
    BIT_XOR,  // x ^ y
    BIT_AND,  // x & y
    EQUALS,  // x == y
    LT,  // x < y
    GT = LT,  // x > y
    BIT_SHIFT, // x << y
    PLUS, // x + y
    MINUS = PLUS, // x - y
    MULT, // x * y
    DIV = MULT, // x / y
    MOD, // x % y
    POWER, // x²
    INC, // x--
    DEC = INC, // x++
    X_OF, // alignof x, sizeof x, typeof x
    CAST, // x: y
    CALL, // x(y)
    ARRAY, // x[y]
    MEMBER, // x.y

    HIGHEST
} Precedence_T;

static ASTNode_T* parse_id(Parser_T* p);
static ASTNode_T* parse_int_lit(Parser_T* p);
static ASTNode_T* parse_inline_asm(Parser_T* p);
static ASTNode_T* parse_float_lit(Parser_T* p);
static ASTNode_T* parse_char_lit(Parser_T* p);
static ASTNode_T* parse_bool_lit(Parser_T* p);
static ASTNode_T* parse_str_lit(Parser_T* p, bool keep_inline);
static ASTNode_T* parse_nil_lit(Parser_T* p);
static ASTNode_T* parse_closure(Parser_T* p);

static ASTNode_T* parse_array_lit(Parser_T* p);
static ASTNode_T* parse_struct_lit(Parser_T* p, ASTNode_T* id);
static ASTNode_T* parse_anonymous_struct_lit(Parser_T* p);

static ASTNode_T* parse_lambda_lit(Parser_T* p);
static ASTNode_T* parse_if_expr(Parser_T* p);

static ASTNode_T* parse_sizeof(Parser_T* p);
static ASTNode_T* parse_alignof(Parser_T* p);
static ASTNode_T* parse_len(Parser_T* p);
static ASTNode_T* parse_va_arg(Parser_T* p);

static ASTNode_T* parse_unary(Parser_T* p);
static ASTNode_T* parse_num_op(Parser_T* p, ASTNode_T* left);
static ASTNode_T* parse_bit_op(Parser_T* p, ASTNode_T* left);
static ASTNode_T* parse_bool_op(Parser_T* p, ASTNode_T* left);
static ASTNode_T* parse_assignment(Parser_T* p, ASTNode_T* left);
static ASTNode_T* parse_postfix(Parser_T* p, ASTNode_T* left);
static ASTNode_T* parse_index(Parser_T* p, ASTNode_T* left);

static ASTNode_T* parse_infix_call(Parser_T* p, ASTNode_T* left);

static ASTNode_T* parse_call(Parser_T* p, ASTNode_T* left);
static ASTNode_T* parse_cast(Parser_T* p, ASTNode_T* left);
static ASTNode_T* parse_member(Parser_T* p, ASTNode_T* left);
static ASTNode_T* parse_pipe(Parser_T* p, ASTNode_T* left);
static ASTNode_T* parse_hole(Parser_T* p);

static ASTNode_T* parse_pow_2(Parser_T* p, ASTNode_T* left);
static ASTNode_T* parse_pow_3(Parser_T* p, ASTNode_T* left);

static ASTNode_T* parse_current_fn_token(Parser_T* p);

static ASTType_T* parse_type(Parser_T* p);

static struct { 
    PrefixParseFn_T pfn; 
    InfixParseFn_T ifn; 
    Precedence_T prec; 
} expr_parse_fns[TOKEN_EOF + 1] = {
    [TOKEN_ID]       = {parse_id, NULL, LOWEST},
    [TOKEN_INT]      = {parse_int_lit, NULL, LOWEST},
    [TOKEN_ASM]      = {parse_inline_asm, NULL, LOWEST},
    [TOKEN_FLOAT]    = {parse_float_lit, NULL, LOWEST},
    [TOKEN_NIL]      = {parse_nil_lit, NULL, LOWEST},
    [TOKEN_TRUE]     = {parse_bool_lit, NULL, LOWEST},
    [TOKEN_FALSE]    = {parse_bool_lit, NULL, LOWEST},
    [TOKEN_CHAR]     = {parse_char_lit, NULL, LOWEST},
    [TOKEN_STRING]   = {(PrefixParseFn_T) parse_str_lit, NULL, LOWEST},
    [TOKEN_BANG]     = {parse_unary, NULL, LOWEST},
    [TOKEN_MINUS]    = {parse_unary, parse_num_op, MINUS},
    [TOKEN_LPAREN]   = {parse_closure, NULL, CALL}, 
    [TOKEN_LBRACKET] = {parse_array_lit, parse_index, ARRAY},   
    [TOKEN_LBRACE]   = {parse_anonymous_struct_lit, NULL, LOWEST}, 
    [TOKEN_STAR]     = {parse_unary, parse_num_op, MULT},
    [TOKEN_PERCENT]  = {NULL, parse_num_op, DIV},
    [TOKEN_MOD]      = {NULL, parse_assignment, ASSIGN},
    [TOKEN_REF]      = {parse_unary, parse_bit_op, BIT_AND},
    [TOKEN_TILDE]    = {parse_unary, NULL, LOWEST},
    [TOKEN_PLUS]     = {NULL, parse_num_op, PLUS},    
    [TOKEN_SLASH]    = {NULL, parse_num_op, DIV},    
    [TOKEN_EQ]       = {NULL, parse_bool_op, EQUALS}, 
    [TOKEN_NOT_EQ]   = {NULL, parse_bool_op, EQUALS},     
    [TOKEN_GT]       = {NULL, parse_bool_op, GT}, 
    [TOKEN_GT_EQ]    = {NULL, parse_bool_op, GT},    
    [TOKEN_LT]       = {NULL, parse_bool_op, LT}, 
    [TOKEN_LT_EQ]    = {NULL, parse_bool_op, LT},          
    [TOKEN_OR]       = {parse_lambda_lit, parse_bool_op, LOGIC_OR},
    [TOKEN_AND]      = {NULL, parse_bool_op, LOGIC_AND}, 
    [TOKEN_INC]      = {NULL, parse_postfix, INC},  
    [TOKEN_DEC]      = {NULL, parse_postfix, DEC},  
    [TOKEN_ASSIGN]   = {NULL, parse_assignment, ASSIGN},
    [TOKEN_ADD]      = {NULL, parse_assignment, ASSIGN},  
    [TOKEN_SUB]      = {NULL, parse_assignment, ASSIGN},  
    [TOKEN_DIV]      = {NULL, parse_assignment, ASSIGN},  
    [TOKEN_MULT]     = {NULL, parse_assignment, ASSIGN},   
    [TOKEN_DOT]      = {NULL, parse_member, MEMBER},
    [TOKEN_COLON]    = {NULL, parse_cast, CAST},
    [TOKEN_SIZEOF]   = {parse_sizeof, NULL, X_OF},
    [TOKEN_ALIGNOF]  = {parse_alignof, NULL, X_OF},
    [TOKEN_LEN]      = {parse_len, NULL, LOWEST},
    [TOKEN_POW_2]    = {NULL, parse_pow_2, POWER},
    [TOKEN_POW_3]    = {NULL, parse_pow_3, POWER},
    [TOKEN_BIT_OR]   = {parse_lambda_lit, parse_bit_op, BIT_OR},
    [TOKEN_LSHIFT]   = {NULL, parse_bit_op, BIT_SHIFT},
    [TOKEN_RSHIFT]   = {NULL, parse_bit_op, BIT_SHIFT},
    [TOKEN_XOR]      = {NULL, parse_bit_op, BIT_XOR},
    [TOKEN_PIPE]     = {NULL, parse_pipe, PIPE},
    [TOKEN_DOLLAR]   = {parse_hole, NULL, LOWEST},
    [TOKEN_LSHIFT_ASSIGN] = {NULL, parse_assignment, ASSIGN},
    [TOKEN_RSHIFT_ASSIGN] = {NULL, parse_assignment, ASSIGN},
    [TOKEN_XOR_ASSIGN] = {NULL, parse_assignment, ASSIGN},
    [TOKEN_BIT_AND_ASSIGN] = {NULL, parse_assignment, ASSIGN},
    [TOKEN_BIT_OR_ASSIGN] = {NULL, parse_assignment, ASSIGN},  
    [TOKEN_INFIX_CALL] = {NULL, parse_infix_call, INFIX_CALL},
    [TOKEN_STATIC_MEMBER] = {parse_id, NULL, LOWEST},
    [TOKEN_IF]       = {parse_if_expr, NULL, LOWEST},
    [TOKEN_CURRENT_FN] = {parse_current_fn_token, NULL, LOWEST},
}; 

static ASTNodeKind_T unary_ops[TOKEN_EOF + 1] = {
    [TOKEN_MINUS] = ND_NEG,
    [TOKEN_BANG]  = ND_NOT,
    [TOKEN_TILDE] = ND_BIT_NEG,
    [TOKEN_REF]   = ND_REF,
    [TOKEN_STAR]  = ND_DEREF
};

static TokenType_T assign_to_op[TOKEN_EOF + 1] = {
    [TOKEN_RSHIFT_ASSIGN]  = TOKEN_RSHIFT,
    [TOKEN_LSHIFT_ASSIGN]  = TOKEN_LSHIFT,
    [TOKEN_XOR_ASSIGN]     = TOKEN_XOR,
    [TOKEN_BIT_OR_ASSIGN]  = TOKEN_BIT_OR,
    [TOKEN_BIT_AND_ASSIGN] = TOKEN_REF,
    [TOKEN_MOD]            = TOKEN_PERCENT,
    [TOKEN_ADD]            = TOKEN_PLUS,
    [TOKEN_SUB]            = TOKEN_MINUS,
    [TOKEN_MULT]           = TOKEN_STAR,
    [TOKEN_DIV]            = TOKEN_SLASH,
};

static ASTNodeKind_T infix_ops[TOKEN_EOF + 1] = {
    [TOKEN_MINUS] = ND_SUB,
    [TOKEN_PLUS]  = ND_ADD,
    [TOKEN_STAR]  = ND_MUL,
    [TOKEN_SLASH] = ND_DIV,

    [TOKEN_EQ]     = ND_EQ,
    [TOKEN_NOT_EQ] = ND_NE,
    [TOKEN_GT]     = ND_GT,
    [TOKEN_GT_EQ]  = ND_GE,
    [TOKEN_LT]     = ND_LT,
    [TOKEN_LT_EQ]  = ND_LE,

    [TOKEN_AND] = ND_AND,
    [TOKEN_OR]  = ND_OR,

    [TOKEN_ASSIGN] = ND_ASSIGN,
    [TOKEN_ADD]    = ND_ADD,    // is still an assignment!
    [TOKEN_SUB]    = ND_SUB,    // is still an assignment!
    [TOKEN_MULT]   = ND_MUL,    // is still an assignment!
    [TOKEN_DIV]    = ND_DIV,    // is still an assignment!

    [TOKEN_RSHIFT_ASSIGN] = ND_RSHIFT,
    [TOKEN_LSHIFT_ASSIGN] = ND_LSHIFT,
    [TOKEN_MOD] = ND_MOD,
    [TOKEN_XOR_ASSIGN] = ND_XOR,
    [TOKEN_BIT_OR_ASSIGN] = ND_BIT_OR,
    [TOKEN_BIT_AND_ASSIGN] = ND_BIT_AND,

    [TOKEN_LSHIFT] = ND_LSHIFT,
    [TOKEN_RSHIFT] = ND_RSHIFT,
    [TOKEN_XOR] = ND_XOR,
    [TOKEN_BIT_OR] = ND_BIT_OR,
    [TOKEN_REF] = ND_BIT_AND, 
    [TOKEN_PERCENT] = ND_MOD,

    [TOKEN_INC] = ND_INC,   // technically postfix operators, but get treated like infix ops internally
    [TOKEN_DEC] = ND_DEC    // technically postfix operators, but get treated like infix ops internally
};

static ASTObj_T alloca_bottom = {
    .kind = OBJ_LOCAL,
    .id = &(ASTIdentifier_T) {
        .callee = "__alloca_size__"
    },
    .data_type = &(ASTType_T){
        .size = sizeof(void*),
        .align = sizeof(void*)
    },
    .offset = 0
};

static inline PrefixParseFn_T get_PrefixParseFn_T(TokenType_T tt)
{
    return expr_parse_fns[tt].pfn;
}

static inline InfixParseFn_T get_InfixParseFn_T(TokenType_T tt)
{
    return expr_parse_fns[tt].ifn;
}

static inline Precedence_T get_precedence(TokenType_T tt)
{
    return expr_parse_fns[tt].prec;
}

/////////////////////////////////
// helperfunctions             //
/////////////////////////////////

static void init_parser(Parser_T* parser, List_T* tokens)
{
    memset(parser, 0, sizeof(struct PARSER_STRUCT));
    parser->tokens = tokens;
    parser->tok = tokens->items[0];
}

static void free_parser(Parser_T* p)
{
    // nothing to do here
}

static inline bool streq(char* s1, char* s2)
{
    return strcmp(s1, s2) == 0;
}

static inline Token_T* parser_advance(Parser_T* p)
{
    p->tok = p->tokens->items[++p->token_i];
    if(p->tok->type == TOKEN_SEMICOLON && streq(p->tok->value, ";"))
        throw_error(ERR_SYNTAX_WARNING, p->tok, "found `;` (greek question mark) instead of `;` (semicolon)");
    return p->tok;
}

static inline Token_T* parser_peek(Parser_T* p, i32 level)
{
    if(p->token_i + level >= p->tokens->size || p->token_i + level <= 0)
        return NULL;
    return p->tokens->items[p->token_i + level];
}

static inline bool tok_is(Parser_T* p, TokenType_T type)
{
    return p->tok->type == type;
}

Token_T* parser_consume(Parser_T* p, TokenType_T type, const char* msg)
{
    if(!tok_is(p, type))
        throw_error(ERR_SYNTAX_ERROR, p->tok, "unexpected token `%s`, %s", p->tok->value,  msg);

    return parser_advance(p);
}

static inline void parser_enable_holes(Parser_T* p)
{
    p->holes_enabled = true;
}

static inline void parser_disable_holes(Parser_T* p)
{
    p->holes_enabled = false;
}

static inline bool parser_holes_enabled(Parser_T* p)
{
    return p->holes_enabled;
}

static inline bool is_executable(ASTNode_T* n)
{
    if(n->kind == ND_CLOSURE)
        return is_executable(n->expr);
    if(n->kind == ND_PIPE)
        return is_executable(n->right);
    if(n->kind == ND_IF_EXPR)
        return is_executable(n->if_branch) && is_executable(n->else_branch);
    return n->kind == ND_CALL || n->kind == ND_ASSIGN || n->kind == ND_INC || n->kind == ND_DEC || n->kind == ND_CAST || n->kind == ND_MEMBER || n->kind == ND_ASM;
}

static bool check_type(ASTType_T* a, ASTType_T* b)
{
    if(a->kind != b->kind)
        return false;
    
    /*if(a->callee != NULL && b->callee != NULL && strcmp(a->callee, b->callee) != 0)
        return false;
    
    if((a->callee == NULL && b->callee != NULL) || (a->callee != NULL && b->callee == NULL))
        return false;*/
    
    if(a->is_primitive != b->is_primitive)
        return false;
    
    if((a->base == NULL && b->base != NULL) || (a->base != NULL && b->base == NULL))
        return false;

    if(a->base && b->base)
        return check_type(a->base, b->base);

    return true;
}

static ASTObj_T* get_compatible_tuple(Parser_T* p, ASTType_T* tuple)
{
    for(size_t i = 0; i < p->root_ref->objs->size; i++)
    {
        ASTObj_T* obj = p->root_ref->objs->items[i];
        if(obj->kind == OBJ_TYPEDEF && str_starts_with(obj->id->callee, "__csp_tuple_") && obj->data_type->members->size == tuple->members->size)
        {
            for(size_t j = 0; j < obj->data_type->members->size; j++)
            {
                if(!check_type(((ASTNode_T*) obj->data_type->members->items[j])->data_type, ((ASTNode_T*) tuple->members->items[j])->data_type))
                    goto cont;
            }
            return obj;
        }
    cont:
        ;
    }

    return NULL;
}

/////////////////////////////////
// Parser                      //
/////////////////////////////////

static void parse_obj(Parser_T* p, List_T* obj_list);
static void parse_compiler_directives(Parser_T* p, List_T* obj_list);

void parse(ASTProg_T* ast, List_T* files, bool is_silent)
{
    // get the main source file
    SrcFile_T* main_file = files->items[0];

    // initialize the lexer for the main file
    Lexer_T lex;
    init_lexer(&lex, main_file);

    List_T* tokens = lex_and_preprocess_tokens(&lex, files, is_silent);

    // initialize the parser;
    Parser_T parser;
    init_parser(&parser, tokens);

    if(!is_silent)
    {
        LOG_OK_F(COLOR_BOLD_GREEN "\33[2K\r  Compiling " COLOR_RESET " %s\n", main_file->path);
    }

    // initialize the main ast node
    init_ast_prog(ast, main_file->path, NULL, NULL);
    parser.root_ref = ast;

    // parse
    while(!tok_is(&parser, TOKEN_EOF))
    {
        switch(parser.tok->type)
        {
            case TOKEN_IMPORT:
                parser_advance(&parser);
                parser_consume(&parser, TOKEN_STRING, "expect file to import as string");
                parser_consume(&parser, TOKEN_SEMICOLON, "expect `;` after import statement");
                break;
            default: 
                parse_obj(&parser, ast->objs);
        }
    }

    // dispose
    free_list(tokens);
    free_parser(&parser);

    // check the ast for validity
    validate_ast(ast);
}

// function to quickly build string literals

ASTNode_T* build_str_lit(Token_T* tok, char* str, bool allocate_global, List_T* objs)
{
    ASTNode_T* str_lit = init_ast_node(ND_STR, tok);
    str_lit->is_constant = true;
    str_lit->data_type = (ASTType_T*) char_ptr_type;
    str_lit->str_val = str;
    mem_add_ptr(str);

    if(global.ct == CT_ASM && allocate_global)
    {
        static u64 i = 0;
        ASTIdentifier_T* ast_id = init_ast_identifier(str_lit->tok, (char[]){'\0'});
        sprintf(ast_id->callee, ".L.str.%ld", i++);

        ASTObj_T* globl = init_ast_obj(OBJ_GLOBAL, str_lit->tok);
        globl->id = ast_id;
        globl->value = str_lit;
        globl->data_type = init_ast_type(TY_ARR, str_lit->tok);
        globl->data_type->num_indices = init_ast_node(ND_LONG, str_lit->tok);
        globl->data_type->num_indices->long_val = strlen(str_lit->str_val) + 1;
        globl->data_type->base = (ASTType_T*) primitives[TY_CHAR];
        list_push(objs, globl);

        ASTNode_T* caller = init_ast_node(ND_ID, str_lit->tok);
        caller->id = ast_id;
        caller->referenced_obj = globl;
        caller->data_type = globl->data_type;//(ASTType_T*) char_ptr_type;

        return caller;
    }
    else
        return str_lit;
}

/////////////////////////////////
// Compiler Directives Parser  //
/////////////////////////////////

static void eval_compiler_directive(Parser_T* p, Token_T* field, char* value, List_T* obj_list)
{
    if(streq(field->value, "link"))
    {
        char* link_flag = calloc(strlen(value) + 3, sizeof(char));
        sprintf(link_flag, "-l%s", value);
        mem_add_ptr(link_flag);

        list_push(global.linker_flags, link_flag);
    }
    else if(streq(field->value, "link_dir"))
    {
        char* link_flag = calloc(strlen(value) + 3, sizeof(char));
        sprintf(link_flag, "-L%s", value);
        mem_add_ptr(link_flag);

        list_push(global.linker_flags, link_flag);
    }
    else if(streq(field->value, "link_obj"))
        list_push(global.linker_flags, value);
    else if(streq(field->value, "no_return"))
    {
        bool all = streq("*", value);
        for(size_t i = 0; i < obj_list->size; i++)
        {
            ASTObj_T* obj = obj_list->items[i];
            if(all)
            {
                if(obj->kind != OBJ_FUNCTION) 
                    continue;
                obj->no_return = true;
            }
            else if(streq(obj->id->callee, value))
            {
                if(obj->kind != OBJ_FUNCTION)
                    throw_error(ERR_TYPE_ERROR, p->tok, "`%s` is not a function, thus cannot have the `no_return` attribute", value);

                obj->no_return = true;
                return;
            }
        }
        
        if(!all)
            throw_error(ERR_SYNTAX_ERROR, p->tok, "could not find function `%s` in current scope", value);        
    }
    else if(streq(field->value, "ignore_unused"))
    {
        bool all = streq("*", value);
        for(size_t i = 0; i < obj_list->size; i++)
        {
            ASTObj_T* obj = obj_list->items[i];
            if(all) 
            {
                obj->ignore_unused = true;
                continue;
            }
            else if(streq(value, obj->id->callee)) 
            {
                obj->ignore_unused = true;
                return;
            }
        }

        if(!all)
            throw_error(ERR_SYNTAX_ERROR, p->tok, "could not find identifier `%s` in current scope", value);
    }
    else if(streq(field->value, "exit_fn"))
    {
        parser_consume(p, TOKEN_COLON, "expect `:` after `exit_fn` compiler directive arg");
        ASTType_T* ty = parse_type(p);
        for(size_t i = 0; i < obj_list->size; i++)
        {
            ASTObj_T* obj = obj_list->items[i];
            if(streq(value, obj->id->callee) && obj->kind == OBJ_FUNCTION)
            {
                ASTExitFnHandle_T* handle = mem_malloc(sizeof(ASTExitFnHandle_T));
                handle->fn = obj;
                handle->type = ty;
                handle->tok = parser_peek(p, -2);

                if(!p->root_ref->type_exit_fns)
                    mem_add_list(p->root_ref->type_exit_fns = init_list(sizeof(ASTExitFnHandle_T*)));
                list_push(p->root_ref->type_exit_fns, handle);
                return;
            }
        }
        throw_error(ERR_SYNTAX_ERROR, p->tok, "could not find function `%s` in current scope", value);
    }
    else
        throw_error(ERR_SYNTAX_WARNING, field, "undefined compiler directive `%s`", field->value);
}

static void parse_compiler_directives(Parser_T* p, List_T* obj_list)
{
    parser_consume(p, TOKEN_LBRACKET, "expect `[` for compiler directive");

    Token_T* field_token = p->tok;
    parser_consume(p, TOKEN_ID, "expect compiler directive identifier");
    parser_consume(p, TOKEN_LPAREN, "expect `(` after identifier");

    do {
        if(tok_is(p, TOKEN_COMMA))
            parser_advance(p);
        Token_T* tok = p->tok;
        parser_consume(p, TOKEN_STRING, "expect value as string");
        eval_compiler_directive(p, field_token, tok->value, obj_list);
    } while(tok_is(p, TOKEN_COMMA));

    parser_consume(p, TOKEN_RPAREN, "expect `)` after value");
    parser_consume(p, TOKEN_RBRACKET, "expect `]` after compiler directive");
}

/////////////////////////////////
// Identifier Parser           //
/////////////////////////////////

static ASTIdentifier_T* __parse_identifier(Parser_T* p, ASTIdentifier_T* outer, bool is_simple)
{
    bool global_scope = false;
    if(tok_is(p, TOKEN_STATIC_MEMBER) && parser_peek(p, 1)->type == TOKEN_ID)
    {
        parser_advance(p);
        global_scope = true;
    }

    ASTIdentifier_T* id = init_ast_identifier(p->tok, p->tok->value);
    id->outer = outer;
    id->global_scope = global_scope;
    parser_consume(p, TOKEN_ID, "expect identifier");

    if(tok_is(p, TOKEN_STATIC_MEMBER) && !is_simple && parser_peek(p, 1)->type == TOKEN_ID)
    {
        if(parser_peek(p, 1)->type == TOKEN_LT)
           return id; // :: followed by < would be a generic in a functon or -call 
        
        parser_advance(p);
        id->kind = OBJ_NAMESPACE;  // only namespaces can have static members
        return __parse_identifier(p, id, false);
    }
    return id;
}

#define parse_identifier(p) __parse_identifier(p, NULL, false)
#define parse_simple_identifier(p) __parse_identifier(p, NULL, true)

/////////////////////////////////
// Datatype Parser             //
/////////////////////////////////

static ASTNode_T* parse_expr(Parser_T* p, Precedence_T prec, TokenType_T end_tok);

static ASTType_T* parse_struct_type(Parser_T* p)
{
    ASTType_T* struct_type = init_ast_type(TY_STRUCT, p->tok);
    if(tok_is(p, TOKEN_STRUCT))
        parser_consume(p, TOKEN_STRUCT, "expect `struct` keyword for struct type");
    else
    { 
        struct_type->is_union = true;
        parser_consume(p, TOKEN_UNION, "expect `union` keyword for struct type");
    }

    parser_consume(p, TOKEN_LBRACE, "expect `{` or identifier after struct keyword");
    struct_type->members = init_list(sizeof(struct AST_NODE_STRUCT*));
    mem_add_list(struct_type->members);

    while(!tok_is(p, TOKEN_RBRACE) && !tok_is(p, TOKEN_EOF))
    {
        ASTNode_T* member = init_ast_node(ND_STRUCT_MEMBER, p->tok);
        member->id = parse_simple_identifier(p);
        parser_consume(p, TOKEN_COLON, "expect `:` after struct member name");
        member->data_type = parse_type(p);

        list_push(struct_type->members, member);

        if(!tok_is(p, TOKEN_RBRACE))
            parser_consume(p, TOKEN_COMMA, "expect `,` between struct members");
    }

    parser_consume(p, TOKEN_RBRACE, "expect `}` after struct members");
    return struct_type;
}

static ASTType_T* parse_enum_type(Parser_T* p)
{
    ASTType_T* enum_type = init_ast_type(TY_ENUM, p->tok);

    parser_consume(p, TOKEN_ENUM, "expect `enum` keyword for enum type");
    parser_consume(p, TOKEN_LBRACE, "expect `{` after enum keyword");

    enum_type->members = init_list(sizeof(struct AST_OBJ_STRUCT*));
    mem_add_list(enum_type->members);

    for(i32 i = 0; !tok_is(p, TOKEN_RBRACE) && !tok_is(p, TOKEN_EOF); i++)
    {
        ASTObj_T* member = init_ast_obj(OBJ_ENUM_MEMBER, p->tok);
        member->data_type = (ASTType_T*) primitives[TY_I32];
        member->id = parse_simple_identifier(p);
        list_push(enum_type->members, member);

        if(tok_is(p, TOKEN_ASSIGN))
        {
            parser_advance(p);
            
            member->value = parse_expr(p, LOWEST, TOKEN_COMMA);
        }
        else {
            member->value = init_ast_node(ND_NOOP, member->tok);
        }

        if(!tok_is(p, TOKEN_RBRACE))
            parser_consume(p, TOKEN_COMMA, "expect `,` between enum members");
    }

    parser_consume(p, TOKEN_RBRACE, "expect `}` after enum members");
    return enum_type;
}

static ASTType_T* parse_lambda_type(Parser_T* p)
{
    ASTType_T* lambda = init_ast_type(TY_LAMBDA, p->tok);

    parser_consume(p, TOKEN_FN, "expect `fn` keyword for lambda type");

    if(tok_is(p, TOKEN_LT))
    {
        parser_consume(p, TOKEN_LT, "expect `<` before lambda return type");
        lambda->base = parse_type(p);
        parser_consume(p, TOKEN_GT, "expect `>` after lambda return type");
    }
    else
    {
        lambda->base = (ASTType_T*) primitives[TY_VOID];
    }

    parser_consume(p, TOKEN_LPAREN, "expect `(` before lambda argument types");
    lambda->arg_types = init_list(sizeof(struct AST_TYPE_STRUCT*));
    mem_add_list(lambda->arg_types);

    while(!tok_is(p, TOKEN_RPAREN) && !tok_is(p, TOKEN_EOF))
    {
        list_push(lambda->arg_types, parse_type(p));

        if(!tok_is(p, TOKEN_RPAREN))
            parser_consume(p, TOKEN_COMMA, "expect `,` between lambda argument types");
    }

    parser_consume(p, TOKEN_RPAREN, "expect `)` after lambda argument types");
    return lambda;
}

static ASTObj_T* parser_generate_tuple_type(Parser_T* p, ASTType_T* tuple)
{
    ASTObj_T* existing_tydef = get_compatible_tuple(p, tuple);
    if(existing_tydef)
    {
        tuple->kind = TY_UNDEF;
        tuple->id = existing_tydef->id;

        return existing_tydef;
    }
    else
    {
        ASTObj_T* tydef = init_ast_obj(OBJ_TYPEDEF, tuple->tok);
        tydef->data_type = mem_malloc(sizeof(struct AST_TYPE_STRUCT));
        *tydef->data_type = *tuple;
        tydef->id = init_ast_identifier(tuple->tok, "");
        sprintf(tydef->id->callee, "__csp_tuple_%lu__", p->cur_tuple_id++);
        list_push(p->root_ref->objs, tydef);
        tuple->kind = TY_UNDEF;
        tuple->id = tydef->id;

        return tydef;
    }
}

static ASTType_T* parse_type(Parser_T* p)
{
    ASTType_T* type = get_primitive_type(p->tok->value);
    if(type)
        parser_advance(p);
    else
    {
        switch(p->tok->type)
        {
            case TOKEN_CONST: 
                parser_advance(p);
                type = parse_type(p);
                type->is_constant = true;
                return type;

            case TOKEN_LPAREN:
                parser_advance(p);
                type = parse_type(p);
                parser_consume(p, TOKEN_RPAREN, "expect closing `)` after data type");
                break;
            case TOKEN_FN:
                type = parse_lambda_type(p);
                break;
            case TOKEN_UNION:
            case TOKEN_STRUCT:
                type = parse_struct_type(p);
                break;
            case TOKEN_ENUM:
                type = parse_enum_type(p);
                break;
            case TOKEN_AND:
                type = init_ast_type(TY_PTR, p->tok);
                type->base = init_ast_type(TY_PTR, p->tok);
                parser_advance(p);
                type->base->base = parse_type(p);
                break;
            case TOKEN_REF:
                type = init_ast_type(TY_PTR, p->tok);
                parser_advance(p);
                type->base = parse_type(p);
                break;
            case TOKEN_LBRACE:
                type = init_ast_type(TY_STRUCT, p->tok);
                type->members = init_list(sizeof(struct AST_TYPE_STRUCT*));
                mem_add_list(type->members);
                parser_advance(p);

                for(size_t i = 0; !tok_is(p, TOKEN_RBRACE) && !tok_is(p, TOKEN_EOF); i++)
                {
                    ASTNode_T* member = init_ast_node(ND_STRUCT_MEMBER, p->tok);
                    member->data_type = parse_type(p);
                    member->id = init_ast_identifier(p->tok, "");
                    sprintf(member->id->callee, "_%lu", i);

                    list_push(type->members, member);
                    if(!tok_is(p, TOKEN_RBRACE))
                        parser_consume(p, TOKEN_COMMA, "expect `,` between tuple argument types");
                }
                parser_consume(p, TOKEN_RBRACE, "expect `}` after tuple argument types");
                parser_generate_tuple_type(p, type);
                break;
            case TOKEN_TYPEOF:
                type = init_ast_type(TY_TYPEOF, p->tok);
                parser_advance(p);
                type->num_indices = parse_expr(p, X_OF, TOKEN_SEMICOLON);
                break;
            default:
                type = init_ast_type(TY_UNDEF, p->tok);
                type->id = parse_identifier(p);
                break;
        }
    }

parse_array_ty:
    if(tok_is(p, TOKEN_LBRACKET))
    {
        ASTType_T* arr_type = init_ast_type(TY_ARR, p->tok);
        parser_advance(p);
        if(!tok_is(p, TOKEN_RBRACKET))
            arr_type->num_indices = parse_expr(p, LOWEST, TOKEN_RBRACKET);
        parser_consume(p, TOKEN_RBRACKET, "expect `]` after array type");
        arr_type->base = type;
        type = arr_type;

        // repeat for arrays of arrays
        goto parse_array_ty;
    }

    return type;
}

/////////////////////////////////
// Definition & Obj Parser     //
/////////////////////////////////

static ASTObj_T* parse_global(Parser_T* p);
static ASTObj_T* parse_fn_def(Parser_T* p);

static ASTObj_T* parse_typedef(Parser_T* p)
{
    ASTObj_T* tydef = init_ast_obj(OBJ_TYPEDEF, p->tok);
    parser_consume(p, TOKEN_TYPE, "expect `type` keyword for typedef");

    tydef->id = parse_simple_identifier(p);
    parser_consume(p, TOKEN_COLON, "expect `:` after type name");

    tydef->data_type = parse_type(p);

    parser_consume(p, TOKEN_SEMICOLON, "expect `;` after type definition");
    return tydef;
}

static ASTObj_T* parse_extern_def(Parser_T *p)
{
    switch(p->tok->type)
    {
        case TOKEN_LET:
        {
            ASTObj_T* ext_var = parse_global(p);
            ext_var->is_extern = true;

            if(ext_var->value)
                throw_error(ERR_SYNTAX_WARNING, ext_var->value->tok, "cannot set a value to an extern variable");
            return ext_var;
        }
        case TOKEN_FN:
        {
            ASTObj_T* ext_fn = parse_fn_def(p);
            if(tok_is(p, TOKEN_SEMICOLON))
                parser_advance(p);
            ext_fn->is_extern = true;

            return ext_fn;
        }
        default:
            throw_error(ERR_SYNTAX_ERROR, p->tok, "expect function or variable declaration");
            break;
    }

    // satisfy -Wall
    return NULL;
}

static void parse_extern(Parser_T* p, List_T* objs)
{
    parser_advance(p);

    if(tok_is(p, TOKEN_LBRACE)) {
        parser_advance(p);
        while(!tok_is(p, TOKEN_RBRACE) && !tok_is(p, TOKEN_EOF))
        {
            list_push(objs, parse_extern_def(p));  
        }

        parser_consume(p, TOKEN_RBRACE, "expect `}` after extern function/variable definitions");
        return;
    }

    list_push(objs, parse_extern_def(p));    
}

List_T* parse_argument_list(Parser_T* p, TokenType_T end_tok, ASTIdentifier_T** variadic_id)
{
    List_T* arg_list = init_list(sizeof(ASTObj_T*));

    while(p->tok->type != end_tok)
    {
        if(parser_peek(p, 2)->type == TOKEN_VA_LIST)
        {
            (*variadic_id) = parse_simple_identifier(p);
            parser_consume(p, TOKEN_COLON, "expect `:` after argument name");
            parser_advance(p);

            if(!tok_is(p, end_tok))
                throw_error(ERR_SYNTAX_ERROR, p->tok, "expect `...` to be the last function argument");

            break;
        }
        else 
        {
            ASTObj_T* arg = init_ast_obj(OBJ_FN_ARG, p->tok);
            arg->id = parse_simple_identifier(p);
            parser_consume(p, TOKEN_COLON, "expect `:` after argument name");

            arg->data_type = parse_type(p);
            list_push(arg_list, arg);

            if(p->tok->type != end_tok)
                parser_consume(p, TOKEN_COMMA, "expect `,` between arguments");
        }
    }

    return arg_list;
}

static ASTNode_T* parse_stmt(Parser_T* p, bool needs_semicolon);

static ASTObj_T* parse_fn_def(Parser_T* p)
{
    ASTObj_T* fn = init_ast_obj(OBJ_FUNCTION, p->tok);
    parser_consume(p, TOKEN_FN, "expect `fn` keyword for a function definition");

    fn->id = parse_simple_identifier(p);

    parser_consume(p, TOKEN_LPAREN, "expect `(` after function name");

    ASTIdentifier_T* va_id = NULL;
    fn->args = parse_argument_list(p, TOKEN_RPAREN, &va_id);
    mem_add_list(fn->args);

    if(va_id)
    {
        fn->is_variadic = true;

        fn->va_area = init_ast_obj(OBJ_LOCAL, fn->tok);
        fn->va_area->id = va_id;
        fn->va_area->data_type = init_ast_type(TY_ARR, fn->tok);
        fn->va_area->data_type->num_indices = init_ast_node(ND_LONG, fn->tok);
        fn->va_area->data_type->num_indices->long_val = 136;
        fn->va_area->data_type->base = (ASTType_T*) primitives[TY_U8];
    }

    parser_consume(p, TOKEN_RPAREN, "expect `)` after function arguments");

    if(tok_is(p, TOKEN_COLON))
    {
        parser_advance(p);
        fn->return_type = parse_type(p);
    } else
        fn->return_type = (ASTType_T*) primitives[TY_VOID];

    fn->data_type = (ASTType_T*) primitives[TY_FN];

    if(global.ct == CT_ASM)
        fn->alloca_bottom = &alloca_bottom;

    return fn;
}

static void collect_locals(ASTNode_T* stmt, List_T* locals)
{
    switch(stmt->kind)
    {
        case ND_BLOCK:
            for(size_t i = 0; i < stmt->locals->size; i++)
                list_push(locals, stmt->locals->items[i]);
            for(size_t i = 0; i < stmt->stmts->size; i++)
                collect_locals(stmt->stmts->items[i], locals);
            break;
        case ND_WITH:
            list_push(locals, stmt->obj);
        case ND_IF:
            collect_locals(stmt->if_branch, locals);
            if(stmt->else_branch)
                collect_locals(stmt->else_branch, locals);
            break;
        case ND_FOR:
            for(size_t i = 0; i < stmt->locals->size; i++)
                list_push(locals, stmt->locals->items[i]);
        case ND_WHILE:
        case ND_LOOP:
        case ND_CASE:
            collect_locals(stmt->body, locals);
            break;
        case ND_MATCH:
            for(size_t i = 0; i < stmt->cases->size; i++)
                collect_locals(stmt->cases->items[i], locals);
            if(stmt->default_case)
                collect_locals(stmt->default_case, locals);
            break;
        default:
            break;
    }
}

static ASTObj_T* parse_fn(Parser_T* p)
{
    ASTObj_T* fn = parse_fn_def(p);

    p->cur_fn = fn;
    fn->body = parse_stmt(p, true);

    if(global.ct == CT_ASM)
    {
        fn->objs = init_list(sizeof(struct AST_OBJ_STRUCT*));
        mem_add_list(fn->objs);
        collect_locals(fn->body, fn->objs);
    }
    
    return fn;
}

static ASTObj_T* parse_global(Parser_T* p)
{
    ASTObj_T* global = init_ast_obj(OBJ_GLOBAL, p->tok);
    if(p->tok->type == TOKEN_LET)
        parser_advance(p);
    else if(p->tok->type == TOKEN_CONST)
    {
        global->is_constant = true;
        parser_advance(p);
    }
    else
        throw_error(ERR_SYNTAX_ERROR, p->tok, "expect `let` keyword for variable definition");
    
    global->id = parse_simple_identifier(p);

    parser_consume(p, TOKEN_COLON, "expect `:` after variable name");
    global->data_type = parse_type(p);
    if(tok_is(p, TOKEN_ASSIGN))
    {
        parser_advance(p);
        global->value = parse_expr(p, LOWEST, TOKEN_SEMICOLON);
    }
    
    parser_consume(p, TOKEN_SEMICOLON, "expect `;` after variable definition");
    return global;
}

static ASTObj_T* find_namespace(List_T* objs, char* callee)
{
    for(size_t i = 0; i < objs->size; i++)
    {
        ASTObj_T* obj = objs->items[i];
        if(obj->kind == OBJ_NAMESPACE && strcmp(obj->id->callee, callee) == 0)
            return obj;
    }
    return NULL;
}

static void parse_namespace(Parser_T* p, List_T* objs)
{
    Token_T* tok = p->tok;
    parser_advance(p); // skip the "namespace" token
    ASTIdentifier_T* id = parse_simple_identifier(p);

    // if there is already a namespace with this name in the current scope, add the new objs to it rather than creating a new namespace
    ASTObj_T* namespace = find_namespace(objs, id->callee);
    if(!namespace)
    {
        namespace = init_ast_obj(OBJ_NAMESPACE, tok);
        namespace->id = id;
        list_push(objs, namespace);

        // initialize the namespace's object list
        namespace->objs = init_list(sizeof(struct AST_OBJ_STRUCT));
        mem_add_list(namespace->objs);
    }
        
    // if the namespace has a { directly after its name, it exists in the current scope
    parser_consume(p, TOKEN_LBRACE, "expect `{` after namespace declaration");

    while(!tok_is(p, TOKEN_RBRACE) && !tok_is(p, TOKEN_EOF))
    {
        p->cur_fn = NULL;
        parse_obj(p, namespace->objs);
    }

    parser_consume(p, TOKEN_RBRACE, "expect `}` at end of namespace");

    for(size_t i = 0; i < namespace->objs->size; i++)
        ((ASTObj_T*) namespace->objs->items[i])->id->outer = namespace->id;
}

static void parse_obj(Parser_T* p, List_T* obj_list)
{
    switch(p->tok->type)
    {
        case TOKEN_TYPE:
                list_push(obj_list, parse_typedef(p));
                break;
            case TOKEN_CONST:
            case TOKEN_LET:
                list_push(obj_list, parse_global(p));
                break;
            case TOKEN_FN:
                list_push(obj_list, parse_fn(p));
                break;
            case TOKEN_EXTERN:  
                parse_extern(p, obj_list);
                break;
            case TOKEN_NAMESPACE:
                parse_namespace(p, obj_list);
                break;
            case TOKEN_LBRACKET:
                parse_compiler_directives(p, obj_list);
                break;
            default:
                throw_error(ERR_SYNTAX_ERROR, p->tok, "unexpected token `%s`, expect [import, type, let, const, fn]", p->tok->value);
    }
}

/////////////////////////////////
// Statement Parser            //
/////////////////////////////////

static ASTNode_T* parse_block(Parser_T* p)
{
    ASTNode_T* block = init_ast_node(ND_BLOCK, p->tok);
    block->locals = init_list(sizeof(struct AST_OBJ_STRUCT*));
    block->stmts = init_list(sizeof(struct AST_NODE_STRUCT*));

    parser_consume(p, TOKEN_LBRACE, "expect `{` at the beginning of a block statement");

    ASTNode_T* prev_block = p->cur_block;
    p->cur_block = block;
    while(p->tok->type != TOKEN_RBRACE)
    {
        if(tok_is(p, TOKEN_USING))
        {
            parser_advance(p);

            if(tok_is(p, TOKEN_COMMA))
                throw_error(ERR_SYNTAX_ERROR, p->tok, "expect identifier");

            do {
                if(tok_is(p, TOKEN_COMMA))
                    parser_advance(p);
                ASTNode_T* using = init_ast_node(ND_USING, p->tok);
                using->id = parse_identifier(p);
                list_push(block->stmts, using);
            } while(tok_is(p, TOKEN_COMMA));

            parser_consume(p, TOKEN_SEMICOLON, "expect `;` after identifiers");
        }
        else
            list_push(block->stmts, parse_stmt(p, true));
    }
    p->cur_block = prev_block;

    parser_consume(p, TOKEN_RBRACE, "expect `}` at the end of a block statement");

    mem_add_list(block->locals);
    mem_add_list(block->stmts);

    return block;
}

static ASTNode_T* parse_return(Parser_T* p, bool needs_semicolon)
{
    ASTNode_T* ret = init_ast_node(ND_RETURN, p->tok);

    parser_consume(p, TOKEN_RETURN, "expect `ret` or `<-` to return from function");

    if(!tok_is(p, TOKEN_SEMICOLON))
    {
        if((p->cur_fn && p->cur_fn->return_type->kind == TY_VOID))
            throw_error(ERR_TYPE_CAST_WARN, ret->tok, "cannot return value from function with type `void`, expect `;`");
        ret->return_val = parse_expr(p, LOWEST, TOKEN_SEMICOLON);
    }
    if(needs_semicolon)
        parser_consume(p, TOKEN_SEMICOLON, "expect `;` after return statement");

    return ret;
}

static ASTNode_T* parse_if(Parser_T* p, bool needs_semicolon)
{
    ASTNode_T* if_stmt = init_ast_node(ND_IF, p->tok);

    parser_consume(p, TOKEN_IF, "expect `if` keyword for an if statement");

    if_stmt->condition = parse_expr(p, LOWEST, TOKEN_EOF);
    if_stmt->if_branch = parse_stmt(p, needs_semicolon);

    if(tok_is(p, TOKEN_ELSE))
    {
        parser_advance(p);
        if_stmt->else_branch = parse_stmt(p, needs_semicolon);
    }

    return if_stmt;
}

static ASTNode_T* parse_loop(Parser_T* p, bool needs_semicolon)
{
    ASTNode_T* loop = init_ast_node(ND_LOOP, p->tok);

    parser_consume(p, TOKEN_LOOP, "expect `loop` keyword for a endless loop");

    loop->body = parse_stmt(p, needs_semicolon);

    return loop;
}

static ASTNode_T* parse_while(Parser_T* p, bool needs_semicolon)
{
    ASTNode_T* loop = init_ast_node(ND_WHILE, p->tok);

    parser_consume(p, TOKEN_WHILE, "expect `while` for a while loop statement");

    loop->condition = parse_expr(p, LOWEST, TOKEN_SEMICOLON);
    loop->body = parse_stmt(p, needs_semicolon);

    return loop;
}

static ASTNode_T* parse_for(Parser_T* p, bool needs_semicolon)
{
    ASTNode_T* loop = init_ast_node(ND_FOR, p->tok);

    parser_consume(p, TOKEN_FOR, "expect `for` for a for loop statement");

    loop->locals = init_list(sizeof(struct AST_OBJ_STRUCT*));
    mem_add_list(loop->locals);

    ASTNode_T* prev_block = p->cur_block;
    p->cur_block = loop;

    size_t num_locals = p->cur_block->locals->size;
    if(!tok_is(p, TOKEN_SEMICOLON))
    {
        ASTNode_T* init_stmt = parse_stmt(p, true);
        if(init_stmt->kind != ND_EXPR_STMT)
            throw_error(ERR_SYNTAX_ERROR, init_stmt->tok, "can only have expression-like statements in for-loop initializer");
        loop->init_stmt = init_stmt;
    } 
    else
        parser_advance(p);
    
    if(!tok_is(p, TOKEN_SEMICOLON))
        loop->condition = parse_expr(p, LOWEST, TOKEN_SEMICOLON);
    parser_advance(p);

    if(!tok_is(p, TOKEN_SEMICOLON))
        loop->expr = parse_expr(p, LOWEST, TOKEN_SEMICOLON);
    parser_advance(p);

    loop->body = parse_stmt(p, needs_semicolon);

    p->cur_block = prev_block;

    return loop; 
}

static ASTNode_T* parse_case(Parser_T* p)
{
    ASTNode_T* case_stmt = init_ast_node(ND_CASE, p->tok);

    if(tok_is(p, TOKEN_UNDERSCORE))
    {
        parser_advance(p);
        case_stmt->is_default_case = true;
    }
    else
        case_stmt->condition = parse_expr(p, LOWEST, TOKEN_ARROW);

    parser_consume(p, TOKEN_ARROW, "expect `=>` after case condition");
    case_stmt->body = parse_stmt(p, true);

    return case_stmt;
}

static ASTNode_T* parse_type_case(Parser_T* p)
{
    ASTNode_T* case_stmt = init_ast_node(ND_CASE_TYPE, p->tok);

    if(tok_is(p, TOKEN_UNDERSCORE))
    {
        parser_advance(p);
        case_stmt->is_default_case = true;
    }
    else
        case_stmt->data_type = parse_type(p);
    
    parser_consume(p, TOKEN_ARROW, "expect `=>` after case condition");
    case_stmt->body = parse_stmt(p, true);

    return case_stmt;
}

static ASTNode_T* parse_type_match(Parser_T* p, ASTNode_T* match)
{
    parser_advance(p);
    parser_advance(p);
    parser_consume(p, TOKEN_RPAREN, "expect `)` after `type`");

    match->kind = ND_MATCH_TYPE;
    match->data_type = parse_type(p);

    parser_consume(p, TOKEN_LBRACE, "expect `{` after match condition");

    while(!tok_is(p, TOKEN_RBRACE) && !tok_is(p, TOKEN_EOF))
    {
        ASTNode_T* case_stmt = parse_type_case(p);

        if(case_stmt->is_default_case)
        {
            if(match->default_case)
                throw_error(ERR_REDEFINITION, p->tok, "redefinition of default case `_`.");
            
            match->default_case = case_stmt;
            continue;
        }

        list_push(match->cases, case_stmt);
    }

    parser_consume(p, TOKEN_RBRACE, "expect `}` after match condition");
    return match;
}

static ASTNode_T* parse_match(Parser_T* p)
{
    ASTNode_T* match = init_ast_node(ND_MATCH, p->tok);
    match->cases = init_list(sizeof(struct AST_NODE_STRUCT*));
    match->default_case = NULL;
    mem_add_list(match->cases);

    parser_consume(p, TOKEN_MATCH, "expect `match` keyword to match an expression");

    if(tok_is(p, TOKEN_LPAREN) && parser_peek(p, 1)->type == TOKEN_TYPE)
        return parse_type_match(p, match);

    match->condition = parse_expr(p, LOWEST, TOKEN_LBRACE);
    
    parser_consume(p, TOKEN_LBRACE, "expect `{` after match condition");

    while(!tok_is(p, TOKEN_RBRACE) && !tok_is(p, TOKEN_EOF))
    {
        ASTNode_T* case_stmt = parse_case(p);

        if(case_stmt->is_default_case)
        {
            if(match->default_case)
                throw_error(ERR_REDEFINITION, p->tok, "redefinition of default case `_`.");

            match->default_case = case_stmt;
            continue;
        }

        list_push(match->cases, case_stmt);
    }

    parser_consume(p, TOKEN_RBRACE, "expect `}` after match condition");
    return match;
}

static ASTNode_T* parse_expr_stmt(Parser_T* p, bool needs_semicolon)
{
    ASTNode_T* stmt = init_ast_node(ND_EXPR_STMT, p->tok);
    stmt->expr = parse_expr(p, LOWEST, TOKEN_SEMICOLON);

    if(!is_executable(stmt->expr))
        throw_error(ERR_SYNTAX_ERROR, stmt->expr->tok, "cannot treat `%s` as a statement, expect function call, assignment or similar", stmt->expr->tok->value);
    if(needs_semicolon)
        parser_consume(p, TOKEN_SEMICOLON, "expect `;` after expression statement");
    return stmt;
}

static ASTNode_T* parse_local(Parser_T* p)
{
    ASTObj_T* local = init_ast_obj(OBJ_LOCAL, p->tok);
    if(p->tok->type == TOKEN_LET)
        parser_advance(p);
    else if(p->tok->type == TOKEN_CONST)
    {
        local->is_constant = true;
        parser_advance(p);
    }
    else
        throw_error(ERR_SYNTAX_ERROR, p->tok, "expect `let` keyword for variable definition");
    
    ASTNode_T* id = init_ast_node(ND_ID, p->tok);
    local->id = parse_simple_identifier(p);
    id->id = local->id;
    
    ASTNode_T* value = NULL;

    if(tok_is(p, TOKEN_COLON))
    {
        parser_consume(p, TOKEN_COLON, "expect `:` after variable name");

        local->data_type = parse_type(p);

        if(tok_is(p, TOKEN_ASSIGN))
        {
            ASTNode_T* assignment = init_ast_node(ND_ASSIGN, p->tok);
            parser_advance(p);
            assignment->left = id; 
            assignment->right = parse_expr(p, LOWEST, TOKEN_SEMICOLON);
            value = assignment;
        }
    }
    else
    {
        ASTNode_T* assignment = init_ast_node(ND_ASSIGN, p->tok);
        parser_consume(p, TOKEN_ASSIGN, "expect assignment `=` after typeless variable declaration");
        assignment->left = id;
        assignment->right = parse_expr(p, LOWEST, TOKEN_SEMICOLON);
        assignment->right->is_assigning = assignment->right->kind == ND_ARRAY || assignment->right->kind == ND_STRUCT;
        value = assignment;
    }

    if(!value)
        value = init_ast_node(ND_NOOP, p->tok);
    else
    {
        value->referenced_obj = local;
        value->is_initializing = true;
    }

    parser_consume(p, TOKEN_SEMICOLON, "expect `;` after variable definition");
                                                            // ND_FOR only for the for loop initializer
    if(!p->cur_block || (p->cur_block->kind != ND_BLOCK && p->cur_block->kind != ND_FOR))
        throw_error(ERR_SYNTAX_ERROR, local->tok, "cannot define a local variable outside a block statement");
    list_push(p->cur_block->locals, local);

    return value;
}

static ASTNode_T* parse_break(Parser_T* p, bool needs_semicolon)
{
    ASTNode_T* break_stmt = init_ast_node(ND_BREAK, p->tok);
    parser_consume(p, TOKEN_BREAK, "expect `break` keyword");
   
    if(needs_semicolon) 
        parser_consume(p, TOKEN_SEMICOLON, "expect `;` after break statement");

    return break_stmt;
}

static ASTNode_T* parse_continue(Parser_T* p, bool needs_semicolon)
{
    ASTNode_T* continue_stmt = init_ast_node(ND_CONTINUE, p->tok);
    parser_consume(p, TOKEN_CONTINUE, "expect `continue` keyword");

    if(needs_semicolon)
        parser_consume(p, TOKEN_SEMICOLON, "expect `;` after continue statement");

    return continue_stmt;
}

static ASTNode_T* parse_with(Parser_T* p, bool needs_semicolon)
{
    ASTNode_T* with_stmt = init_ast_node(ND_WITH, p->tok);
    parser_consume(p, TOKEN_WITH, "expect `with` keyword");

    ASTObj_T* var = with_stmt->obj = init_ast_obj(OBJ_LOCAL, p->tok);
    var->id = parse_simple_identifier(p);
    if(tok_is(p, TOKEN_COLON))
    {
        parser_advance(p);
        var->data_type = parse_type(p);
    }
    
    ASTNode_T* assignment = var->value = init_ast_node(ND_ASSIGN, p->tok);
    assignment->left = init_ast_node(ND_ID, var->id->tok);
    assignment->left->id = var->id;
    assignment->left->referenced_obj = var;
    assignment->is_initializing = true;
    assignment->referenced_obj = var;

    parser_consume(p, TOKEN_ASSIGN, "expect `=` after variable initializer");
    assignment->right = parse_expr(p, LOWEST, TOKEN_LBRACE);

    with_stmt->condition = assignment;
    with_stmt->if_branch = parse_stmt(p, needs_semicolon);
    if(tok_is(p, TOKEN_ELSE))
    {
        parser_advance(p);
        with_stmt->else_branch = parse_stmt(p, needs_semicolon);
    }

    return with_stmt;
}

static ASTNode_T* parse_stmt(Parser_T* p, bool needs_semicolon)
{

    switch(p->tok->type)
    {
        case TOKEN_LBRACE:
            return parse_block(p);
        case TOKEN_RETURN:
            return parse_return(p, needs_semicolon);
        case TOKEN_IF:
            return parse_if(p, needs_semicolon);
        case TOKEN_LOOP:
            return parse_loop(p, needs_semicolon);
        case TOKEN_FOR:
            return parse_for(p, needs_semicolon);
        case TOKEN_WHILE:
            return parse_while(p, needs_semicolon);
        case TOKEN_MATCH:
            return parse_match(p);
        case TOKEN_WITH:
            return parse_with(p, needs_semicolon);
        case TOKEN_CONST:
        case TOKEN_LET:
            {
                ASTNode_T* assignment = parse_local(p);
                if(assignment->kind == ND_NOOP)
                    return assignment;
                ASTNode_T* stmt = init_ast_node(ND_EXPR_STMT, assignment->tok);
                stmt->expr = assignment;
                return stmt;
            }
        case TOKEN_BREAK:
            return parse_break(p, needs_semicolon);
        case TOKEN_CONTINUE:
            return parse_continue(p, needs_semicolon);
        case TOKEN_SEMICOLON:   // skip random semicolons in the code
        case TOKEN_NOOP:
            {
                ASTNode_T* noop = init_ast_node(ND_NOOP, p->tok);
                
                if(tok_is(p, TOKEN_NOOP))
                {
                    parser_advance(p);
                    if(needs_semicolon)
                        parser_consume(p, TOKEN_SEMICOLON, "expect `;` after `noop` statement");
                } 
                else 
                {
                    parser_advance(p);
                }
                return noop;
            }
        default:
            return parse_expr_stmt(p, needs_semicolon);
    }

    // satisfy -Wall
    return NULL;
}

/////////////////////////////////
// Expression PRATT parser     //
/////////////////////////////////

static ASTNode_T* parse_expr(Parser_T* p, Precedence_T prec, TokenType_T end_tok)
{
    PrefixParseFn_T prefix = get_PrefixParseFn_T(p->tok->type);

    if(!prefix)
        throw_error(ERR_SYNTAX_ERROR, p->tok, "unexpected token `%s`, expect expression", p->tok->value);

    ASTNode_T* left_expr = prefix(p);

    while(!tok_is(p, end_tok) && prec < get_precedence(p->tok->type))
    {
        InfixParseFn_T infix = get_InfixParseFn_T(p->tok->type);
        if(!infix)
            return left_expr;
        
        left_expr = infix(p, left_expr);
    }

    return left_expr;
}

static List_T* parse_expr_list(Parser_T* p, TokenType_T end_tok)
{
    List_T* list = init_list(sizeof(struct AST_NODE_STRUCT*));
    mem_add_list(list);

    while (!tok_is(p, end_tok) && !tok_is(p, TOKEN_EOF)) 
    {
        list_push(list, parse_expr(p, LOWEST, TOKEN_COMMA));

        if(!tok_is(p, end_tok))
            parser_consume(p, TOKEN_COMMA, "expect `,` between call arguments");
    }

    return list;
}

static ASTNode_T* parse_id(Parser_T* p)
{
    ASTNode_T* id = init_ast_node(ND_ID, p->tok);
    id->id = parse_identifier(p);

    switch(p->tok->type) 
    {
        case TOKEN_LPAREN:
            return parse_call(p, id);
        case TOKEN_STATIC_MEMBER:
            if(parser_peek(p, 1)->type == TOKEN_LBRACE)
                return parse_struct_lit(p, id);
        default:
            return id;
    }
}

static ASTNode_T* parse_inline_asm(Parser_T* p)
{
    ASTNode_T* asm_stmt = init_ast_node(ND_ASM, p->tok);
    asm_stmt->args = init_list(sizeof(struct AST_NODE_STRUCT*));
    mem_add_list(asm_stmt->args);
    parser_advance(p);

    while(!tok_is(p, TOKEN_SEMICOLON))
    {
        switch (p->tok->type) {
            case TOKEN_STRING:
                list_push(asm_stmt->args, parse_str_lit(p, true));
                break;
            case TOKEN_INT:
                list_push(asm_stmt->args, parse_int_lit(p));
                break;
            case TOKEN_ID:
            case TOKEN_STATIC_MEMBER:
                list_push(asm_stmt->args, parse_id(p));
                break;
            default:
                throw_error(ERR_SYNTAX_ERROR, p->tok, "unexpected token `%s` in `asm` statement", p->tok->value);
        }
    }

    return asm_stmt;
}

static ASTNode_T* parse_int_lit(Parser_T* p)
{
    ASTNode_T* lit = init_ast_node(ND_INT, p->tok);
    parser_consume(p, TOKEN_INT, "expect integer literal (0, 1, 2, ...)");
    i128 num = atoll(lit->tok->value);
    if(num <= INT_MAX)
    {
        lit->kind = ND_INT;
        lit->int_val = (i32) num;
        lit->data_type = get_primitive_type("i32");
    }
    else if(num <= LONG_MAX)
    {
        lit->kind = ND_LONG;
        lit->long_val = (i64) num;
        lit->data_type = get_primitive_type("i64");
    }
    else
    {
        lit->kind = ND_ULONG;
        lit->ulong_val = num;
        lit->data_type = get_primitive_type("u64");
    }

    lit->is_constant = true;
    return lit;
}

static ASTNode_T* parse_float_lit(Parser_T* p)
{
    ASTNode_T* lit = init_ast_node(ND_FLOAT, p->tok);
    parser_consume(p, TOKEN_FLOAT, "expect float literal (0, 1, 2.3, ...)");
    f64 num; 
    sscanf(lit->tok->value, "%lf", &num); 

    if(num <= FLT_MAX)
    {
        lit->kind = ND_FLOAT;
        lit->float_val = (float) num;
        lit->data_type = get_primitive_type("f32");
    }
    else
    {
        lit->kind = ND_DOUBLE;
        lit->double_val = num;
        lit->data_type = get_primitive_type("f64");
    }

    return lit;
}

static ASTNode_T* parse_bool_lit(Parser_T* p)
{
    ASTNode_T* bool_lit = constant_literals[p->tok->type];

    bool_lit->bool_val = p->tok->type == TOKEN_TRUE;

    parser_advance(p);

    if(!bool_lit->data_type)
        bool_lit->data_type = (ASTType_T*) primitives[TY_BOOL];

    return bool_lit;
}

static ASTNode_T* parse_nil_lit(Parser_T* p)
{
    ASTNode_T* nil_lit = constant_literals[p->tok->type];
    parser_advance(p);

    if(!nil_lit->data_type)
        nil_lit->data_type = (ASTType_T*) void_ptr_type;

    return nil_lit;
}

static ASTNode_T* parse_char_lit(Parser_T* p)
{
    ASTNode_T* char_lit = init_ast_node(ND_CHAR, p->tok);

    if(strlen(p->tok->value) > 1)
        char_lit->str_val = strdup((char[]){'\\', p->tok->value[1], '\0'});
    else 
        char_lit->str_val = strdup((char[]){p->tok->value[0], '\0'});
    
    char_lit->is_constant = true; 
    char_lit->data_type = (ASTType_T*) primitives[TY_CHAR];

    parser_consume(p, TOKEN_CHAR, "expect char literal ('a', 'b', ...)");
    
    mem_add_ptr(char_lit->str_val);
    return char_lit;
}

static ASTNode_T* parse_str_lit(Parser_T* p, bool keep_inline)
{
    Token_T* tok = p->tok;
    parser_consume(p, TOKEN_STRING, "expect string literal (\"abc\", \"wxyz\", ...)");

    char* str = strdup(tok->value);

    while(tok_is(p, TOKEN_STRING)) // expressions like `"h" "e" "l" "l" "o"` get grouped together to `"hello"`
    {
        str = realloc(str, (strlen(str) + strlen(p->tok->value) + 1) * sizeof(char));
        strcat(str, p->tok->value);
        parser_advance(p);
    }

    ASTNode_T* node = build_str_lit(p->tok, str, !keep_inline && p->cur_fn, p->root_ref->objs);
    return node;
}

static ASTNode_T* parse_array_lit(Parser_T* p)
{
    ASTNode_T* arr_lit = init_ast_node(ND_ARRAY, p->tok);
    parser_consume(p, TOKEN_LBRACKET, "expect `[` for array literal");
    arr_lit->is_constant = true;
    arr_lit->args = parse_expr_list(p, TOKEN_RBRACKET);
    parser_consume(p, TOKEN_RBRACKET, "expect `]` after array literal");

    return arr_lit;
}

static ASTNode_T* parse_struct_lit(Parser_T* p, ASTNode_T* id)
{
    parser_consume(p, TOKEN_STATIC_MEMBER, "expect `::` before `{`");
    ASTNode_T* struct_lit = init_ast_node(ND_STRUCT, p->tok);
    parser_consume(p, TOKEN_LBRACE, "expect `{` for struct literal");
    struct_lit->args = parse_expr_list(p, TOKEN_RBRACE);
    parser_consume(p, TOKEN_RBRACE, "expect `}` after struct literal");

    struct_lit->data_type = init_ast_type(TY_UNDEF, id->tok);
    struct_lit->data_type->id = id->id;

    return struct_lit;
}

static ASTNode_T* parse_anonymous_struct_lit(Parser_T* p)
{
    ASTNode_T* struct_lit = init_ast_node(ND_STRUCT, p->tok);
    parser_consume(p, TOKEN_LBRACE, "expect `{` for struct literal");
    struct_lit->args = parse_expr_list(p, TOKEN_RBRACE);
    parser_consume(p, TOKEN_RBRACE, "expect `}` after struct literal");

    return struct_lit;
}

static ASTNode_T* parse_lambda_lit(Parser_T* p)
{
    ASTObj_T* lambda = init_ast_obj(OBJ_FUNCTION, p->tok);
    lambda->args = init_list(sizeof(struct AST_OBJ_STRUCT*));
    mem_add_list(lambda->args);

    if(tok_is(p, TOKEN_BIT_OR))
    {
        parser_advance(p);
        while(!tok_is(p, TOKEN_BIT_OR) && !tok_is(p, TOKEN_EOF))
        {
            // parse a lambda arguments
            ASTObj_T* arg = init_ast_obj(OBJ_FN_ARG, p->tok);
            arg->id = parse_simple_identifier(p);
            parser_consume(p, TOKEN_COLON, "expect `:` after lambda argument");

            arg->data_type = parse_type(p);
            list_push(lambda->args, arg);

            if(!tok_is(p, TOKEN_BIT_OR))
                parser_consume(p, TOKEN_COMMA, "expect `,` between lambda arguments");
        }
        parser_consume(p, TOKEN_BIT_OR, "expect `|` after lambda args");
    }
    else if(tok_is(p, TOKEN_OR))
        parser_advance(p);
    else
        throw_error(ERR_SYNTAX_ERROR, p->tok, "expect `|` for lambda literal, got `%s`", p->tok->value);

    lambda->return_type = tok_is(p, TOKEN_ARROW) ? (ASTType_T*) primitives[TY_VOID] : parse_type(p);
    parser_consume(p, TOKEN_ARROW, "expect `=>` after lambda return type");

    const char* callee_tmp = "__csp_lambda_lit_%ld__";
    char callee[__CSP_MAX_TOKEN_SIZE];
    sprintf(callee, callee_tmp, p->cur_lambda_id++);
    
    lambda->id = init_ast_identifier(lambda->tok, callee);
    lambda->data_type = (ASTType_T*) primitives[TY_FN];

    ASTObj_T* prev_fn = p->cur_fn;
    p->cur_fn = lambda;
    lambda->body = parse_stmt(p, false);
    p->cur_fn = prev_fn;

    if(global.ct == CT_ASM)
    {
        lambda->alloca_bottom = &alloca_bottom;
        lambda->objs = init_list(sizeof(struct AST_OBJ_STRUCT*));
        mem_add_list(lambda->objs);
    }

    list_push(p->root_ref->objs, lambda);

    ASTNode_T* caller = init_ast_node(ND_ID, lambda->tok);
    caller->id = lambda->id;

    return caller;
}

static ASTNode_T* parse_if_expr(Parser_T* p)
{
    ASTNode_T* if_expr = init_ast_node(ND_IF_EXPR, p->tok);
    parser_consume(p, TOKEN_IF, "expect `if` keyword");

    if_expr->condition = parse_expr(p, LOWEST, TOKEN_ARROW);
    parser_consume(p, TOKEN_ARROW, "expect `=>` after condition");

    if_expr->if_branch = parse_expr(p, LOWEST, TOKEN_ELSE);
    parser_consume(p, TOKEN_ELSE, "expect `else` between if branches");

    if_expr->else_branch = parse_expr(p, LOWEST, TOKEN_SEMICOLON);

    return if_expr;
}

static ASTNode_T* parse_unary(Parser_T* p)
{
    ASTNode_T* unary = init_ast_node(unary_ops[p->tok->type], p->tok);
    parser_advance(p);

    unary->right = parse_expr(p, LOWEST, TOKEN_ASSIGN);
    return unary;
}

static ASTNode_T* parse_num_op(Parser_T* p, ASTNode_T* left)
{
    ASTNode_T* infix = init_ast_node(infix_ops[p->tok->type], p->tok);
    parser_advance(p);

    infix->left = left;
    infix->right = parse_expr(p, expr_parse_fns[infix->tok->type].prec, TOKEN_EOF);

    return infix;
}

static ASTNode_T* parse_bit_op(Parser_T* p, ASTNode_T* left)
{
    return parse_num_op(p, left);
}

static ASTNode_T* parse_bool_op(Parser_T* p, ASTNode_T* left)
{
    ASTNode_T* infix = init_ast_node(infix_ops[p->tok->type], p->tok);
    parser_advance(p);

    infix->left = left;
    infix->right = parse_expr(p, expr_parse_fns[infix->tok->type].prec, TOKEN_EOF);

    infix->data_type = (ASTType_T*) primitives[TY_BOOL]; // set the data type, since == != > >= < <= will always result in booleans

    return infix;
}

static ASTNode_T* generate_assignment_op_rval(Parser_T* p, ASTNode_T* left, TokenType_T op)
{
    ASTNode_T* rval = init_ast_node(infix_ops[op], p->tok);
    parser_advance(p);
    rval->left = left;
    rval->right = parse_expr(p, expr_parse_fns[op].prec, TOKEN_EOF);

    return rval;
}

static ASTNode_T* parse_assignment(Parser_T* p, ASTNode_T* left)
{
    ASTNode_T* assign = init_ast_node(ND_ASSIGN, p->tok);
    assign->left = left;

    switch(p->tok->type)
    {
        case TOKEN_ASSIGN:
            parser_advance(p);
            assign->right = parse_expr(p, expr_parse_fns[p->tok->type].prec, TOKEN_EOF);
            assign->right->is_assigning = assign->right->kind == ND_ARRAY || assign->right->kind == ND_STRUCT;
            break;
        default:   
            assign->right = generate_assignment_op_rval(p, left, assign_to_op[p->tok->type]);
            break;
    }

    return assign;
}

static ASTNode_T* parse_postfix(Parser_T* p, ASTNode_T* left)
{
    ASTNode_T* postfix = init_ast_node(infix_ops[p->tok->type], p->tok);
    postfix->left = left;

    parser_advance(p);

    return postfix;
}

static ASTNode_T* parse_call(Parser_T* p, ASTNode_T* left)
{
    ASTNode_T* call = init_ast_node(ND_CALL, p->tok);

    if(left->kind != ND_ID)
        throw_error(ERR_SYNTAX_ERROR, p->tok, "can only call identifiers");

    call->expr = left;  // the expression to call

    parser_consume(p, TOKEN_LPAREN, "expect `(` after callee");

    call->args = parse_expr_list(p, TOKEN_RPAREN);
    parser_consume(p, TOKEN_RPAREN, "expect `)` after call arguments");

    return call;
}

static ASTNode_T* parse_index(Parser_T* p, ASTNode_T* left)
{
    ASTNode_T* index = init_ast_node(ND_INDEX, p->tok);
    index->left = left;

    parser_consume(p, TOKEN_LBRACKET, "expect `[` after array name for an index expression");

    index->expr = parse_expr(p, LOWEST, TOKEN_RBRACKET);
    parser_consume(p, TOKEN_RBRACKET, "expect `]` after array index");

    return index;
}

static ASTNode_T* parse_pipe(Parser_T* p, ASTNode_T* left)
{
    ASTNode_T* pipe = init_ast_node(ND_PIPE, p->tok);
    pipe->left = left;

    parser_consume(p, TOKEN_PIPE, "expect `|>` for pipe expression");

    parser_enable_holes(p);
    pipe->right = parse_expr(p, PIPE, TOKEN_SEMICOLON);
    parser_disable_holes(p);

    return pipe;
}

static ASTNode_T* parse_hole(Parser_T* p)
{
    Token_T* tok = p->tok;
    if(!parser_holes_enabled(p))
        throw_error(ERR_SYNTAX_ERROR, tok, "cannot have `$` here, only use `$` in pipe expressions");
    parser_consume(p, TOKEN_DOLLAR, "expect `$`");
    return init_ast_node(ND_HOLE, tok);
}

static ASTNode_T* parse_builtin_type_exprs(Parser_T* p, ASTNode_T* expr)
{
    if(streq(p->tok->value, "__reg_class"))
    {
        expr->cmp_kind = TOKEN_BUILTIN_REG_CLASS;
        expr->data_type = (ASTType_T*) primitives[TY_I32];
    }
    else if(streq(p->tok->value, "__is_int"))
    {
        expr->cmp_kind = TOKEN_BUILTIN_IS_INT;
        expr->data_type = (ASTType_T*) primitives[TY_BOOL];
    }
    else if(streq(p->tok->value, "__is_uint"))
    {
        expr->cmp_kind = TOKEN_BUILTIN_IS_UINT;
        expr->data_type = (ASTType_T*) primitives[TY_BOOL];
    }
    else if(streq(p->tok->value, "__is_float"))
    {
        expr->cmp_kind = TOKEN_BUILTIN_IS_FLOAT;
        expr->data_type = (ASTType_T*) primitives[TY_BOOL];
    }
    else if(streq(p->tok->value, "__is_pointer"))
    {
        expr->cmp_kind = TOKEN_BUILTIN_IS_POINTER;
        expr->data_type = (ASTType_T*) primitives[TY_BOOL];
    }
    else if(streq(p->tok->value, "__is_array"))
    {
        expr->cmp_kind = TOKEN_BUILTIN_IS_ARRAY;
        expr->data_type = (ASTType_T*) primitives[TY_BOOL];
    }
    else if(streq(p->tok->value, "__is_struct"))
    {
        expr->cmp_kind = TOKEN_BUILTIN_IS_STRUCT;
        expr->data_type = (ASTType_T*) primitives[TY_BOOL];
    }
    else if(streq(p->tok->value, "__is_union"))
    {
        expr->cmp_kind = TOKEN_BUILTIN_IS_UNION;
        expr->data_type = (ASTType_T*) primitives[TY_BOOL];
    }
    else if(streq(p->tok->value, "__to_str"))
    {
        expr->cmp_kind = TOKEN_BUILTIN_TO_STR;
        expr->data_type = (ASTType_T*) char_ptr_type; 
    }
    else
        throw_error(ERR_UNDEFINED, p->tok, "Undefined builtin type expression `%s`", p->tok->value);


    parser_consume(p, TOKEN_ID, "expect identifier");
    parser_consume(p, TOKEN_LPAREN, "expect `(` after `reg_class`");
    
    expr->r_type = parse_type(p);
    parser_consume(p, TOKEN_RPAREN, "expect `)` after `reg_class`");
    
    return expr;
}

static ASTNode_T* parse_type_expr(Parser_T* p)
{
    ASTNode_T* expr = init_ast_node(ND_TYPE_EXPR, p->tok);
    parser_consume(p, TOKEN_LPAREN, "expect `(` for type comparison");
    parser_consume(p, TOKEN_TYPE, "expect `type` for type comparison");
    parser_consume(p, TOKEN_RPAREN, "expect `)` after `type` keyword");

    if(tok_is(p, TOKEN_ID) && parser_peek(p, 1)->type == TOKEN_LPAREN)
    {
        return parse_builtin_type_exprs(p, expr);
    }

    expr->l_type = parse_type(p);
    
    switch(p->tok->type)
    {
        case TOKEN_EQ:
        case TOKEN_NOT_EQ:
        case TOKEN_GT:
        case TOKEN_GT_EQ:
        case TOKEN_LT:
        case TOKEN_LT_EQ:
            expr->cmp_kind = p->tok->type;
            parser_advance(p);
            break;
        
        default:
            throw_error(ERR_SYNTAX_ERROR, p->tok, "expect one of `==` `!=` `>` `>=` `<` `<=`, got `%s`", p->tok->value);
    }

    expr->r_type = parse_type(p);
    expr->data_type = (ASTType_T*) primitives[TY_BOOL];

    return expr;
}

static ASTNode_T* parse_closure(Parser_T* p)
{
    if(parser_peek(p, 1)->type == TOKEN_TYPE)
        return parse_type_expr(p);

    // if compiled to C, closures must be represented in the AST
    if(global.ct == CT_TRANSPILE)
    {
        ASTNode_T* closure = init_ast_node(ND_CLOSURE, p->tok);
        parser_consume(p, TOKEN_LPAREN, "expect `(` for closure");
        closure->expr = parse_expr(p, LOWEST, TOKEN_RPAREN);
        parser_consume(p, TOKEN_RPAREN, "expect `)` after closure");

        return closure;
    }
    else
    {
        parser_consume(p, TOKEN_LPAREN, "expect `(` for closure");
        ASTNode_T* expr = parse_expr(p, LOWEST, TOKEN_RPAREN);
        parser_consume(p, TOKEN_RPAREN, "expect `)` after closure");

        return expr;
    }
}

static ASTNode_T* parse_cast(Parser_T* p, ASTNode_T* left)
{   
    ASTNode_T* cast = init_ast_node(ND_CAST, p->tok);
    parser_consume(p, TOKEN_COLON, "expect `:` after expression for type cast");
    cast->left = left;
    cast->data_type = parse_type(p);
    cast->is_constant = left->is_constant;

    return cast;
}

static ASTNode_T* parse_sizeof(Parser_T* p)
{
    ASTNode_T* size_of = init_ast_node(ND_SIZEOF, p->tok);
    parser_consume(p, TOKEN_SIZEOF, "expect `sizeof` keyword");

    size_of->the_type = parse_type(p);
    size_of->data_type = (ASTType_T*) primitives[TY_U64];

    return size_of;
}

static ASTNode_T* parse_alignof(Parser_T* p)
{
    ASTNode_T* align_of = init_ast_node(ND_ALIGNOF, p->tok);
    parser_consume(p, TOKEN_ALIGNOF, "expect `alignof` keyword");

    align_of->the_type = parse_type(p);
    align_of->data_type = (ASTType_T*) primitives[TY_U64];

    return align_of;
}

static ASTNode_T* parse_len(Parser_T* p)
{
    ASTNode_T* len = init_ast_node(ND_LEN, p->tok);
    parser_consume(p, TOKEN_LEN, "expect `len` keyword");

    len->expr = parse_expr(p, LOWEST, TOKEN_SEMICOLON);
    len->data_type = (ASTType_T*) primitives[TY_U64];

    return len;
}

static ASTNode_T* parse_member(Parser_T* p, ASTNode_T* left)
{
    ASTNode_T* member = init_ast_node(ND_MEMBER, p->tok);
    parser_consume(p, TOKEN_DOT, "expect `.` for member expression");

    member->left = left;
    member->right = parse_expr(p, MEMBER, TOKEN_SEMICOLON);

    if(member->right->kind != ND_ID)
        throw_error(ERR_SYNTAX_ERROR, member->right->tok, "expect identifier");

    return member;
}

static ASTNode_T* parse_infix_call_expr(Parser_T* p)
{
    ASTNode_T* infix_id = init_ast_node(ND_ID, p->tok);

    parser_consume(p, TOKEN_INFIX_CALL, "expect infix call name before infix function call");
    infix_id->id = parse_identifier(p);
    parser_consume(p, TOKEN_INFIX_CALL, "expect infix call name after infix function call");

    return infix_id;
}

static ASTNode_T* parse_infix_call(Parser_T* p, ASTNode_T* left)
{
    ASTNode_T* call = init_ast_node(ND_CALL, p->tok);
    call->expr = parse_infix_call_expr(p);
    call->args = init_list(sizeof(ASTNode_T*));
    list_push(call->args, left);
    list_push(call->args, parse_expr(p, INFIX_CALL, TOKEN_SEMICOLON));

    mem_add_list(call->args);
    return call;
}

static ASTNode_T* parse_pow_2(Parser_T* p, ASTNode_T* left)
{
    // x² = (x * x)
    ASTNode_T* mult = init_ast_node(ND_MUL, p->tok);
    parser_consume(p, TOKEN_POW_2, "expect `²`");

    mult->left = left;
    mult->right = left;

    if(global.ct == CT_TRANSPILE)
    {
        ASTNode_T* closure = init_ast_node(ND_CLOSURE, p->tok);
        closure->expr = mult;
        return closure;
    }

    return mult;
}

static ASTNode_T* parse_pow_3(Parser_T* p, ASTNode_T* left)
{
    // x³ = (x * x * x)
    ASTNode_T* mult_a = init_ast_node(ND_MUL, p->tok);
    ASTNode_T* mult_b = init_ast_node(ND_MUL, p->tok);
    parser_consume(p, TOKEN_POW_3, "expect `³`");

    mult_a->left = left;
    mult_a->right = mult_b;
    mult_b->left = left;
    mult_b->right = left;

    if(global.ct == CT_TRANSPILE)
    {
        ASTNode_T* closure = init_ast_node(ND_CLOSURE, p->tok);
        closure->expr = mult_a;
        return closure;
    }

    return mult_a;
}

static ASTNode_T* parse_current_fn_token(Parser_T* p)
{
    p->tok->type = TOKEN_STRING,
    strcpy(p->tok->value, p->cur_fn->id->callee);

    return parse_str_lit(p, false);
}