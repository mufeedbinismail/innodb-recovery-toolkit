#include <stdio.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <sys/dir.h>

#include <error.h>
#include <tables_dict.h>
#include <print_data.h>
#include <check_data.h>
#include <dirent.h>


// Global flags from getopt
bool deleted_pages_only = 0;
bool deleted_records_only = 0;
bool undeleted_records_only = 0;
bool debug = 0;
bool process_redundant = 0;
bool process_compact = 0;
char blob_dir[256] = ".";
char dump_prefix[256] = "default";

dulint filter_id;
bool use_filter_id = 0;

/*******************************************************************/
ulint process_ibrec(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets) {
	ulint data_size;
	int i;

	// Print table name
	if (debug) {
		printf("Processing record %p from table '%s'\n", rec, table->name);
		rec_print_new(stdout, rec, offsets);
	} else {
		printf("%s\t", table->name);
	}

	data_size = rec_offs_data_size(offsets);

	for(i = 0; i < table->fields_count; i++) {
		ulint len;
		byte *field = rec_get_nth_field(rec, offsets, i, &len);
		
		if (table->fields[i].type == FT_INTERNAL) continue;
		
		if (debug) printf("Field #%i @ %p: length %lu, value: ", i, field, len);

		if (len == UNIV_SQL_NULL) {
			printf("NULL");
		} else {
            if (rec_offs_nth_extern(offsets, i)) {
                print_field_value_with_external(field, len, &(table->fields[i]));
                } else {
			        print_field_value(field, len, &(table->fields[i]));
                    }
		    }
		
		if (i < table->fields_count - 1) printf("\t");
		if (debug) printf("\n");
	}

	printf("\n");
	return data_size; // point to the next possible record's start
}

/*******************************************************************/
static inline ibool check_constraints(rec_t *rec, table_def_t* table, ulint* offsets) {
	int i;
    ulint len_sum = 0;
	
	if (debug) {
		printf("\nChecking constraints for a row (%s) at %p:", table->name, rec);
		ut_print_buf(stdout, rec, 100);
	}
	
	// Check every field
	for(i = 0; i < table->fields_count; i++) {
		// Get field value pointer and field length
		ulint len;
		byte *field = rec_get_nth_field(rec, offsets, i, &len);
		if (debug) printf("\n - field %s(addr = %p, len = %lu):", table->fields[i].name, field, len);

        if (len != UNIV_SQL_NULL) {
            len_sum += len;
            }
        else {
            if (!rec_offs_comp(offsets)) {
                len_sum += rec_get_nth_field_size(rec, i);
                }
            }
                

		// Skip null fields from type checks and fail if null is not allowed by data limits
		if (len == UNIV_SQL_NULL) {
			if (table->fields[i].has_limits && !table->fields[i].limits.can_be_null) {
				if (debug) printf("data can't be NULL");
				return FALSE;
			}
			continue;
		}
		
		// Check limits
		if (!table->fields[i].has_limits) continue;
		if (!check_field_limits(&(table->fields[i]), field, len)) {
			if (debug) printf("LIMITS check failed(field = %p, len = %ld)!\n", field, len);
			return FALSE;
		}
	}

    // Why do we need this check?
    /*
    if (len_sum != rec_offs_data_size(offsets)) {
        fprintf(stderr,
            "\nInnoDB: Error: record len should be %lu, len %lu\n",
            (ulong) len_sum,
            (ulong) rec_offs_data_size(offsets));
            return FALSE;
            }
    */

	if (debug) printf("\nRow looks OK!\n");
	return TRUE;
}

