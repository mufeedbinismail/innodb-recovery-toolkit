#ifndef _print_data_h_
#define _print_data_h_

#include <univ.i>
#include <page0page.h>
#include <rem0rec.h>
#include <btr0cur.h>

#include "tables_dict.h"

extern bool debug;

void print_datetime(ulonglong ldate);
void print_date(ulong ldate);
void print_time(ulong ltime);

void print_enum(int value, field_def_t *field);
void print_field_value(byte *value, ulint len, field_def_t *field);
void print_field_value_with_external(byte *value, ulint len, field_def_t *field);
void print_string(char *value, ulint len, field_def_t *field);
void print_decimal(byte *value, field_def_t *field);
void print_string_raw(char *value, ulint len);

void rec_print_new(FILE* file, rec_t* rec, const ulint* offsets);

#endif
