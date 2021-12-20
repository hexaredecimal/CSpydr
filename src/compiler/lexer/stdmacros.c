#include "stdmacros.h"
#include "preprocessor.h"
#include "../version.h"
#include "token.h"

#include <time.h>
#include <string.h>
#include <stdio.h>

const char* get_date_str(void);
const char* get_time_str(void);
const char* get_compile_type(void);

char current_time[128] = { '\0' };
char current_date[128] = { '\0' };

struct {
    const char* id;
    union {
        const char* value;
        const char* (*fn)(void);
    };
    enum {
        INTEGER,
        STRING,
        STRING_FN
    } value_type;
} 
macros[] = {
    {"__version__",  .value = "v" CSPYDR_VERSION_X "." CSPYDR_VERSION_Y "." CSPYDR_VERSION_Z CSPYDR_VERSION_W, STRING},
#if defined(__linux) || defined(__linux__)
    {"__system__", .value = "linux", STRING},
#elif defined(_WIN32)
    {"__system__", .value = "windows", STRING},
#endif

#if defined(__x86_64) || defined(__x86_64__) || defined(_M_X64)
    {"__architecture__", .value =  "x86_64", STRING},
#elif defined(__x86) || defined(__x86__) || defined(_M_X86)
    {"__architecture__", .value = "x86", STRING},
#else
    {"__architecture__", .value = "unknown", STRING},
#endif

    {"__time__", .fn = get_time_str, STRING_FN},
    {"__date__", .fn = get_date_str, STRING_FN},
    {"__compile_type__", .fn = get_compile_type, STRING_FN},
    {NULL, NULL, 0}
};

void define_std_macros(List_T *macro_list)
{
    for(int i = 0; macros[i].id; i++)
    {
        Token_T* main_token = init_token((char*) macros[i].id, 0, 0, TOKEN_ID, NULL);
        Macro_T* macro = init_macro(main_token);

        Token_T* replacement_token;
        switch(macros[i].value_type)
        {
            case INTEGER:
                replacement_token = init_token((char*) macros[i].value, 0, 0, TOKEN_INT, NULL);
                break;
            case STRING:
                replacement_token = init_token((char*) macros[i].value, 0, 0, TOKEN_STRING, NULL);
                break;
            case STRING_FN:
                replacement_token = init_token((char*) macros[i].fn(), 0, 0, TOKEN_STRING, NULL);
                break;
        }

        list_push(macro->replacing_tokens, replacement_token);
        list_push(macro_list, macro);
    }
}

const char* get_time_str(void)
{
    if(!current_time[0])
    {
        time_t raw_time;
        struct tm* time_info;

        time(&raw_time);
        time_info = localtime(&raw_time);

        sprintf(current_time, "%d:%d:%d", time_info->tm_hour, time_info->tm_min, time_info->tm_sec);
        printf("time is: %s\n", current_time);
    }

    return current_time;
}

const char* get_date_str(void)
{
    if(!current_date[0])
    {
        time_t raw_time;
        struct tm* time_info;

        time(&raw_time);
        time_info = localtime(&raw_time);

        sprintf(current_date, "%d.%d.%d", time_info->tm_mday, time_info->tm_mon + 1, time_info->tm_year + 1900);
    }

    return current_date;
}

const char* get_compile_type(void)
{
    return ""; // todo: implement this!
}