/*******************************************************************/
inline ibool check_fields_sizes(rec_t *rec, table_def_t *table, ulint *offsets) {
	int i;

	if (debug) {
		printf("\nChecking field lengths for a row (%s): ", table->name);
		printf("OFFSETS: ");
		for(i = 0; i < rec_offs_n_fields(offsets); i++) {
			printf("%lu ", rec_offs_base(offsets)[i]);
		}
//		printf("\n");
	}
	
	// check every field
	for(i = 0; i < table->fields_count; i++) {
		// Get field size
		ulint len = rec_offs_nth_size(offsets, i);
		if (debug) printf("\n - field %s(%lu):", table->fields[i].name, len);
		
		// If field is null
		if (len == UNIV_SQL_NULL) {
			// Check if it can be null and jump to a next field if it is OK
			if (table->fields[i].can_be_null) continue;
			// Invalid record where non-nullable field is NULL
			if (debug) printf("Can't be NULL or zero-length!\n");
			return FALSE;
		} 
		
		// Check size of fixed-length field
		if (table->fields[i].fixed_length) {
			// Check if size is the same and jump to the next field if it is OK
			if (len == table->fields[i].fixed_length || (len == 0 && table->fields[i].can_be_null)) continue;
			// Invalid fixed length field
			if (debug) printf("Invalid fixed length field size: %lu, but should be %u!\n", len, table->fields[i].fixed_length);
			return FALSE;
		}
		
		// Check if has externally stored data 
		if (rec_offs_nth_extern(offsets, i)) {
			if (debug) printf("\nEXTERNALLY STORED VALUE FOUND in field %i\n", i);
			if (table->fields[i].type == FT_TEXT || table->fields[i].type == FT_BLOB) continue;
			if (debug) printf("Invalid external data flag!\n");
			return FALSE;
		}
		
		// Check size limits for varlen fields
		if (len < table->fields[i].min_length || len > table->fields[i].max_length) {
			if (debug) printf("Length limits check failed (%lu < %u || %lu > %u)!\n", len, table->fields[i].min_length, len, table->fields[i].max_length);
			return FALSE;
		}

		if (debug) printf("OK!");
	}

	if (debug) printf("\n");
	return TRUE;
}

/*******************************************************************/
static inline ibool ibrec_init_offsets_new(page_t *page, rec_t* rec, table_def_t* table, ulint* offsets) {
	ulint i = 0;
	ulint offs;
	const byte* nulls;
	const byte* lens;
	ulint null_mask;
	ulint status = rec_get_status(rec);

	// Skip non-ordinary records
	if (status != REC_STATUS_ORDINARY) return FALSE;

	// First field is 0 bytes from origin point
	rec_offs_base(offsets)[0] = 0;
	
	// Init first bytes
	rec_offs_set_n_fields(offsets, table->fields_count);
	
	nulls = rec - (REC_N_NEW_EXTRA_BYTES + 1);
	lens = nulls - (table->n_nullable + 7) / 8;
	offs = 0;
	null_mask = 1;
	
	/* read the lengths of fields 0..n */
	do {
		ulint	len;
		field_def_t *field = &(table->fields[i]);

		/* nullable field => read the null flag */
		if (field->can_be_null) {
//			if (debug) printf("nullable field => read the null flag\n");
			if (!(byte)null_mask) {
				nulls--;
				null_mask = 1;
			}

			if (*nulls & null_mask) {
				null_mask <<= 1;
				/* No length is stored for NULL fields.
				We do not advance offs, and we set
				the length to zero and enable the
				SQL NULL flag in offsets[]. */
				len = offs | REC_OFFS_SQL_NULL;
				goto resolved;
			}
			null_mask <<= 1;
		}

		if (!field->fixed_length) {
//			if (debug) printf("Variable-length field: read the length\n");
			/* Variable-length field: read the length */
			len = *lens--;

			if (field->max_length > 255 || field->type == FT_BLOB || field->type == FT_TEXT) {
				if (len & 0x80) {
					/* 1exxxxxxx xxxxxxxx */
					len <<= 8;
					len |= *lens--;

					offs += len & 0x3fff;
					if (len	& 0x4000) {
						len = offs | REC_OFFS_EXTERNAL;
					} else {
						len = offs;
					}

					goto resolved;
				}
			}

			len = offs += len;
		} else {
			len = offs += field->fixed_length;
		}
	resolved:
        offs &= 0xffff;
		if (rec + offs - page > UNIV_PAGE_SIZE) {
			if (debug) printf("Invalid offset for field %lu: %lu\n", i, offs);
			return FALSE;
		}
		rec_offs_base(offsets)[i + 1] = len;
	} while (++i < table->fields_count);

	return TRUE;
}

