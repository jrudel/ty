#ifndef PARSE_H_INCLUDED
#define PARSE_H_INCLUDED

#include "token.h"
#include "table.h"
#include "ast.h"

char const *
parse_error(void);

struct table *
parse_module_table(void);

struct statement **
parse(char const *source, char const *file);

#endif
