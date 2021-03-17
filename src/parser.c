#include "parser.h"
#include "list.h"
#include "token.h"
#include "log.h"

#include <string.h>

#define LINE_NUMBER_SPACES 4
#define SYNTAX_ERROR(msg) LOG_ERROR(COLOR_BOLD_WHITE "%s:%d:%d =>" COLOR_BOLD_RED " [Error]" RESET " %s\n %*d | %s\n %*s | " COLOR_BOLD_RED "%*s%s\n",  \
                                parser->lexer->srcPath, parser->lexer->line, parser->lexer->iInLine, msg, LINE_NUMBER_SPACES, parser->lexer->line,      \
                                parser->lexer->currentLine, LINE_NUMBER_SPACES, "", parser->lexer->iInLine, "^~", "here");

parser_T* initParser(lexer_T* lexer)
{
    parser_T* parser = calloc(1, sizeof(struct PARSER_STRUCT));
    parser->lexer = lexer;
    parser->token = lexerNextToken(lexer);

    return parser;
}

token_T* parserAdvance(parser_T* parser)
{
    parser->token = lexerNextToken(parser->lexer);
    return parser->token;
}

token_T* parserConsume(parser_T* parser, tokenType_T type, const char* msg)
{
    if(parser->token->type != type)
    {
        SYNTAX_ERROR(msg);
        exit(1);
    }

    return parserAdvance(parser);
}

static AST_T* parserParseFnDef(parser_T* parser);
static AST_T* parserParseCompound(parser_T* parser);
static AST_T* parserParseVarDef(parser_T* parser);
static AST_T* parserParseId(parser_T* parser);

static AST_T* parserParseExpr(parser_T* parser);
static AST_T* parserParseAssignment(parser_T* parser, AST_T* left);
static AST_T* parserParseOp(parser_T* parser, AST_T* left);
static AST_T* parserParseNegate(parser_T* parser);
static AST_T* parserParseClosure(parser_T* parser);

static AST_T* parserParseNil(parser_T* parser);
static AST_T* parserParseNumber(parser_T* parser);
static AST_T* parserParseBool(parser_T* parser);
static AST_T* parserParseString(parser_T* parser);

static AST_T* parserParseStmt(parser_T* parser);
static AST_T* parserParseReturn(parser_T* parser);
static AST_T* parserParseExit(parser_T* parser);
static AST_T* parserParseWhile(parser_T* parser);
static AST_T* parserParseFor(parser_T* parser);
static AST_T* parserParseExit(parser_T* parser);
static AST_T* parserParseIf(parser_T* parser);

AST_T* parserParse(parser_T* parser)
{
    AST_T* root = initAST(ROOT, 0);
    root->root->contents = initList(sizeof(struct AST_STRUCT*));

    while(parser->token->type != TOKEN_EOF)
    {
        switch(parser->token->type)
        {
            case TOKEN_FN:
                parserAdvance(parser);
                listPush(root->root->contents, parserParseFnDef(parser));
                break;
            case TOKEN_LET:
                parserAdvance(parser);
                parserParseVarDef(parser);
                parserConsume(parser, TOKEN_SEMICOLON, "expect ';' after variable definition.");
                break;
            default: {
                const char* template = "unexpected token '%s'";
                char* msg = calloc(strlen(template) + strlen(parser->token->value) + 1, sizeof(char));
                sprintf(msg, template, parser->token->value);
                SYNTAX_ERROR(msg);
                exit(1);
            }
        }
    }

    LOG_OK("Parsing complete!%s", "\n");
    return root;
}

static AST_T* parserParseStmt(parser_T* parser)
{
    char* type = parser->token->value;

    if(strcmp(type, "return") == 0)
    {
        return parserParseReturn(parser);
    }
    else if(strcmp(type, "exit") == 0)
    {
        return parserParseExit(parser);
    }
    else if(strcmp(type, "for") == 0)
    {
        return parserParseFor(parser);
    }
    else if(strcmp(type, "while") == 0)
    {
        return parserParseWhile(parser);
    }
    else if(strcmp(type, "if") == 0)
    {
        return parserParseIf(parser);
    }
    else
    {
        const char* template = "unexpected token '%s'";
        char* msg = calloc(strlen(template) + strlen(parser->token->value) + 1, sizeof(char));
        sprintf(msg, template, parser->token->value);
        SYNTAX_ERROR(msg);
        exit(1);
    }
}