/*******************************************************************/
static inline ibool ibrec_init_offsets_old(page_t *page, rec_t* rec, table_def_t* table, ulint* offsets) {
	ulint i = 0;
	ulint offs;

	// First field is 0 bytes from origin point
	rec_offs_base(offsets)[0] = 0;
	
	// Init first bytes
	rec_offs_set_n_fields(offsets, table->fields_count);
		
	/* Old-style record: determine extra size and end offsets */
	offs = REC_N_OLD_EXTRA_BYTES;
	if (rec_get_1byte_offs_flag(rec)) {
		offs += rec_offs_n_fields(offsets);
		*rec_offs_base(offsets) = offs;
		/* Determine offsets to fields */
		do {
			offs = rec_1_get_field_end_info(rec, i);
			if (offs & REC_1BYTE_SQL_NULL_MASK) {
				offs &= ~REC_1BYTE_SQL_NULL_MASK;
				offs |= REC_OFFS_SQL_NULL;
			}

            offs &= 0xffff;
    		if (rec + offs - page > UNIV_PAGE_SIZE) {
    			if (debug) printf("Invalid offset for field %lu: %lu\n", i, offs);
    			return FALSE;
    		}

			rec_offs_base(offsets)[1 + i] = offs;
		} while (++i < rec_offs_n_fields(offsets));
	} else {
		offs += 2 * rec_offs_n_fields(offsets);
		*rec_offs_base(offsets) = offs;
		/* Determine offsets to fields */
		do {
			offs = rec_2_get_field_end_info(rec, i);
			if (offs & REC_2BYTE_SQL_NULL_MASK) {
				offs &= ~REC_2BYTE_SQL_NULL_MASK;
				offs |= REC_OFFS_SQL_NULL;
			}

			if (offs & REC_2BYTE_EXTERN_MASK) {
				offs &= ~REC_2BYTE_EXTERN_MASK;
				offs |= REC_OFFS_EXTERNAL;
			}

            offs &= 0xffff;
    		if (rec + offs - page > UNIV_PAGE_SIZE) {
    			if (debug) printf("Invalid offset for field %lu: %lu\n", i, offs);
    			return FALSE;
    		}

			rec_offs_base(offsets)[1 + i] = offs;
		} while (++i < rec_offs_n_fields(offsets));
	}
	
	return TRUE;	
}

/*******************************************************************/
ibool check_for_a_record(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets) {
	ulint offset, data_size;
    int flag;

	// Check if given origin is valid
	offset = rec - page;
	if (offset < record_extra_bytes + table->min_rec_header_len) return FALSE;
	if (debug) printf("ORIGIN=OK ");

    flag = rec_get_deleted_flag(rec, page_is_comp(page));
    if (debug) printf("DELETED=0x%X ", flag);
	// Skip non-deleted records
	if (deleted_records_only && flag == 0) return FALSE;

	// Skip deleted records
	if (undeleted_records_only && flag != 0) return FALSE;
	
    // Get field offsets for current table
	if (process_compact && !ibrec_init_offsets_new(page, rec, table, offsets)) return FALSE;
	if (process_redundant && !ibrec_init_offsets_old(page, rec, table, offsets)) return FALSE;
	if (debug) printf("OFFSETS=OK ");

	// Check the record's data size
	data_size = rec_offs_data_size(offsets);
	if (data_size > table->data_max_size) {
        if (debug) printf("DATA_SIZE=FAIL(%lu > %ld) ", (long int)data_size, (long int)table->data_max_size);
        return FALSE;
	}
	if (data_size < table->data_min_size) {
        if (debug) printf("DATA_SIZE=FAIL(%lu < %lu) ", (long int)data_size, (long int)table->data_min_size);
        return FALSE;
	}
	if (debug) printf("DATA_SIZE=OK ");

	// Check fields sizes
	if (!check_fields_sizes(rec, table, offsets)) return FALSE;
	if (debug) printf("FIELD_SIZES=OK ");
	
	// This record could be valid and useful for us
	return TRUE;
}

