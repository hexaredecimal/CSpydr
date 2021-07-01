#ifndef CSPYDR_LINUX_PLATFORM_H
#define CSPYDR_LINUX_PLATFORM_H

#if defined(__linux__) || defined (__linux)

#include <stdlib.h>
#include <linux/limits.h>
// the default output file used for code generation
#define DEFAULT_OUTPUT_FILE "a.out"
// the characters between directories e.g.: /home/usr/...
#define DIRECTORY_DELIMS "/"

#define CACHE_DIR ".cache/cspydr"
char* get_home_directory();



char* get_absolute_path(char* relative_path);
char* get_path_from_file(char* file_path);

#endif
#endif