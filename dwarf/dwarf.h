#ifndef _DWARF_H_
#define _DWARF_H_

#include <stddef.h>

/****************************
 * General Dwarf structures *
 ****************************/
typedef struct {
    uint8_t *start;
    size_t   size;
} Debug_Line;

typedef struct {
    uint8_t *start;
    size_t   size;
} Debug_Line_Str;

/*****************************
 * Specific For Line Section *
 *****************************/
typedef struct {
    uint32_t unit_length;
    uint16_t version;
    uint8_t address_size;
    uint8_t segment_selector_size;
    uint32_t prologue_length;
    uint8_t minimum_instruction_length;
    uint8_t maximum_operations_per_instruction;
    uint8_t default_is_stmt;
    int8_t line_base;
    uint8_t line_range;
    uint8_t opcode_base;
} Debug_Line_Header;

typedef struct {
    uint64_t content_type;
    uint64_t form;
} Entry_Format;

#define MAX_ENTRY_FORMATS 8
typedef struct {
    // FIXME: should be dynamically allocated
    Entry_Format formats[MAX_ENTRY_FORMATS];
    uint32_t path_offsets[MAX_ENTRY_FORMATS];
    const char *paths[MAX_ENTRY_FORMATS];
    uint64_t directory_indices[MAX_ENTRY_FORMATS];
    uint8_t format_count;
    uint64_t count;

    ptrdiff_t offset; // for printing
    /* "Directory Table" / "File Name Table" */
    const char *label;
} Dwarf_Table;

typedef struct {
    Dwarf_Table dirs;
    Dwarf_Table files;
} Dwarf_Line_File_Info;

#define MAX_LINE_ROWS 128
typedef struct {
    uint64_t address;
    unsigned int op_index;
    unsigned int file;
    unsigned int line;
    unsigned int column;
    unsigned int isa; /// MAYBE?

    bool is_stmt;
    bool end_sequence;
} Dwarf_Line_Row;

typedef struct {
    Dwarf_Line_Row rows[MAX_LINE_ROWS];
    size_t count;
} Dwarf_Line_Rows;

/* ---------------------- */
/* MAIN CONTEXT STRUCTURE */
/* ---------------------- */
typedef struct {
    Debug_Line debug_line;
    Debug_Line_Str debug_line_str;

    /* Specific for .debug_line */
    Debug_Line_Header header;
    Dwarf_Line_Rows line_rows;
    Dwarf_Line_File_Info file_info;

    const char *path;
} Dwarf_Context;

/* PUBLIC INTERFACE FUNCTIONS */
bool dwarf_line_to_address(const Dwarf_Context *ctx, unsigned int line, uint64_t *address);
bool dwarf_address_to_line(const Dwarf_Context *ctx, uint64_t address, unsigned int *line);
void dwarf_init_context(Dwarf_Context *ctx);

#endif // _DWARF_H_