/*******************************************************************/
ibool check_page_format(page_t *page) {
	if (process_redundant && page_is_comp(page)) {
		if (debug) printf("Page is in COMPACT format while we're looking for REDUNDANT - skipping\n");
		return FALSE;
	}

	if (process_compact && !page_is_comp(page)) {
		if (debug) printf("Page is in REDUNDANT format while we're looking for COMPACT - skipping\n");
		return FALSE;
	}
	
    return TRUE;
}

/*******************************************************************/
int check_page(page_t *page){
    int comp = page_is_comp(page);
    int16_t i, s, p, b, p_prev;
    int recs = 0;
    int max_recs = UNIV_PAGE_SIZE / 5;
    i = (comp) ? PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM;
    s = (comp) ? PAGE_NEW_SUPREMUM : PAGE_OLD_SUPREMUM;
    if (debug) printf("Checking a page\nInfimum offset: 0x%X\nSupremum offset: 0x%X\n", i, s);
    p_prev = 0;
    p = i;
    while(p != s){
        if(recs > max_recs){
            if (debug) printf("Page is bad\n");
            return 0;
            }
        // If a pointer to the next record is negative - the page is bad
        if(p < 2){
            if (debug) printf("Page is bad\n");
            return 0;
            }
	// If the pointer is bigger than UNIV_PAGE_SIZE, the page is corrupted
        if(p > UNIV_PAGE_SIZE){
            if (debug) printf("Page is bad\n");
            return 0;
            }
	//  If we've already was here, the page is bad
        if(p == p_prev){
            if (debug) printf("Page is bad\n");
            return 0;
            }
	p_prev = p;
        // Get next pointer
        if(comp){
            b = mach_read_from_2(page + p - 2);
            p = p + b;
            }
        else{
            p =  mach_read_from_2(page + p - 2);
            }
        if (debug) printf("Next record at offset: 0x%X (%d) \n", 0x0000FFFF & p, p);
        recs++;
        }
    if (debug) printf("Page is good\n");
    return 1;
    }
/*******************************************************************/
void process_ibpage(page_t *page) {
    ulint page_id;
	rec_t *origin;
	ulint offsets[MAX_TABLE_FIELDS + 2];
	ulint offset, i;
    int is_page_valid = 0;
    int comp;
    int16_t b, infimum, supremum;
	
	// Skip tables if filter used
    if (use_filter_id) {
        dulint index_id = mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID);
        if (index_id.low != filter_id.low || index_id.high != filter_id.high) {
            if (debug) {
            	page_id = mach_read_from_4(page + FIL_PAGE_OFFSET);
                printf("Skipped using index id filter: %lu!\n", page_id);
            }
            return;
        }
    }

	// Read page id
	page_id = mach_read_from_4(page + FIL_PAGE_OFFSET);
	if (debug) printf("Page id: %lu\n", page_id);

	// Check requested and actual formats
    if (!check_page_format(page)) return;
    is_page_valid = check_page(page);
    
    // comp == 1 if page in COMPACT format and 0 if REDNDANT
    comp = page_is_comp(page);
    infimum = (comp) ? PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM;
    supremum = (comp) ? PAGE_NEW_SUPREMUM : PAGE_OLD_SUPREMUM;
	// Find possible data area start point (at least 5 bytes of utility data)
	if(is_page_valid){
        b = mach_read_from_2(page + infimum - 2);
        offset = (comp) ? infimum + b : b;
        }
    else{
        offset = 100 + record_extra_bytes;
        }
	if (debug) printf("Starting offset: %lu (%lX). Checking %d table definitions.\n", offset, offset, table_definitions_cnt);
	
	// Walk through all possible positions to the end of page 
	// (start of directory - extra bytes of the last rec)
    //is_page_valid = 0;
	while (offset < UNIV_PAGE_SIZE - record_extra_bytes && ( (offset != supremum ) || !is_page_valid) ) {
		// Get record pointer
		origin = page + offset;
		if (debug) printf("\nChecking offset: 0x%lX: ", offset);
		
		// Check all tables
		for (i = 0; i < table_definitions_cnt; i++) {
			// Get table info
			table_def_t *table = &(table_definitions[i]);
			if (debug) printf(" (%s) ", table->name);

			// Check if origin points to a valid record
			if (check_for_a_record(page, origin, table, offsets) && check_constraints(origin, table, offsets)) {
				if (debug) printf("\n---------------------------------------------------\n"
			       			  "PAGE%lu: Found a table %s record: %p (offset = %lu)\n", \
						  page_id, table->name, origin, offset);
                		if(is_page_valid){
					process_ibrec(page, origin, table, offsets);
                    			b = mach_read_from_2(page + offset - 2);
					offset = (comp) ? offset + b : b;
                    			}
				else{
					offset += process_ibrec(page, origin, table, offsets);
                    			}
                		if (debug) printf("Next offset: 0x%lX", offset);
			   		break;
		        	}
            		else{
                		if(is_page_valid){
					b = mach_read_from_2(page + offset - 2);
					offset = (comp) ? offset + b : b;
                    			}
				else{
					offset++;
					}
                		if (debug) printf("\nNext offset: %lX", offset);
               			}
			}
		}
}

