#include "c_codegen.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <codegen/asm/asm_codegen.h>
#include <config.h>
#include <globals.h>
#include <platform/platform_bindings.h>
#include <io/log.h>
#include <io/io.h>
#include "../codegen_utils.h"
#include "ast/ast.h"
#include "error/error.h"
#include "hashmap.h"
#include "keywords.h"
#include "list.h"
#include "util.h"
#include <mem/mem.h>
#include "debugger/register.h"

#define ID_PREFIX  "__csp_"
#define MAIN_FN_ID ID_PREFIX "main"

#define C_NUM_REGISTERS REG_RFLAGS

static void c_gen_typedefs(CCodegenData_T* cg, List_T* objs);
static void c_gen_structs(CCodegenData_T* cg, List_T* objs);
static void c_gen_globals(CCodegenData_T* cg, List_T* objs);
static void c_gen_function_definitions(CCodegenData_T* cg, List_T* objs);
static void c_gen_functions(CCodegenData_T* cg, List_T* objs);
static void c_gen_expr(CCodegenData_T* cg, ASTNode_T* expr);
static void c_gen_stmt(CCodegenData_T* cg, ASTNode_T* stmt);
static void c_gen_type(CCodegenData_T* cg, ASTType_T* type);
static void c_gen_typed_name(CCodegenData_T* cg, ASTIdentifier_T* id, ASTType_T* type);
static void c_predefine_dependant_types(CCodegenData_T* cg, ASTType_T* type);

char* cc = DEFAULT_CC;
char* cc_flags = DEFAULT_CC_FLAGS;

static const char* reg_names[C_NUM_REGISTERS] = {
    "%rax", "%rbx", "%rcx", "%rdx",
    "%rdi", "%rsi",
    "%rbp", "%rsp",
    "%r8", "%r9", "%r10", "%r11",
    "%r12", "%r13", "%r14", "%r15",
    "%rip" 
};

static const char* op_symbols[ND_KIND_LEN] = {
    [ND_ADD] = "+",
    [ND_SUB] = "-",
    [ND_MUL] = "*",
    [ND_DIV] = "/",
    [ND_MOD] = "%",
    [ND_NEG] = "-",
    [ND_BIT_NEG] = "~",
    [ND_NOT] = "!",
    [ND_REF] = "&",
    [ND_DEREF] = "*",
    [ND_EQ] = "==",
    [ND_NE] = "!=",
    [ND_GT] = ">",
    [ND_GE] = ">=",
    [ND_LT] = "<",
    [ND_LE] = "<=",
    [ND_AND] = "&&",
    [ND_OR] = "||",
    [ND_LSHIFT] = "<<",
    [ND_RSHIFT] = ">>",
    [ND_XOR] = "^",
    [ND_BIT_OR] = "|",
    [ND_BIT_AND] = "&",
    [ND_ASSIGN] = "="
};

static const char c_header_text[] = 
    "// Automatically generated by the CSpydr compiler.\n"
    "\n"
    "#include <stdarg.h>\n"
    "\n"
    "static const _Bool _false = 0;\n"
    "static const _Bool _true = 1;\n"
    "\n"
    "static inline unsigned long _inline_strlen(const char* s) {\n"
    "  unsigned long l;\n"
    "  for(l = 0; s[l]; l++);\n"
    "  return l;\n"
    "}\n"
    "\n";

static const char* c_start_text[] = 
{
#define _START_HEADER             \
    "\n"                          \
    "extern void _start(void);\n" \
    "__asm__(\n"                  \
    "  \".globl _start\\n\"\n"    \
    "  \"_start:\\n\"\n"

#define _START_FOOTER \
    ");\n"

#define _START_EXIT                \
    "  \"  movq %rax, %rdi\\n\"\n" \
    "  \"  movq $60, %rax\\n\"\n"  \
    "  \"  syscall\"\n"            \
    _START_FOOTER

    [MFK_NO_ARGS] =
        _START_HEADER
        "  \"  call " MAIN_FN_ID "\\n\"\n"
        _START_EXIT,
    
    [MFK_ARGV_PTR] =
        _START_HEADER
        "  \"  xorl %ebp, %ebp\\n\"\n"
        "  \"  popq %rdi\\n\"\n"
        "  \"  movq %rsp, %rdi\\n\"\n"
        "  \"  call " MAIN_FN_ID "\\n\"\n"
        _START_EXIT,

    [MFK_ARGC_ARGV_PTR] =
        _START_HEADER
        "  \"  xorl %ebp, %ebp\\n\"\n"
        "  \"  popq %rdi\\n\"\n"
        "  \"  movq %rsp, %rdi\\n\"\n"
        "  \"  andq $~15, %rsp\\n\"\n"
        "  \"  call " MAIN_FN_ID "\\n\"\n"
        _START_EXIT,
    
    [MFK_ARGS_ARRAY] = 
        _START_HEADER
        "  \"  call " MAIN_FN_ID "\\n\"\n"
        _START_EXIT
};