static AST_T* parserParseReturn(parser_T* parser)
{
    AST_T* ast = initAST(STMT, RETURN);
    parserConsume(parser, TOKEN_STMT, "Expect return statement.");
    ast->stmt->value = parserParseExpr(parser);

    return ast;
}

static AST_T* parserParseExit(parser_T* parser)
{
    AST_T* ast = initAST(STMT, EXIT);
    parserConsume(parser, TOKEN_STMT, "Expect exit statement.");
    ast->stmt->value = parserParseExpr(parser);

    return ast;
}

static AST_T* parserParseFor(parser_T* parser)
{
    AST_T* ast = initAST(STMT, FOR);
    parserConsume(parser, TOKEN_STMT, "Expect for statement.");
    
    if(parser->token->type != TOKEN_UNDERSCORE)
    {
        ast->stmt->value = parserParseVarDef(parser);
    } else {
        parserAdvance(parser);
    }

    parserConsume(parser, TOKEN_COMMA, "Expect ',' after for argument.");

    ast->stmt->condition = parserParseExpr(parser);

    parserConsume(parser, TOKEN_COMMA, "Expect ',' after for argument.");

    if(parser->token->type != TOKEN_UNDERSCORE)
    {
        ast->stmt->inc = parserParseExpr(parser);
    } else {
        parserAdvance(parser);
    }

    ast->stmt->body = parserParseCompound(parser);

    return ast;
}

static AST_T* parserParseWhile(parser_T* parser)
{
    AST_T* ast = initAST(STMT, WHILE);
    parserConsume(parser, TOKEN_STMT, "Expect while statement.");
    ast->stmt->condition = parserParseExpr(parser);
    ast->stmt->body = parserParseCompound(parser);

    return ast;
}

static AST_T* parserParseIf(parser_T* parser)
{
    AST_T* ast = initAST(STMT, IF);
    parserConsume(parser, TOKEN_STMT, "Expect if statement.");

    ast->stmt->condition = parserParseExpr(parser);
    ast->stmt->ifBody = parserParseCompound(parser);

    if(parser->token->type == TOKEN_STMT && strcmp(parser->token->value, "else") == 0)
    {
        parserAdvance(parser);

        // TODO: implement else if

        ast->stmt->elseBody = parserParseCompound(parser);
    }

    return ast;
}

static AST_T* parserParseExpr(parser_T* parser)
{
    switch(parser->token->type)
    {
        case TOKEN_ID:
            return parserParseId(parser);
        case TOKEN_LEFT_PAREN:
            return parserParseClosure(parser);
        case TOKEN_NUMBER:
            return parserParseNumber(parser);
        case TOKEN_STR:
            return parserParseString(parser);
        case TOKEN_BOOL:
            return parserParseBool(parser);
        case TOKEN_NIL:
            return parserParseNil(parser);
        default: {
            const char* template = "unexpected token '%s'";
            char* msg = calloc(strlen(template) + strlen(parser->token->value) + 1, sizeof(char));
            sprintf(msg, template, parser->token->value);
            SYNTAX_ERROR(msg);
            exit(1);
        }
    }
}

static AST_T* parserParseNumber(parser_T* parser)
{
    AST_T* ast = initAST(EXPR, CONSTANT);
    ast->expr->intValue = atoi(parser->token->value);
    parserConsume(parser, TOKEN_NUMBER, "Expect number constant.");

    return parserParseOp(parser, ast);
}

static AST_T* parserParseString(parser_T* parser)
{
    AST_T* ast = initAST(EXPR, STRING);
    ast->expr->strValue = parser->token->value;
    parserConsume(parser, TOKEN_STR, "Expect string constant.");

    return parserParseOp(parser, ast);
}