/*******************************************************************/
void process_ibfile(int fn) {
	int read_bytes;
	page_t *page = malloc(UNIV_PAGE_SIZE);
    struct stat st;
    off_t pos;
    ulint free_offset;
    	
	if (!page) error("Can't allocate page buffer!");

	// Initialize table definitions (count nullable fields, data sizes, etc)
	init_table_defs();

	if (debug) printf("Read data from fn=%d...\n", fn);

    // Get file info
    fstat(fn, &st);

	// Read pages to the end of file
	while ((read_bytes = read(fn, page, UNIV_PAGE_SIZE)) == UNIV_PAGE_SIZE) {
        pos = lseek(fn, 0, SEEK_CUR);
        
        if (pos % (UNIV_PAGE_SIZE * 10) == 0) {
            fprintf(stderr, "%.2f%% done\n", 100.0 * pos / st.st_size);
        }

	    if (deleted_pages_only) {
    		free_offset = page_header_get_field(page, PAGE_FREE);
    		if (page_header_get_field(page, PAGE_N_RECS) == 0 && free_offset == 0) continue;
    		if (free_offset > 0 && page_header_get_field(page, PAGE_GARBAGE) == 0) continue;
    		if (free_offset > UNIV_PAGE_SIZE) continue;
    	}
        
        process_ibpage(page);
	}
}

/*******************************************************************/
int open_ibfile(char *fname) {
	struct stat fstat;
	int fn;

	// Skip non-regular files
	if (debug) printf("Opening file: %s\n", fname);
	if (stat(fname, &fstat) != 0 || (fstat.st_mode & S_IFREG) != S_IFREG) error("Invalid file specified!");
	fn = open(fname, O_RDONLY, 0);
	if (!fn) error("Can't open file!");
	
	return fn;
}

/*******************************************************************/
void set_filter_id(char *id) {
    int cnt = sscanf(id, "%lu:%lu", &filter_id.high, &filter_id.low);
    if (cnt < 2) {
        error("Invalid index id provided! It should be in N:M format, where N and M are unsigned integers");
    }
    use_filter_id = 1;
}

/*******************************************************************/
void usage() {
	error(
	  "Usage: ./constraints_parser -4|-5 [-dDV] -f <InnoDB page or dir> [-T N:M] [-b <extrenal pages directory>]\n"
	  "  Where\n"
	  "    -f <InnoDB page(s)> -- InnoDB page or directory with pages\n"
	  "    -h  -- Print this help\n"
	  "    -d  -- Process only those pages which potentially could have deleted records (default = NO)\n"
	  "    -D  -- Recover deleted rows only (default = NO)\n"
	  "    -U  -- Recover UNdeleted rows only (default = NO)\n"
	  "    -V  -- Verbode mode (lots of debug information)\n"
	  "    -4  -- innodb_datafile is in REDUNDANT format\n"
	  "    -5  -- innodb_datafile is in COMPACT format\n"
	  "    -T  -- retrieves only pages with index id = NM (N - high word, M - low word of id)\n"
	  "    -b <dir> -- Directory where external pages can be found. Usually it is pages-XXX/FIL_PAGE_TYPE_BLOB/\n"
	  "\n"
	);
}

