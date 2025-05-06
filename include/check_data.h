#ifndef _check_data_h_
#define _check_data_h_

#include "tables_dict.h"

extern bool debug;

ibool check_datetime(ulonglong ldate);
ibool check_char_ascii(char *value, ulint len);
ibool check_char_digits(char *value, ulint len);
ibool check_field_limits(field_def_t *field, byte *value, ulint len);

extern inline ulonglong make_ulonglong(dulint x);
extern inline longlong make_longlong(dulint x);

unsigned long long int get_uint_value(field_def_t *field, byte *value);
long long int get_int_value(field_def_t *field, byte *value);

#endif