static char* c_primitive_types[TY_KIND_LEN] = {
    [TY_U8]  = "unsigned char",
    [TY_U16] = "unsigned short",
    [TY_U32] = "unsigned int",
    [TY_U64] = "unsigned long",
    [TY_I8]  = "signed char",
    [TY_I16] = "signed short",
    [TY_I32] = "signed int",
    [TY_I64] = "signed long",
    [TY_F32] = "float",
    [TY_F64] = "double",
    [TY_F80] = "long double",
    [TY_VOID] = "void",
    [TY_CHAR] = "char",
    [TY_BOOL] = "_Bool"
};

void init_c_cg(CCodegenData_T* cg, ASTProg_T* ast)
{
    memset(cg, 0, sizeof(struct C_CODEGEN_DATA_STRUCT));

    cg->ast = ast;
    cg->silent = global.silent;
    cg->code_buffer = open_memstream(&cg->buf, &cg->buf_len);
    cg->unique_id = 0;
}

void free_c_cg(CCodegenData_T* cg)
{
    free(cg->buf);
}

#ifdef __GNUC__
__attribute((format(printf, 2, 3)))
#endif
static void c_print(CCodegenData_T* cg, char* fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    vfprintf(cg->code_buffer, fmt, va);
    va_end(va);
}

#ifdef __GNUC__
__attribute((format(printf, 2, 3)))
#endif
static void c_println(CCodegenData_T* cg, char* fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    vfprintf(cg->code_buffer, fmt, va);
    va_end(va);    
    fputc('\n', cg->code_buffer);
}

static void c_putc(CCodegenData_T* cg, int c)
{
    fputc(c, cg->code_buffer);
}

static void c_rewind(CCodegenData_T* cg, i64 amount)
{
    fseek(cg->code_buffer, amount, SEEK_END);
}

static void write_code(CCodegenData_T* cg, const char* target, bool cachefile)
{
    char file_path[BUFSIZ * 2] = {'\0'};
    
    if(cachefile)
    {
        char* homedir = get_home_directory();
        char cache_dir[BUFSIZ] = {'\0'};

        if(cachefile)
            sprintf(cache_dir, "%s" DIRECTORY_DELIMS CACHE_DIR DIRECTORY_DELIMS, homedir);

        if(make_dir(cache_dir))
        {
            LOG_ERROR("error creating cache directory `" DIRECTORY_DELIMS CACHE_DIR DIRECTORY_DELIMS "`.\n");
            throw(global.main_error_exception);
        }
        sprintf(file_path, "%s" DIRECTORY_DELIMS "%s.c", cache_dir, target);
    }
    else
        sprintf(file_path, "%s.c", target);

    fclose(cg->code_buffer);

    FILE* out = open_file(file_path);
    fwrite(cg->buf, cg->buf_len, 1, out);
    fclose(out);
}

void c_gen_code(CCodegenData_T* cg, const char* target)
{
    if(!cg->silent)
    {
        LOG_OK(COLOR_BOLD_BLUE "  Generating" COLOR_BOLD_WHITE " C" COLOR_RESET " code\n");
    }

    // generate the c code
    c_print(cg, "%s", c_header_text);
    c_gen_typedefs(cg, cg->ast->objs);
    c_gen_structs(cg, cg->ast->objs);
    c_gen_globals(cg, cg->ast->objs);
    c_gen_function_definitions(cg, cg->ast->objs);
    c_gen_functions(cg, cg->ast->objs);
    c_println(cg, "%s", c_start_text[cg->ast->mfk]);
    write_code(cg, target, global.do_assemble);

    if(cg->print)
    {
        if(!cg->silent)
            LOG_INFO(COLOR_RESET);
        fprintf(OUTPUT_STREAM, "%s", cg->buf);
    }

    if(!global.do_assemble)
        return;
    char c_source_file[BUFSIZ] = {'\0'};
    get_cached_file_path(c_source_file, target, ".c");

    char obj_file[BUFSIZ] = {'\0'};
    if(global.do_link) 
        get_cached_file_path(obj_file, target, ".o");
    else
        sprintf(obj_file, "%s.o", target);
    
    // run the compiler
    {
        const char* args[] = {
            cc,
            "-c",
            c_source_file,
            "-nostdlib",
            "-ffreestanding",
            "-std=c2x",
            "-o",
            obj_file,
            NULL,
            NULL
        };

        if(global.embed_debug_info)
            args[LEN(args) - 2] = "-g";
        
        i32 exit_code = subprocess(args[0], (char* const*) args, false);

        if(exit_code != 0)
        {
            LOG_ERROR_F("error compiling code. (exit code %d)\n", exit_code);
            throw(global.main_error_exception);
        }
    }

    // run the linker
    if(global.do_link)    
        link_obj(target, obj_file, cg->silent);
}

