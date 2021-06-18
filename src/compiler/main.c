/*
    THE CSPYDR PROGRAMMING LANGUAGE COMPILER
    This is the main file and entry point to the compiler.

    This compiler and all components of CSpydr, except external dependencies (LLVM, acutest, ...), are licensed under the GNU General Public License v3.0.

    Creator:
        https://github.com/spydr06
    Official git repository:
        https://github.com/spydr06/cspydr.git
*/

// std includes
#include <string.h>

// compiler includes
#include "io/io.h"
#include "io/log.h"
#include "version.h"
#include "parser/parser.h"
#include "parser/preprocessor.h"
#include "codegen/llvm/llvm_codegen.h"
#include "codegen/transpiler/c_codegen.h"
#include "platform/platform_bindings.h"

// default texts, which get shown if you enter help, info or version flags
// links to me, the creator of CSpydr
// please be nice and don't change them without any reason. You may add yourself to the credits, if you changed something
#define CSPYDR_GIT_REPOSITORY "https://github.com/spydr06/cspydr.git"
#define CSPYDR_GIT_DEVELOPER "https://github.com/spydr06"

const char* usage_text = COLOR_BOLD_WHITE "Usage:" COLOR_RESET " cspydr [run, build, debug] <input file> [<flags>]\n"
                         "       cspydr [--help, --info, --version]\n";

// this text gets shown if -i or --info is used
const char* info_text = COLOR_BOLD_YELLOW "** THE CSPYDR PROGRAMMING LANGUAGE COMPILER **\n" COLOR_RESET
                       COLOR_BOLD_WHITE "Version:" COLOR_RESET " %s\n"
                       COLOR_BOLD_WHITE "Build:" COLOR_RESET " %s\n"
                       "\n"
                       "Copyright (C) 2021 Spydr06\n"
                       "CSpydr is distributed under the GNU General Public License (v3)\n"
                       "This is free software; see the source for copying conditions;\n"
                       "you may redistribute it under the terms of the GNU GPL version 3\n"
                       "or (at your option) a later version.\n"
                       "This program has absolutely no warranty.\n"
                       "\n"
                       COLOR_BOLD_WHITE "    repository: " COLOR_RESET CSPYDR_GIT_REPOSITORY "\n"
                       COLOR_BOLD_WHITE "    developer:  " COLOR_RESET CSPYDR_GIT_DEVELOPER "\n"
                       "\n"
                       "Type -h or --help for help page.\n";

// this text gets shown if -h or --help is used
const char* help_text = "%s"
                       COLOR_BOLD_WHITE "Actions:\n" COLOR_RESET
                       "  build    Builds a cspydr program to a binary to execute.\n"
                       "  run      Builds, then runs a cspydr program directly.\n"
                       "  debug    Runs a cspydr program with special debug tools. [!!NOT IMPLEMENTED YET!!]\n"
                       COLOR_BOLD_WHITE "Options:\n" COLOR_RESET
                       "  -h, --help          Displays this help text and quits.\n"
                       "  -v, --version       Displays the version of CSpydr and quits.\n"
                       "  -i, --info          Displays information text and quits.\n"
                       "  -o, --output [file] Sets the target output file (default: " DEFAULT_OUTPUT_FILE ")\n"
                       "  -t, --transpile     Instructs the compiler to compile to C source code\n"
                       "  -l, --llvm          Instructs the compiler to compile to LLVM BitCode (default)\n"
                       "      --print-llvm    Prints the generated LLVM bitcode\n"
                       "      --print-c       Prints the generated C code\n"
                       "\n"
                       "If you are unsure, what CSpydr is (or how to use it), please check out the GitHub repository: \n" CSPYDR_GIT_REPOSITORY "\n";

// this text gets shown if -v or --version is used
const char* version_text = COLOR_BOLD_YELLOW "** THE CSPYDR PROGRAMMING LANGUAGE COMPILER **\n" COLOR_RESET
                          COLOR_BOLD_WHITE "Version:" COLOR_RESET " %s\n"
                          COLOR_BOLD_WHITE "Build:" COLOR_RESET " %s\n"
                          "\n"
                          "For more information type -i.\n";

typedef enum ACTION_ENUM
{
    AC_BUILD,
    AC_RUN,
    AC_DEBUG,
    AC_UNDEF
} Action_T;

typedef enum COMPILE_TYPE_ENUM
{
    CT_LLVM,
    CT_TRANSPILE
} CompileType_T;

const struct { char* as_str; Action_T ac; } action_table[AC_UNDEF] = {
    {"build", AC_BUILD},
    {"run",   AC_RUN},
    {"debug", AC_DEBUG},
};

// declaration of the functions used below
extern const char* get_cspydr_version();
extern const char* get_cspydr_build();
void compile_llvm(char* path, char* target, Action_T action, bool print_llvm);
void transpile_c(char* path, char* target, Action_T action, bool print_c);

static inline bool streq(char* a, char* b)
{
    return strcmp(a, b) == 0;
}