static AST_T* parserParseBool(parser_T* parser)
{
    AST_T* ast = initAST(EXPR, CONSTANT);

    if(strcmp(parser->token->value, "true") == 0)
    {
        ast->expr->boolValue = true;
    }
    else if(strcmp(parser->token->value, "false") == 0)
    {
        ast->expr->boolValue = true;
    }
    
    parserConsume(parser, TOKEN_BOOL, "Expect boolean constant.");

    return parserParseOp(parser, ast);
}

static AST_T* parserParseNil(parser_T* parser)
{
    AST_T* ast = initAST(EXPR, NIL);
    parserConsume(parser, TOKEN_NIL, "Expect nil.");
    
    return parserParseOp(parser, ast);
}

static AST_T* parserParseClosure(parser_T* parser)
{
    parserAdvance(parser);
    AST_T* ast = parserParseExpr(parser);
    parserConsume(parser, TOKEN_RIGHT_BRACE, "Expect ')'.");

    return parserParseOp(parser, ast);
}

static AST_T* parserParseNegate(parser_T* parser)
{
    AST_T* ast = initAST(EXPR, NEGATE);
    parserAdvance(parser);
    ast->expr->op.right = parserParseExpr(parser);

    return ast;
}

static AST_T* parserParseOp(parser_T* parser, AST_T* left)
{
    AST_T* ast = initAST(EXPR, 0);
    int opType = 0;

    switch(parser->token->type)
    {
        case TOKEN_PLUS:
            opType = ADD;
            break;
        case TOKEN_MINUS:
            opType = SUB;
            break;
        case TOKEN_STAR:
            opType = MULT;
            break;
        case TOKEN_SLASH:
            opType = DIV;
            break;
        case TOKEN_EQUALS:
        case TOKEN_PLUS_EQUALS:
        case TOKEN_MINUS_EQUALS:
        case TOKEN_STAR_EQUALS:
        case TOKEN_SLASH_EQUALS:
            return parserParseAssignment(parser, left);
            break;
        default:
            return left;
    }

    parserAdvance(parser);
    
    ast->expr->type = opType;
    ast->expr->op.left = left;
    ast->expr->op.right = parserParseExpr(parser);

    return ast;
}

static AST_T* parserParseAssignment(parser_T* parser, AST_T* left)
{
    AST_T* ast = initAST(EXPR, ASSIGN);

    if(left->expr->type != CALL || left->expr->isFunctionCall != false)
    {
        const char* msg = "can only assign a value to a variable.";
        SYNTAX_ERROR(msg);
        exit(1);
    }
        ast->expr->op.left = left;
    switch(parser->token->type)
    {
        case TOKEN_EQUALS:
            parserAdvance(parser);
            ast->expr->op.right = parserParseExpr(parser);
            break;
        case TOKEN_PLUS_EQUALS: {
            AST_T* op = initAST(EXPR, ADD);
            op->expr->op.left = left;
            parserAdvance(parser);
            op->expr->op.right = parserParseExpr(parser);
            break;
        }
        case TOKEN_MINUS_EQUALS: {
            AST_T* op = initAST(EXPR, SUB);
            op->expr->op.left = left;
            parserAdvance(parser);
            op->expr->op.right = parserParseExpr(parser);
            break;
        }
        case TOKEN_STAR_EQUALS: {
            AST_T* op = initAST(EXPR, MULT);
            op->expr->op.left = left;
            parserAdvance(parser);
            op->expr->op.right = parserParseExpr(parser);
            break;
        }
        case TOKEN_SLASH_EQUALS: {
            AST_T* op = initAST(EXPR, DIV);
            op->expr->op.left = left;
            parserAdvance(parser);
            op->expr->op.right = parserParseExpr(parser);
            break;
        }
        default: {
            const char* template = "unexpected token '%s'";
            char* msg = calloc(strlen(template) + strlen(parser->token->value) + 1, sizeof(char));
            sprintf(msg, template, parser->token->value);
            SYNTAX_ERROR(msg);
            exit(1);
        }
    }

    return ast;
}