static char* c_gen_identifier(ASTIdentifier_T* id)
{
    char* str = gen_identifier(id, "_", ID_PREFIX);
    mem_add_ptr(str);
    return str;
}

static void c_gen_typedef(CCodegenData_T* cg, ASTObj_T* obj)
{
    if(obj->generated)
        return;

    char* callee = c_gen_identifier(obj->id);
    obj->generated = true;
    c_predefine_dependant_types(cg, obj->data_type);
    c_print(cg, "typedef ");
    if(obj->data_type->kind == TY_STRUCT)
    {
        char* keyword = obj->data_type->is_union ? "union" : "struct";
        c_println(cg, "%s %s %s;", keyword, callee, callee);
    }
    else
    {
        c_gen_typed_name(cg, obj->id, obj->data_type);
        c_println(cg, ";");
    }
}

static void c_predefine_dependant_types(CCodegenData_T* cg, ASTType_T* type)
{
    switch(type->kind)
    {
    case TY_UNDEF:
        if(type->referenced_obj && !type->referenced_obj->generated)
            c_gen_typedef(cg, type->referenced_obj);
        break;
    default:
        break;
    }
}

static void c_gen_typedefs(CCodegenData_T* cg, List_T* objs)
{
    for(size_t i = 0; i < objs->size; i++)
    {
        ASTObj_T* obj = objs->items[i];
        switch(obj->kind)
        {
        case OBJ_NAMESPACE:
            c_gen_typedefs(cg, obj->objs);
            break;
        case OBJ_TYPEDEF:
            c_gen_typedef(cg, obj);
            break;
        default:
            break;
        }
    }
}

static void c_gen_struct(CCodegenData_T* cg, ASTType_T* type, const char* name)
{
    c_println(cg, "%s %s{", type->is_union ? "union" : "struct", name ? name : "");
    for(size_t i = 0; i < type->members->size; i++)
    {
        ASTNode_T* member = type->members->items[i];
        c_print(cg, "  ");
        c_gen_typed_name(cg, member->id, member->data_type);
        c_println(cg, ";");
    }
    c_putc(cg, '}');
}

static void c_predefine_dependant_structs(CCodegenData_T* cg, ASTObj_T* obj)
{
    ASTType_T* s_type = obj->data_type;
    for(size_t i = 0; i < s_type->members->size; i++)
    {
        ASTNode_T* member = s_type->members->items[i];
        if(member->data_type->kind == TY_UNDEF && 
            member->data_type->referenced_obj && 
            member->data_type->referenced_obj->generated &&
            member->data_type->base->kind == TY_STRUCT)
        {
            member->data_type->referenced_obj->generated = false;
            c_gen_struct(cg, member->data_type->base, c_gen_identifier(member->data_type->id));
            c_println(cg, ";");
        }
    }
}

static void c_gen_structs(CCodegenData_T* cg, List_T* objs)
{
    for(size_t i = 0; i < objs->size; i++)
    {
        ASTObj_T* obj = objs->items[i];
        switch(obj->kind)
        {
        case OBJ_NAMESPACE:
            c_gen_structs(cg, obj->objs);
            break;
        case OBJ_TYPEDEF:
            if(obj->generated && obj->data_type->kind == TY_STRUCT)
            {
                obj->generated = false;
                c_predefine_dependant_structs(cg, obj);
                c_gen_struct(cg, obj->data_type, c_gen_identifier(obj->id));
                c_println(cg, ";");
            }
            break;
        default:
            break;
        }
    }
}