/*******************************************************************/
int main(int argc, char **argv) {
	int fn = 0, ch;
	int is_dir = 0;
	struct stat st;
	char src[256];

	setbuf(stdout, NULL);

	while ((ch = getopt(argc, argv, "45hdDUVf:T:b:p:")) != -1) {
		switch (ch) {
			case 'd':
				deleted_pages_only = 1;
				break;
			case 'D':
			    deleted_records_only = 1;
				break;
			case 'U':
			    undeleted_records_only = 1;
				break;
			case 'f':
				strncpy(src, optarg, sizeof(src));
				if(stat(src, &st) == 0){
					if(S_ISDIR(st.st_mode)){
						is_dir = 1;
						}
					}
				else{
					perror("stat");
					fprintf(stderr, "Can't stat %s\n", src);
					exit(-1);
					}
				break;
			case 'V':
				debug = 1;
				break;
            		case '4':
                		process_redundant = 1;
                		break;
            		case '5':
                		process_compact = 1;
             	 		break;
            		case 'T':
                		set_filter_id(optarg);
                		break;
            		case 'b':
                		strncpy(blob_dir, optarg, sizeof(blob_dir));
                		break;
			case 'p':
				strncpy(dump_prefix, optarg, sizeof(dump_prefix));
				break;
			default:
			case '?':
			case 'h':
				usage();
		}
	}

	if(is_dir){
		DIR *src_dir;
		char src_file[256]; src_file[0] = 0;
		struct dirent *de;
		src_dir = opendir(src);
		while(NULL != (de = readdir(src_dir))){
			src_file[0] = 0;
			if(!strncmp(de->d_name, ".", sizeof(de->d_name))) continue;
			if(!strncmp(de->d_name, "..", sizeof(de->d_name))) continue;
			strncat(src_file, src, sizeof(src_file));
			strncat(src_file, "/", sizeof(src_file));
			strncat(src_file, de->d_name, sizeof(src_file));
			if(debug) { fprintf(stderr, "Processing %s\n", src_file); }
			if(0 == (fn = open_ibfile(src_file))){
				fprintf(stderr, "Can't open %s\n", src_file);
				perror("open_ibfile");
				exit(-1);
				}
			process_ibfile(fn);
			close(fn);
			}
		closedir(src_dir);
		}
	else{
		if(0 == (fn = open_ibfile(src))){
			fprintf(stderr, "Can't open %s\n", src);
			perror("open_ibfile");
			exit(-1);
			}
		process_ibfile(fn);
		close(fn);
		}
	table_def_t *table = &(table_definitions[0]);
	fprintf(stderr, "LOAD DATA INFILE '%s/dumps/%s/%s' REPLACE INTO TABLE `%s` FIELDS TERMINATED BY '\\t' OPTIONALLY ENCLOSED BY '\"' LINES STARTING BY '%s\\t' ", getenv("PWD"), dump_prefix, table->name, table->name, table->name);
	int i = 0;
	int comma = 0;
	int has_set = 0;
	fprintf(stderr, "(");
	for(i = 0; i < table->fields_count; i++) {
		if (table->fields[i].type == FT_INTERNAL) continue;
		if(comma) fprintf(stderr, ", ");
		if(table->fields[i].type == FT_BLOB){
			fprintf(stderr, "@var_%s", table->fields[i].name);
			has_set = 1;
			}
		else{
			fprintf(stderr, "%s", table->fields[i].name);
		}
		comma = 1;
		}
	fprintf(stderr, ")");
	comma = 0;
	if(has_set){
		fprintf(stderr, "\nSET\n");
		for(i = 0; i < table->fields_count; i++) {
			if (table->fields[i].type == FT_INTERNAL) continue;
			if(table->fields[i].type == FT_BLOB){
				if(comma) fprintf(stderr, ",\n");
				fprintf(stderr, "    %s = UNHEX(@var_%s)", table->fields[i].name, table->fields[i].name);
				comma = 1;
				}
			}
		}
	fprintf(stderr, ";\n");
	if (!process_compact && !process_redundant) {
        printf("Error: Please, specify what format your datafile in. Use -4 for mysql 4.1 and below and -5 for 5.X+\n");
        usage();
	}
	
	return 0;
}