static AST_T* parserParseId(parser_T* parser)
{
    AST_T* ast = initAST(EXPR, CALL);
    ast->expr->name = parser->token->value;
    ast->expr->isFunctionCall = false;

    parserConsume(parser, TOKEN_ID, "Expect name before call expression.");
    if(parser->token->type == TOKEN_LEFT_PAREN)
    {
        parserAdvance(parser);
        ast->expr->isFunctionCall = true;
        ast->expr->args = initList(sizeof(struct AST_STRUCT*));

        while(parser->token->type != TOKEN_RIGHT_PAREN)
        {
            listPush(ast->expr->args, parserParseExpr(parser));
            if(parser->token->type == TOKEN_COMMA) {
                parserAdvance(parser);
            }
        }

        parserConsume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after function call arguments");
    }

    return parserParseOp(parser, ast);
}

static AST_T* parserParseCompound(parser_T* parser)
{
    AST_T* ast = initAST(COMPOUND, 0);
    ast->compound->contents = initList(sizeof(struct AST_STRUCT*));

    parserConsume(parser, TOKEN_LEFT_BRACE, "Expect '{'.");

    while(parser->token->type != TOKEN_RIGHT_BRACE)
    {
        if(parser->token->type == TOKEN_EOF)
        {
            parserConsume(parser, TOKEN_RIGHT_BRACE, "Unterminated compound, expect '}'.");
            exit(1);
        }

        if(parser->token->type == TOKEN_RIGHT_BRACE)
        {
            break;
        }

        if(parser->token->type == TOKEN_STMT)
        {
            listPush(ast->compound->contents, parserParseStmt(parser));
        }
        else if(parser->token->type == TOKEN_LET)
        {
            parserAdvance(parser);
            listPush(ast->compound->contents, parserParseVarDef(parser));
            parserConsume(parser, TOKEN_SEMICOLON, "Expect ';' after variable definition.");
        }
        else if(parser->token->type != TOKEN_RIGHT_BRACE) {
            listPush(ast->compound->contents, parserParseExpr(parser));
            parserConsume(parser, TOKEN_SEMICOLON, "Expect ';' after expression.");
        }
    }
    parserConsume(parser, TOKEN_RIGHT_BRACE, "Expect '}'.");

    return ast;
}

static AST_T* parserParseVarDef(parser_T* parser)
{
    AST_T* var = initAST(DEF, VAR);
    var->def->name = parser->token->value;
    parserConsume(parser, TOKEN_ID, "Expect variable name.");

    var->def->isFunction = false;
    parserConsume(parser, TOKEN_COLON, "Expect ':' after variable name.");
    var->def->dataType = parser->token->value;
    parserConsume(parser, TOKEN_IDENTIFIER, "Expect data type after ':'.");

    if(parser->token->type == TOKEN_EQUALS)
    {
        parserAdvance(parser);
        var->def->value = parserParseExpr(parser);
    }

    return var;
}

static AST_T* parserParseFnDef(parser_T* parser)
{
    AST_T* fn = initAST(DEF, FN);
    fn->def->name = parser->token->value;
    parserConsume(parser, TOKEN_ID, "Expect function name.");

    fn->def->isFunction = true;
    fn->def->args = initList(sizeof(struct AST_STRUCT));

    parserConsume(parser, TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    
    while(parser->token->type != TOKEN_RIGHT_PAREN)
    {
        // TODO: parse arg list
        parserAdvance(parser);
    }

    parserConsume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after function arguments.");

    if(parser->token->type == TOKEN_COLON)
    {
        parserAdvance(parser);
        fn->def->dataType = parser->token->value;
        parserConsume(parser, TOKEN_IDENTIFIER, "Expect return type after ':'.");
    }

    parserConsume(parser, TOKEN_EQUALS, "Expect '=' after function definition.");

    fn->def->value = parserParseCompound(parser);

    return fn;    
}