static void c_gen_type(CCodegenData_T* cg, ASTType_T* type)
{
    if(type->is_constant)
        c_print(cg, "const ");

    if(type->is_primitive && type->kind != TY_FN)
    {
        c_print(cg, "%s", c_primitive_types[type->kind]);
        return;
    }

    switch(type->kind)
    {
    case TY_PTR:
        c_gen_type(cg, type->base);
        c_putc(cg, '*');
        break;
    case TY_ARRAY:
        c_print(cg, "struct { unsigned long __s; ");
        c_gen_type(cg, type->base);
        c_print(cg, " __v[%lu]; }", type->num_indices);
        break;
    case TY_C_ARRAY:
        c_gen_type(cg, type->base);
        c_print(cg, "[%ld]", type->num_indices);
        break;
    case TY_FN:
        c_gen_type(cg, type->base);
        c_print(cg, "(*)(");
        for(size_t i = 0; i < type->arg_types->size; i++)
        {
            c_gen_type(cg, type->arg_types->items[i]);
            if(type->arg_types->size - i > 1)
                c_putc(cg, ',');
        }
        c_print(cg, ")");
        break;
    case TY_UNDEF:
        c_print(cg, "%s", c_gen_identifier(type->id));
        break;
    case TY_STRUCT:
        c_gen_struct(cg, type, NULL);
        break;
    case TY_ENUM:
        c_print(cg, "int");
        break;
    default:
        LOG_ERROR_F("cannot generate type %d\n", type->kind);
        break;
    }
}

static void c_gen_typed_name(CCodegenData_T* cg, ASTIdentifier_T* id, ASTType_T* type)
{
    switch(type->kind)
    {
        case TY_C_ARRAY:
            c_gen_type(cg, type->base);
            c_putc(cg, ' ');
            c_print(cg, "%s", c_gen_identifier(id));
            c_print(cg, "[%lu]", type->num_indices);
            break;
        case TY_FN:
            c_gen_type(cg, type->base);
            c_print(cg, "(*%s)(", c_gen_identifier(id));
            for(size_t i = 0; i < type->arg_types->size; i++)
            {
                c_gen_type(cg, type->arg_types->items[i]);
                if(type->arg_types->size - i > 1)
                    c_putc(cg, ',');
            }
            c_putc(cg, ')');
            break;
        default:
            c_gen_type(cg, type);
            c_putc(cg, ' ');
            c_print(cg, "%s", c_gen_identifier(id));
            break;
    }
}

static void c_gen_globals(CCodegenData_T* cg, List_T* objs)
{
    for(size_t i = 0; i < objs->size; i++)
    {
        ASTObj_T* obj = objs->items[i];
        switch(obj->kind)
        {
            case OBJ_NAMESPACE:
                c_gen_globals(cg, obj->objs);
                break;
            
            case OBJ_TYPEDEF:
            {
                ASTType_T* ty = unpack(obj->data_type);
                if(!ty)
                    continue;
                if(ty->kind == TY_ENUM)
                    for(size_t i = 0; i < ty->members->size; i++)
                    {
                        ASTObj_T* member = ty->members->items[i];
                        if(!should_emit(member))
                            continue;
                        char* id = c_gen_identifier(member->id);
                        c_print(cg, "int %s = ", id);
                        c_gen_expr(cg, member->value);
                        c_println(cg, ";");
                    }
            } break;
            
            case OBJ_GLOBAL:
            if(!should_emit(obj))
                continue;
            {
                if(obj->is_extern)
                    c_print(cg, "extern ");
                c_gen_typed_name(cg, obj->id, obj->data_type);
                if(obj->value)
                {
                    c_print(cg, " = ");
                    c_gen_expr(cg, obj->value);
                    c_println(cg, ";");
                }
                else
                    c_println(cg, ";");
            } break;

            default:
                continue;
        }
            
    }
}

static void c_gen_function_declaration(CCodegenData_T* cg, ASTObj_T* obj)
{
    if(obj->is_extern)
        c_print(cg, "extern ");
    
    c_gen_type(cg, obj->return_type);
    c_print(cg, " %s(", c_gen_identifier(obj->id));
    
    for(size_t i = 0; i < obj->args->size; i++)
    {
        ASTObj_T* arg = obj->args->items[i];
        c_gen_typed_name(cg, arg->id, arg->data_type);
        if(obj->args->size - i > 1)
            c_putc(cg, ',');
    }
    
    if(obj->data_type->is_variadic)
        c_print(cg, ",...)");
    else
        c_print(cg, ")");
}