static void evaluate_info_flags(char* argv)
{
    if(streq(argv, "-h") || streq(argv, "--help"))
        printf(help_text, usage_text);
    else if(streq(argv, "-i") || streq(argv, "--info"))
        printf(info_text, get_cspydr_version(), get_cspydr_build());
    else if(streq(argv, "-v") || streq(argv, "--version"))
        printf(version_text, get_cspydr_version(), get_cspydr_build());
    else
        LOG_ERROR_F("unknown or wrong used flag \"%s\", type \"cspydr --help\" to get help.", argv);

    exit(1);
}

// entry point
int main(int argc, char* argv[])
{
    if(argc == 1)
    {
        LOG_ERROR_F("[Error] Too few arguments given.\n" COLOR_RESET "%s", usage_text);
        exit(1);
    }

    // if there are 2 args, check for --help, --info or --version flags
    if(argc == 2)
        evaluate_info_flags(argv[1]);

    // get the action to perform
    Action_T action = AC_UNDEF;
    CompileType_T ct = CT_LLVM;
    for(int i = 0; i < AC_UNDEF; i++)
        if(streq(argv[1], action_table[i].as_str))
            action = action_table[i].ac;
    if(action == AC_UNDEF)
    {
        LOG_ERROR_F("[Error] Unknown action \"%s\", expect [build, run, debug]\n", argv[1]);
    }

    // declare the input/output files
    char* output_file = DEFAULT_OUTPUT_FILE;
    char* input_file = argv[2];
    if(!file_exists(input_file))
    {
        LOG_ERROR_F("[Error] Error opening file \"%s\": No such file or directory\n", input_file);
        exit(1);
    }

    // remove the first three flags form argc/argv
    argc -= 3;
    argv += 3;

    bool print_llvm = false;
    bool print_c = false;

    // get all the other flags
    for(int i = 0; i < argc; i++)
    {
        char* arg = argv[i];

        if(streq(arg, "-o") || streq(arg, "--output"))
        {
            if(!argv[++i])
            {
                LOG_ERROR("[Error] Expect target file path after -o/--output.\n");
                exit(1);
            }
            output_file = argv[i];
        }
        else if(streq(arg, "--print-llvm"))
            print_llvm = true;
        else if(streq(arg, "--print-c"))
            print_c = true;
        else if(streq(arg, "-t") || streq(arg, "--transpile"))
            ct = CT_TRANSPILE;
        else if(streq(arg, "-l") || streq(arg, "--llvm"))
            ct = CT_LLVM;
        else
        {
            LOG_ERROR_F("[Error] Unknown flag \"%s\", type \"cspydr --help\" to get help.\n", argv[i]);
            exit(1);
        }
    }

    switch(ct)
    {
        case CT_LLVM:
            compile_llvm(input_file, output_file, action, print_llvm);
            break;
        case CT_TRANSPILE:
            transpile_c(input_file, output_file, action, print_c);
            break;
        default:
            LOG_ERROR_F("[Error] Unknown compile type %d!\n", ct);
            return 1;
    }

    return 0;
}

// sets up and runs the compilation pipeline using LLVM
void compile_llvm(char* path, char* target, Action_T action, bool print_llvm)
{
    SrcFile_T* main_file = read_file(path);

    ASTProg_T* ast = parse_file(init_list(sizeof(char*)), main_file, false);
    List_T* imports = ast->imports;

    for(size_t i = 0; i < imports->size; i++)
    {
        SrcFile_T* import_file = read_file(imports->items[i]);

        ASTProg_T* import_ast = parse_file(imports, import_file, false);
        merge_ast_progs(ast, import_ast);

        free_srcfile(import_file);
    }

    preprocess(ast);

    LLVMCodegenData_T* cg = init_llvm_cg(ast);
    cg->print_ll = print_llvm;
    llvm_gen_code(cg);

    switch(action)
    {
        case AC_BUILD:
            llvm_emit_code(cg, target);
            break;
        case AC_RUN:
            llvm_run_code(cg);
            break;
        case AC_DEBUG:
            // TODO:
            break;
        default:
            LOG_ERROR_F("Unrecognized action of type [%d], exit\n", action);
            exit(1);
    }

    free_llvm_cg(cg);

    free_srcfile(main_file);
}

void transpile_c(char* path, char* target, Action_T action, bool print_c)
{
    SrcFile_T* main_file = read_file(path);

    ASTProg_T* ast = parse_file(init_list(sizeof(char*)), main_file, false);
    List_T* imports = ast->imports;

    for(size_t i = 0; i < imports->size; i++)
    {
        SrcFile_T* import_file = read_file(imports->items[i]);

        ASTProg_T* import_ast = parse_file(imports, import_file, false);
        merge_ast_progs(ast, import_ast);

        free_srcfile(import_file);
    }

    preprocess(ast);

    CCodegenData_T* cg = init_c_cg(ast);
    cg->print_c = print_c;
    c_gen_code(cg, target);

    if(action == AC_RUN)
        run_c_code(cg, target);
    
    free_c_cg(cg);
    free_srcfile(main_file);
}