static void c_gen_function_definitions(CCodegenData_T* cg, List_T* objs)
{
    for(size_t i = 0; i < objs->size; i++)
    {
        ASTObj_T* obj = objs->items[i];
        switch(obj->kind) {
        case OBJ_NAMESPACE:
            c_gen_function_definitions(cg, obj->objs);
            break;
        case CSPYDR_OBJ_FUNCTION:
            if(!should_emit(obj))
                continue;
            c_gen_function_declaration(cg, obj);
            c_println(cg, ";");
            break;
        default:
            break;
        }
    }
}

static void c_gen_function(CCodegenData_T* cg, ASTObj_T* fn)
{
    c_gen_function_declaration(cg, fn);
    c_println(cg, "{");
    c_gen_stmt(cg, fn->body);
    c_println(cg, "}");
}   

static void c_gen_functions(CCodegenData_T* cg, List_T* objs)
{
    for(size_t i = 0; i < objs->size; i++)
    {
        ASTObj_T* obj = objs->items[i];
        switch(obj->kind)
        {
        case OBJ_NAMESPACE:
            c_gen_functions(cg, obj->objs);
            break;
        
        case CSPYDR_OBJ_FUNCTION:
            if(obj->is_extern || !should_emit(obj))
                continue;
            c_gen_function(cg, obj);
            break;

        default:
            break;
        }
    }
}

static char* c_detect_registers(CCodegenData_T* cg, const char* str, bool used_registers[C_NUM_REGISTERS])
{
    u64 num_percent = 0;
    for(size_t i = 0; i < strlen(str); i++)
    {
        if(str[i] != '%')
            continue;
        i++;
        num_percent++;
        u64 reg_len = 1;
        for(; isalnum(str[i]); i++, reg_len++);

        Register_T reg = REG_NUM;
        for(i32 j = 0; j < C_NUM_REGISTERS; j++)
        {
            if(strncmp(reg_names[j], &str[i - reg_len], reg_len) == 0)
            {
                used_registers[j] = true;
                reg = j;
            }
        }
        if(reg == REG_NUM)
        {
            LOG_ERROR_F("Unknown register `%s`", strndup(&str[i - reg_len], reg_len));
            unreachable();
        }
    }

    char* copy = calloc(strlen(str) + num_percent, sizeof(char));
    strcpy(copy, str);

    str_replace(copy, str, "%", "%%");

    return copy;
}

static void c_gen_inline_asm(CCodegenData_T* cg, ASTNode_T* node)
{
    c_print(cg, "__asm__ volatile(\n  ");

    bool used_registers[C_NUM_REGISTERS] = {0};

    size_t num_vars = 0;
    for(size_t i = 0; i < node->args->size; i++)
    {
        ASTNode_T* arg = node->args->items[i];
        switch(arg->kind)
        {
            case ND_STR:
                if(strchr(arg->str_val, '%'))
                {
                    char* new = c_detect_registers(cg, arg->str_val, used_registers);
                    c_print(cg, "\"%s\"", new);
                    free(new);
                }
                else
                    c_print(cg, "\"%s\"", arg->str_val);

                break;
            case ND_INT:
                c_print(cg, "\"$%d\"", arg->int_val);
                break;
            case ND_LONG:
                c_print(cg, "\"$%ld\"", arg->long_val);
                break;
            case ND_ULONG:
                c_print(cg, "\"$%lu\"", arg->ulong_val);
                break;
            case ND_ID:
                c_print(cg, "\"%%%lu\"", num_vars++);
                break;
            default:
                unreachable();
        }

        if(node->args->size - i > 0)
            c_putc(cg, ' ');
    }

    c_print(cg, "\n  ::");
    for(size_t i = 0; i < node->args->size; i++)
    {
        ASTNode_T* arg = node->args->items[i];
        if(arg->kind != ND_ID)
            continue;
        c_print(cg, "\"r\"((unsigned long)%s)%c", c_gen_identifier(arg->id), --num_vars ? ',' : '\n');
    }

    c_print(cg, "  :");
    for(size_t i = 0; i < C_NUM_REGISTERS; i++)
    {
        if(!used_registers[i])
            continue;
        
        c_print(cg, "\"%s\",", reg_names[i]);
    }

    c_rewind(cg, -1);
    c_print(cg, "\n)");
}

static void c_gen_index(CCodegenData_T* cg, ASTNode_T* node)
{
    ASTTypeKind_T ty = unpack(node->left->data_type)->kind;
    switch(ty)
    {
        case TY_PTR:
        case TY_FN:
        case TY_C_ARRAY:
            c_print(cg, "(");
            c_gen_expr(cg, node->left);
            c_print(cg, ")[");
            c_gen_expr(cg, node->expr);
            c_print(cg, "]");
            break;
        
        case TY_VLA:
        case TY_ARRAY:
            c_print(cg, "(");
            c_gen_expr(cg, node->left);
            c_print(cg, ")%s__v[", ty == TY_ARRAY ? "." : "->");
            c_gen_expr(cg, node->expr);
            c_putc(cg, ']');
            break;

        default:
            throw_error(ERR_CODEGEN, node->tok, "wrong index type");
    }
}

static void c_gen_expr(CCodegenData_T* cg, ASTNode_T* node)
{
    switch(node->kind)
    {
        case ND_FLOAT:
            c_print(cg, "%ff", node->float_val);
            break;
        case ND_DOUBLE:
            c_print(cg, "%f", node->double_val);
            break;
        case ND_INT:
            c_print(cg, "%d", node->int_val);
            break;
        case ND_BOOL:
            c_print(cg, "%s", node->bool_val ? "_true" : "_false");
            break;
        case ND_LONG:
            c_print(cg, "%ldl", node->long_val);
            break;
        case ND_ULONG:
            c_print(cg, "%lulu", node->ulong_val);
            break;
        case ND_CHAR:
            c_print(cg, "((char) %d)", node->int_val);
            break;
        case ND_STR:
            c_print(cg, "\"%s\"", node->str_val);
            break;
        case ND_NIL:
            c_print(cg, "((void*) 0)");
            break;
        case ND_SIZEOF:
            c_print(cg, "((unsigned long) %d)", node->the_type->size);
            break;
        case ND_ALIGNOF:
            c_print(cg, "((unsigned long) %d)", node->the_type->align);
            break;
        case ND_LEN:
            {
                ASTType_T* ty = unpack(node->expr->data_type);
                switch(ty->kind)
                {
                case TY_C_ARRAY:
                    c_print(cg, "%lu", ty->num_indices);
                    break;
                case TY_ARRAY:
                    c_putc(cg, '(');
                    c_gen_expr(cg, node->expr);
                    c_print(cg, ".__s)");
                    break;
                case TY_VLA:
                    c_putc(cg, '(');
                    c_gen_expr(cg, node->expr);
                    c_print(cg, "->__s)");
                    break;
                case TY_PTR:
                    if(unpack(ty->base)->kind == TY_CHAR)
                    {
                        c_print(cg, "_inline_strlen(");
                        c_gen_expr(cg, node->expr);
                        c_putc(cg, ')');
                        break;
                    }
                    // fall through

                default:
                    LOG_ERROR_F("len not implemented for type %d.\n", ty->kind);
                    unreachable();
                    break;                    
                }
            } break;
        case ND_NEG:
        case ND_BIT_NEG:
        case ND_NOT:
        case ND_REF:
        case ND_DEREF:
            c_print(cg, "(%s", op_symbols[node->kind]);
            c_gen_expr(cg, node->right);
            c_putc(cg, ')');
            break;
        case ND_INC:
        case ND_DEC:
            c_putc(cg, '(');
            c_gen_expr(cg, node->left);
            c_print(cg, "%s)", node->kind == ND_INC ? "++" : "--");
            break;
        case ND_ADD:
        case ND_SUB:
        case ND_MUL:
        case ND_DIV:
        case ND_MOD:
        case ND_EQ:
        case ND_NE:
        case ND_GT:
        case ND_GE:
        case ND_LT:
        case ND_LE:
        case ND_AND:
        case ND_OR:
        case ND_LSHIFT:
        case ND_RSHIFT:
        case ND_XOR:
        case ND_BIT_OR:
        case ND_BIT_AND:
        case ND_ASSIGN:
            c_putc(cg, '(');
            c_gen_expr(cg, node->left);
            c_print(cg, "%s", op_symbols[node->kind]);
            c_gen_expr(cg, node->right);
            c_putc(cg, ')');
            break;
        case ND_ARRAY:
            c_print(cg, "{%lu,{", unpack(node->data_type)->num_indices);
            for(size_t i = 0; i < node->args->size; i++)
            {
                c_gen_expr(cg, node->args->items[i]);
                if(node->args->size - i > 1)
                    c_putc(cg, ',');
            }
            c_print(cg, "}}");
            break;
        case ND_STRUCT:
            if(node->data_type->kind == TY_UNDEF)
            {
                c_print(cg, "(%s)", c_gen_identifier(node->data_type->id));
            }

            if(node->args->size == 0)
            {
                c_print(cg, "{0}");
                break;
            }

            c_putc(cg, '{');
            for(size_t i = 0; i < node->args->size; i++)
            {
                c_gen_expr(cg, node->args->items[i]);
                c_putc(cg, node->args->size - i > 1 ? ',' : '}');
            }
            break;
        case ND_CAST:
            c_print(cg, "((");
            c_gen_type(cg, node->data_type);
            c_putc(cg, ')');
            c_gen_expr(cg, node->left);
            c_putc(cg, ')');
            break;
        case ND_ID:
            c_print(cg, "%s", c_gen_identifier(node->id));
            break;
        case ND_CALL:
            c_gen_expr(cg, node->expr);
            c_putc(cg, '(');
            for(size_t i = 0; i < node->args->size; i++)
            {
                c_gen_expr(cg, node->args->items[i]);
                if(node->args->size - i > 1)
                    c_print(cg, ",");
            }
            c_putc(cg, ')');
            break;
        case ND_ASM:
            c_gen_inline_asm(cg, node);
            break;
        case ND_CLOSURE:
            c_putc(cg, '(');
            for(size_t i = 0; i < node->exprs->size; i++)
            {
                c_gen_expr(cg, node->exprs->items[i]);
                c_putc(cg, node->exprs->size - i > 1 ? ',' : ')');
            }
            break;
        case ND_INDEX:
            c_gen_index(cg, node);
            break;
        case ND_MEMBER:
            c_print(cg, "((");
            c_gen_expr(cg, node->left);
            c_print(cg, ").");
            c_gen_expr(cg, node->right);
            c_putc(cg, ')');
            break;
        case ND_TERNARY:
            c_print(cg, "((");
            c_gen_expr(cg, node->condition);
            c_print(cg, ")?");
            c_gen_expr(cg, node->if_branch);
            c_putc(cg, ':');
            c_gen_expr(cg, node->else_branch);
            c_putc(cg, ')');
            break;

        default:
            LOG_ERROR_F("expr gen for %d unimplemented.\n", node->kind);
            unreachable();
    }
}

static void c_init_zero(CCodegenData_T* cg, ASTObj_T* var)
{
    switch(unpack(var->data_type)->kind)
    {
    case TY_U8:
    case TY_I8:
    case TY_U16:
    case TY_I16:
    case TY_U32:
    case TY_I32:
    case TY_U64:
    case TY_I64:
    case TY_BOOL:
    case TY_CHAR:
    case TY_ENUM:
        c_putc(cg, '0');
        break;
    case TY_F32:
        c_print(cg, "0.0f");
        break;
    case TY_F64:
    case TY_F80:
        c_print(cg, "0.0");
        break;
    case TY_PTR:
    case TY_VLA:
    case TY_FN:
        c_print(cg, "(void*) 0");
        break;
    case TY_ARRAY:
    case TY_C_ARRAY:
    case TY_STRUCT:
        c_print(cg, "{0}");
        break;
    default:
        unreachable();
    }
}

static void c_gen_local(CCodegenData_T* cg, ASTObj_T* var)
{
    c_gen_typed_name(cg, var->id, var->data_type);
    if(!var->value)
    {
        c_putc(cg, '=');
        c_init_zero(cg, var);
    }
    c_println(cg, ";");
}

static void c_gen_stmt(CCodegenData_T* cg, ASTNode_T* node)
{
    switch(node->kind)
    {
    case ND_BLOCK:
        c_println(cg, "{");
        for(size_t i = 0; i < node->locals->size; i++)
        {
            ASTObj_T* local = node->locals->items[i];
            c_gen_local(cg, local);
        }

        for(size_t i = 0; i < node->stmts->size; i++)
        {
            ASTNode_T* stmt = node->stmts->items[i];
            c_gen_stmt(cg, stmt);
        }

        c_println(cg, "}");
        break;

    case ND_IF:
        c_print(cg, "if(");
        c_gen_expr(cg, node->condition);
        c_println(cg, "){");
        c_gen_stmt(cg, node->if_branch);
        if(node->else_branch)
        {
            c_println(cg, "} else {");
            c_gen_stmt(cg, node->else_branch);
        }
        c_println(cg, "}");
        break;
    
    case ND_LOOP:
        c_println(cg, "for(;;){");
        c_gen_stmt(cg, node->body);
        c_println(cg, "}");
        break;
    
    case ND_WHILE:
        c_print(cg, "while(");
        c_gen_expr(cg, node->condition);
        c_println(cg, "){");
        c_gen_stmt(cg, node->body);
        c_println(cg, "}");
        break;

    case ND_DO_WHILE:
        c_println(cg, "do{");
        c_gen_stmt(cg, node->body);
        c_print(cg, "} while(");
        c_gen_expr(cg, node->condition);
        c_println(cg, ");");
        break;
    
    case ND_DO_UNLESS:
        c_print(cg, "if(!(");
        c_gen_expr(cg, node->condition);
        c_println(cg, ")){");
        c_gen_stmt(cg, node->body);
        c_println(cg, "}");
        break;

    case ND_EXPR_STMT:
        c_gen_expr(cg, node->expr);
        c_println(cg, ";");
        break;

    case ND_BREAK:
        c_println(cg, "break;");
        break;

    case ND_CONTINUE:
        c_println(cg, "continue;");
        break;
    
    case ND_RETURN:
        if(node->return_val) 
        {
            c_print(cg, "return ");
            c_gen_expr(cg, node->return_val);
            c_println(cg, ";");
        }
        else
            c_println(cg, "return;");
        break;
    
    case ND_USING:
        if(node->body)
            c_gen_stmt(cg, node->body);
        break;
    
    case ND_FOR:    
        c_print(cg, "for(");
        
        if(node->init_stmt)
        {
            c_gen_stmt(cg, node->init_stmt);
            c_print(cg, "    ");
        }
        else
            c_putc(cg, ';');

        if(node->condition)
            c_gen_expr(cg, node->condition);
        c_putc(cg, ';');
        
        if(node->expr)
            c_gen_expr(cg, node->expr);
        c_println(cg, "){");
        c_gen_stmt(cg, node->body);
        c_println(cg, "}");
        break;

    case ND_FOR_RANGE:
    {
        u64 low_id = cg->unique_id++, high_id = cg->unique_id++;
        c_print(cg, "for(%s _unique_id_%04lux = ", c_primitive_types[TY_U64], low_id);
        c_gen_expr(cg, node->left);
        c_print(cg, ", _unique_id_%04lux = (%s)", high_id, c_primitive_types[TY_U64]);
        c_gen_expr(cg, node->right);
        c_println(cg, "; _unique_id_%04lux < _unique_id_%04lux; _unique_id_%04lux++){", low_id, high_id, low_id);
        c_gen_stmt(cg, node->body);
        c_println(cg, "}");
    } break;

    case ND_MATCH:
    {
        u64 uid = cg->unique_id;
        c_print(cg, "{\n%s _unique_id_%04lux =", c_primitive_types[TY_U64], uid);
        c_gen_expr(cg, node->condition);
        c_println(cg, ";");
        
        for(size_t i = 0; i < node->cases->size; i++)
        {
            ASTNode_T* _case = node->cases->items[i];
            c_print(cg, "%sif(_unique_id_%04lux == (", i == 0 ? "" : "else ", uid);
            c_gen_expr(cg, _case->condition);
            c_println(cg, ")){");
            c_gen_stmt(cg, _case->body);
            c_println(cg, "}");
        }

        if(node->default_case)
        {
            c_println(cg, "else{");
            c_gen_stmt(cg, node->default_case->body);
            c_putc(cg, '}');
        }

        c_println(cg, "}");
    } break;

    case ND_WITH:
    {
        c_gen_expr(cg, node->condition);
        c_print(cg, ";\nif((");
        c_gen_expr(cg, node->condition->left);
        c_println(cg, ") != 0){");
        c_gen_stmt(cg, node->if_branch);
        
        if(node->else_branch)
        {
            c_println(cg, "} else {");
            c_gen_stmt(cg, node->else_branch);
        }

        c_println(cg, "}");
    } break;

    case ND_NOOP:
        break;

    default:
        LOG_ERROR_F("stmt gen for %d unimplemented.\n", node->kind);
        unreachable();
    }
}