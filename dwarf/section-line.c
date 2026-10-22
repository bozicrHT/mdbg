#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>

#include <elf.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include "dwarf_codes.h"
#include "dwarf.h"

/*****************************
 * Specific For Line Section *
 *****************************/
// XXX: This should be in the "dwarf.h" file, maybe?
typedef struct {
    uint8_t count;
    // FIXME: should be dynamically allocated
    uint64_t std_opcode_lengths[40];
} Standard_Opcodes_Lengths;

/* -------------------------------------- */
/* Structures For State Machine Execution */
/* -------------------------------------- */
typedef struct {
    uint64_t address;
    unsigned int op_index;
    unsigned int file;
    unsigned int line;
    unsigned int column;
    bool is_stmt;
    bool basic_block;
    bool end_sequence;
    bool prologue_end;
    bool epilogue_begin;
    unsigned int isa;
    unsigned int discriminator;
} Line_State_Machine;

/// We want to separate printing logic from parsing logic.
/// We can do that by introducing new abstraction:
/// `Line_Instruction` where we will save decoded
/// instruction. This way we can centralize printing logic,
/// we print just this struct. THIS ISN'T NECESSARY
/// for parsing, just for debugging purposes.
typedef enum {
    LINE_OP_STANDARD,
    LINE_OP_EXTENDED,
    LINE_OP_SPECIAL,
} Line_Opcode_Type;


typedef struct {
    Line_Opcode_Type op_type;

    uint8_t opcode;
    uint8_t adjusted;

    uint64_t address_increment;
    int line_increment;
    uint64_t address;
    uint64_t line;

    bool is_stmt;
    bool emit;

    /* Everything into one */
    /* column, file, op_advance, advance */
    uint64_t operand;
} Line_Instruction;

/* --------------- */
/* Parsing Helpers */
/* --------------- */
static const uint8_t *parse_debug_line_header(Debug_Line_Header *header, const uint8_t **cursor);
static void parse_table(Dwarf_Context *ctx, Dwarf_Table *table, const uint8_t **cursor, const char *label);
static void parse_tables(Dwarf_Context *ctx, Dwarf_Line_File_Info *tables, const uint8_t **cursor);
static void parse_standard_opcode_lengths(Standard_Opcodes_Lengths *result, Debug_Line_Header *header, const uint8_t **ptr);
static bool has_content_type(const Dwarf_Table *table, uint64_t content_type);

static void execute_line_program(Dwarf_Context *ctx, const uint8_t **cursor, const uint8_t *end);
static void reset_line_state(Line_State_Machine *state, const Debug_Line_Header *header);
static Line_Instruction execute_extended_opcodes(Dwarf_Context *ctx, const uint8_t **cursor, const uint8_t *end, Line_State_Machine *lstate);
static Line_Instruction execute_standard_opcodes(Dwarf_Context *ctx, const uint8_t **cursor, Line_State_Machine *lstate, uint8_t opcode);
static Line_Instruction execute_special_opcodes(Dwarf_Context *ctx, Line_State_Machine *lstate, const uint8_t opcode);
static uint64_t advance_address(Debug_Line_Header *header, Line_State_Machine *lstate, uint64_t op_advance);
static void emit_line_row(Dwarf_Line_Rows *table, const Line_State_Machine *lstate);

/**********************
 * Printing Functions *
 **********************/
static void print_table(const Dwarf_Table *table);
static void print_standard_opcode_lengths(Standard_Opcodes_Lengths *lengths);
static void print_line_table(const Dwarf_Line_Rows *table, const Dwarf_Line_File_Info *file_info);
static void print_line_row(const Dwarf_Line_Row *row, const Dwarf_Line_File_Info *file_info);

/*********************
 * Parsing Functions *
 *********************/
void dwarf_parse_line_section(Dwarf_Context *ctx);

/* ------------- */
/* API Functions */
/* ------------- */

/* `readelf --debug-dump=decodedline a.out` */
bool dwarf_line_to_address(const Dwarf_Context *ctx, unsigned int line, uint64_t *address)
{
    const Dwarf_Line_Rows *table = &ctx->line_rows;

    for (size_t i = 0; i < table->count; i++) {
        const Dwarf_Line_Row *row = &table->rows[i];

        if (row->end_sequence)
            continue;

        if (row->is_stmt && row->line >= line) {
            *address = row->address;
            return true;
        }
    }

    return false;
}

bool dwarf_address_to_line(const Dwarf_Context *ctx, uint64_t address, unsigned int *line)
{
    const Dwarf_Line_Rows *table = &ctx->line_rows;
    const Dwarf_Line_Row  *best  = NULL;

    for (size_t i = 0; i < table->count; i++) {
        const Dwarf_Line_Row *row = &table->rows[i];

        if (row->end_sequence)
            continue;

        if (row->address > address)
            break;

        if (row->is_stmt)
            best = row;
    }

    if (!best)
        return false;

    *line = best->line;
    return true;
}

/*********
 * UTILS *
 *********/
static uint64_t read_uleb128(const uint8_t **in);
static int64_t read_sleb128(const uint8_t **in);
static uint8_t parse_ubyte(const uint8_t **ptr);
static int8_t parse_sbyte(const uint8_t **ptr);
static uint64_t parse_u64(const uint8_t **ptr);
static uint32_t parse_u32(const uint8_t **ptr);
static uint16_t parse_u16(const uint8_t **ptr);

static void die(const char *fmt, ...);
static void read_eheader(Elf64_Ehdr *ehdr, int fd);
static uint8_t *map_file(int fd);

//const char *path = "../test";

    /* Context init */
//   Dwarf_Context ctx = {0};
//   ctx.path = path;

void dwarf_init_context(Dwarf_Context *ctx)
{
    int fd;
    if ((fd = open(ctx->path, O_RDONLY)) < 0)
        die("Error: open failed");

    /* ELF header */
    Elf64_Ehdr ehdr;
    read_eheader(&ehdr, fd);
    /* Check if it's ELF -- skipped... */
    uint8_t *ptr = map_file(fd);

    /* Section header (Shdr) */
    Elf64_Shdr *shdr = (Elf64_Shdr *)(ptr + ehdr.e_shoff);
    uint8_t *strtab = ptr + shdr[ehdr.e_shstrndx].sh_offset;

    for (int i = 0; i < ehdr.e_shnum; i++) {
        const char *name = (char *)strtab + shdr[i].sh_name;
        if (!strcmp(name, ".debug_line")) {
            ctx->debug_line.start = ptr + shdr[i].sh_offset;
            ctx->debug_line.size  = shdr[i].sh_size;
        } else if (!strcmp(name, ".debug_line_str")) {
            ctx->debug_line_str.start = ptr + shdr[i].sh_offset;
            ctx->debug_line_str.size  = shdr[i].sh_size;
        }
    }

    dwarf_parse_line_section(ctx);
    //print_line_table(&ctx.line_rows, &ctx.file_info);
}


void dwarf_parse_line_section(Dwarf_Context *ctx)
{
    Debug_Line *dline = &ctx->debug_line;
    const uint8_t *cursor = dline->start;

    /* ----------------------- */
    /* Parse Debug Line Header */
    /* ----------------------- */
    Debug_Line_Header *header = &ctx->header;
    const uint8_t *end = parse_debug_line_header(header, &cursor);

    /* ------------- */
    /* Parse Opcodes */
    /* ------------- */
    Standard_Opcodes_Lengths opcode_lengths = {0};
    parse_standard_opcode_lengths(&opcode_lengths, header, &cursor);
    //print_standard_opcode_lengths(&opcode_lengths);

    /* ------------------------------------ */
    /* Parse Directory and File Name Tables */
    /* ------------------------------------ */
    parse_tables(ctx, &ctx->file_info, &cursor);

    /* --------------------- */
    /* Execute State Machine */
    /* --------------------- */
    execute_line_program(ctx, &cursor, end);
}

static
void execute_line_program(Dwarf_Context *ctx, const uint8_t **cursor, const uint8_t *end)
{
    Debug_Line        *dline  = &ctx->debug_line;
    Debug_Line_Header *header = &ctx->header;

    Line_State_Machine lstate = {0};
    reset_line_state(&lstate, header);

    __attribute__((unused))
    Line_Instruction ins;

    while (*cursor < end) {

        __attribute__((unused))
        ptrdiff_t offset = *cursor - dline->start;
        uint8_t opcode = parse_ubyte(cursor);

        if (opcode == 0)
            ins = execute_extended_opcodes(ctx, cursor, end, &lstate);
        else if (opcode < header->opcode_base)
            ins = execute_standard_opcodes(ctx, cursor, &lstate, opcode);
        else
            ins = execute_special_opcodes(ctx, &lstate, opcode);

        // TODO: print_decoded_instruction(&ins, offset);
    }
}

static
void emit_line_row(Dwarf_Line_Rows *table, const Line_State_Machine *lstate)
{
    if (table->count >= MAX_LINE_ROWS)
        die("Error: more than %d line rows in Line Table", MAX_LINE_ROWS);

    Dwarf_Line_Row *row = &table->rows[table->count];
    table->count++;

    row->address = lstate->address;
    row->op_index = lstate->op_index;
    row->file = lstate->file;
    row->line = lstate->line;
    row->column = lstate->column;
    row->isa = lstate->isa;
    row->is_stmt = lstate->is_stmt;
    row->end_sequence = lstate->end_sequence;
}

static
Line_Instruction execute_special_opcodes(Dwarf_Context *ctx, Line_State_Machine *lstate, const uint8_t opcode)
{
    Debug_Line_Header *header = &ctx->header;
    Dwarf_Line_Rows *rtable = &ctx->line_rows;

    Line_Instruction ins = {
        .opcode = opcode,
        .op_type = LINE_OP_SPECIAL,
    };

    uint8_t adjusted = opcode - header->opcode_base;
    uint64_t operation_advance = adjusted / header->line_range;

    int line_increment = header->line_base +
        (adjusted % header->line_range);
    lstate->line += line_increment;

    uint64_t address_increment =
        advance_address(header, lstate, operation_advance);

    ins.address_increment = address_increment;
    ins.line_increment = line_increment;
    ins.line = lstate->line;
    ins.address = lstate->address;
    ins.adjusted = adjusted;

    /*
     * According to DWARF 5 6.2.5.3, a special
     * opcode advances the address and line
     * registers and then appends a row to the matrix.
     */
    /* A special opcode always emits a row. */
    emit_line_row(rtable, lstate);

    return ins;
}

static
Line_Instruction execute_standard_opcodes(Dwarf_Context *ctx, const uint8_t **cursor, Line_State_Machine *lstate, uint8_t opcode)
{
    Debug_Line_Header *header = &ctx->header;
    Dwarf_Line_Rows *rtable = &ctx->line_rows;

    Line_Instruction ins = {
        .op_type = LINE_OP_STANDARD,
        .opcode = opcode
    };

    switch (opcode) {
        case DW_LNS_set_column: {
            uint64_t col = read_uleb128(cursor);
            lstate->column = col;

            ins.operand = col;
        } break;
        case DW_LNS_set_file: {
            uint64_t file = read_uleb128(cursor);
            lstate->file = file;

            ins.operand = file;
        } break;
        case DW_LNS_const_add_pc: {
             /*
             * It says to advance the address and op_index registers by
             * the increments corresponding to special opcode 255. See
             * also 6.2.5.1, which defines how these registers are
             * updated. */
            uint8_t adjusted = 255 - header->opcode_base;
            uint32_t op_advance = adjusted / header->line_range;

            ins.address_increment =
                advance_address(header, lstate, op_advance);

            ins.address = lstate->address;
            ins.operand = op_advance;
        } break;
        case DW_LNS_advance_pc: {
            /* 6.2.5.2 tells you what DW_LNS_advance_pc does, while
             * 6.2.5.1 tells you how to implement the state updates
             * correctly. */
            uint64_t advance = read_uleb128(cursor);
            uint64_t total_ops = lstate->op_index + advance;

            lstate->address +=
                (total_ops / header->maximum_operations_per_instruction) *
                header->minimum_instruction_length;

            lstate->op_index =
                total_ops % header->maximum_operations_per_instruction;

            ins.address = lstate->address;
            ins.operand = advance;
        } break;
        case DW_LNS_fixed_advance_pc: {
            uint16_t advance = parse_u16(cursor);
            lstate->address += advance;
            lstate->op_index = 0;

            ins.address = lstate->address;
            ins.operand = advance;
        } break;
        case DW_LNS_set_prologue_end: {
            lstate->prologue_end = true;

        } break;
        case DW_LNS_negate_stmt: {
            lstate->is_stmt = !lstate->is_stmt;

            ins.is_stmt = lstate->is_stmt;
        } break;
        case DW_LNS_set_epilogue_begin: {
            lstate->epilogue_begin = true;
        } break;
        case DW_LNS_copy: {
            emit_line_row(rtable, lstate);

            lstate->discriminator = 0;
            lstate->basic_block = false;
            lstate->prologue_end = false;
            lstate->epilogue_begin = false;
        } break;
        case DW_LNS_advance_line:
            lstate->line += read_sleb128(cursor);
            break;

        case DW_LNS_set_basic_block:
        case DW_LNS_set_isa:
            die("Unsupported standard DW_LNS_* (%#x)", opcode);
    }

    return ins;
}

static
Line_Instruction execute_extended_opcodes(Dwarf_Context *ctx, const uint8_t **cursor, const uint8_t *end, Line_State_Machine *lstate)
{
    Debug_Line_Header *header = &ctx->header;
    Dwarf_Line_Rows *rtable = &ctx->line_rows;

    uint64_t length = read_uleb128(cursor);
    if (*cursor > end || length > (uint64_t)(end - *cursor))
        die("Extended opcode exceeds .debug_line bounds");

    if (length == 0)
        die("Invalid extended opcode length");
    const uint8_t *extended_end = *cursor + length;
    uint8_t extended_opcode = parse_ubyte(cursor);

    Line_Instruction ins = {
        .op_type = LINE_OP_EXTENDED,
        .opcode = extended_opcode,
    };

    switch (extended_opcode) {
        case DW_LNE_set_address: {
            /* NOTE: Assume the address is 8 bytes */
            uint64_t address = parse_u64(cursor);
            lstate->address = address;
            lstate->op_index = 0;

            ins.address = address;
        } break;
        case DW_LNE_set_discriminator: {
            uint64_t discr = read_uleb128(cursor);
            lstate->discriminator = discr;
        } break;
        case DW_LNE_end_sequence: {
            lstate->end_sequence = true;

            emit_line_row(rtable, lstate);

            /* Reset the line number state machine for the next sequence. */
            reset_line_state(lstate, header);
        } break;
        case DW_LNE_lo_user:
        case DW_LNE_hi_user:
            die("Unsupported extended DW_LNS_* (%#x)", extended_opcode);
    }
    /* Skip any remaining bytes in this extended instruction. */
    *cursor = extended_end;

    return ins;
}

static
uint64_t advance_address(Debug_Line_Header *header, Line_State_Machine *lstate, uint64_t op_advance)
{
    uint64_t total_ops = lstate->op_index + op_advance;

    uint64_t address_increment =
        (total_ops / header->maximum_operations_per_instruction) *
        header->minimum_instruction_length;

    lstate->op_index =
        total_ops % header->maximum_operations_per_instruction;

    lstate->address += address_increment;

    return address_increment;
}

static
void reset_line_state(Line_State_Machine *state, const Debug_Line_Header *header)
{
    state->address = 0;
    state->op_index = 0;
    state->file = 1;
    state->line = 1;
    state->column = 0;
    state->is_stmt = header->default_is_stmt;
    state->basic_block = false;
    state->end_sequence = false;
    state->prologue_end = false;
    state->epilogue_begin = false;
    state->isa = 0;
    state->discriminator = 0;
}

static
void parse_tables(Dwarf_Context *ctx, Dwarf_Line_File_Info *tables, const uint8_t **cursor)
{
    parse_table(ctx, &tables->dirs, cursor, "Directory Table");
    parse_table(ctx, &tables->files, cursor, "File Name Table");

    //print_table(&tables->dirs);
    //print_table(&tables->files);
}

static
void parse_table(Dwarf_Context *ctx, Dwarf_Table *table, const uint8_t **cursor, const char *label)
{
    *table = (Dwarf_Table) {
        .label = label
    };

    table->format_count = parse_ubyte(cursor);
    if (table->format_count > MAX_ENTRY_FORMATS)
        die("Error: Too many entry formats");

    for (int i = 0; i < table->format_count; i++) {
        table->formats[i].content_type = read_uleb128(cursor);
        table->formats[i].form = read_uleb128(cursor);
    }

    table->count = read_uleb128(cursor);
    table->offset = *cursor - ctx->debug_line.start;

    if (table->count >= MAX_ENTRY_FORMATS)
        die("Error: Table %s exceeds %d entry rows", label, MAX_ENTRY_FORMATS);

    for (uint64_t i = 0; i < table->count; i++) {
        for (uint8_t j = 0; j < table->format_count; j++) {
            Entry_Format *format = &table->formats[j];

            switch (format->content_type) {
                case DW_LNCT_path: {
                    if (format->form != DW_FORM_line_strp)
                        die("bad path form");

                    uint32_t offset = parse_u32(cursor);

                    if (offset >= ctx->debug_line_str.size)
                        die("Invalid .debug_line_str offset: %#x", offset);

                    const char *path = (const char *)ctx->debug_line_str.start + offset;

                    // Ensure NULL terminator exists within the section
                    size_t remaining = ctx->debug_line_str.size - offset;
                    if (!memchr(path, '\0', remaining))
                        die("Unterminated string in .debug_line_str");

                    table->path_offsets[i] = offset;
                    table->paths[i] = (char *)path;
                } break;

                case DW_LNCT_directory_index:
                    table->directory_indices[i] = read_uleb128(cursor);
                    break;

                case DW_LNCT_MD5:
                    // 16-byte MD5
                    (void)parse_u64(cursor);
                    (void)parse_u64(cursor);
                    break;

                default:
                    die("Unsupported '%s' in parsing lines",
                            dwarf_content_type_name(format->content_type));
            }
        }
    }
}

static
bool has_content_type(const Dwarf_Table *table, uint64_t content_type)
{
    for (uint8_t i = 0; i < table->format_count; i++)
        if (table->formats[i].content_type == content_type)
            return true;

    return false;
}

static const
uint8_t *parse_debug_line_header(Debug_Line_Header *header, const uint8_t **cursor)
{
    header->unit_length = parse_u32(cursor);
    const uint8_t *end = *cursor + header->unit_length;

    header->version = parse_u16(cursor);
    header->address_size = parse_ubyte(cursor);
    header->segment_selector_size = parse_ubyte(cursor);
    header->prologue_length = parse_u32(cursor);
    header->minimum_instruction_length = parse_ubyte(cursor);
    header->maximum_operations_per_instruction = parse_ubyte(cursor);
    header->default_is_stmt = parse_ubyte(cursor);
    header->line_base   = parse_sbyte(cursor);
    header->line_range  = parse_ubyte(cursor);
    header->opcode_base = parse_ubyte(cursor);

    return end;
}

static
void die(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
    exit(1);
}

static
void read_eheader(Elf64_Ehdr *ehdr, int fd)
{
    if (read(fd, ehdr, sizeof(*ehdr)) != sizeof(*ehdr)) {
        close(fd);
        die("Error: read failed");
    }
}

static uint8_t *map_file(int fd)
{
    struct stat st;
    if (fstat(fd, &st))
        die("Error: fstat failed");

    uint8_t *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return map;
}

static
uint64_t read_uleb128(const uint8_t **in)
{
    uint64_t value = 0;
    int shift = 0;
    uint8_t byte;

    do {
        byte = *(*in)++;
        value |= ((uint64_t)(byte & 0x7F)) << shift;
        shift += 7;
    } while ((byte & 0x80) != 0);

    return value;
}

static
int64_t read_sleb128(const uint8_t **in)
{
    const uint8_t *ptr = *in;
    int64_t result = 0;
    int shift = 0;
    uint8_t byte;

    while (1) {
        byte = *ptr++;

        result |= ((int64_t)(byte & 0x7F)) << shift;
        shift += 7;

        if (!(byte & 0x80))
            break;
    }

    *in = ptr;

    // sign bit of final byte
    if (shift < 64 && (byte & 0x40)) {
        result |= -((int64_t)1 << shift);
    }

    return result;
}

static
uint8_t parse_ubyte(const uint8_t **ptr)
{
    uint8_t res = *(uint8_t *)(*ptr);
    *ptr += 1;
    return res;
}

static
int8_t parse_sbyte(const uint8_t **ptr)
{
    int8_t res = *(int8_t *)(*ptr);
    *ptr += 1;
    return res;
}

static
uint64_t parse_u64(const uint8_t **ptr)
{
    uint64_t res = *(uint64_t *)(*ptr);
    *ptr += 8;
    return res;
}

static
uint32_t parse_u32(const uint8_t **ptr)
{
    uint32_t res = *(uint32_t *)(*ptr);
    *ptr += 4;
    return res;
}

static
uint16_t parse_u16(const uint8_t **ptr)
{
    uint16_t res = *(uint16_t *)(*ptr);
    *ptr += 2;
    return res;
}

static __attribute__((unused))
void print_table(const Dwarf_Table *table)
{
     printf(" The %s (offset %#lx, lines %lu, columns %d):\n",
             table->label, table->offset, table->count, table->format_count);

     /* Header */
     printf("  Entry\t");

     if (has_content_type(table, DW_LNCT_directory_index))
         printf("Dir\t");

     if (has_content_type(table, DW_LNCT_MD5))
         printf("MD5\t");

     if (has_content_type(table, DW_LNCT_path))
         printf("Name\t");

     putchar('\n');

     /* Rows */
     for (uint64_t i = 0; i < table->count; i++) {
         printf("  %-5lu ", i);

         if (has_content_type(table, DW_LNCT_directory_index))
             printf("%lu\t", table->directory_indices[i]);

         if (has_content_type(table, DW_LNCT_MD5))
             printf("<md5>\t");

         if (has_content_type(table, DW_LNCT_path))
             printf("(indirect line string, offset: 0x%x): %s\t", table->path_offsets[i], table->paths[i]);

         putchar('\n');
     }
     putchar('\n');
}

static
void parse_standard_opcode_lengths(Standard_Opcodes_Lengths *result, Debug_Line_Header *header, const uint8_t **ptr)
{
    result->count = header->opcode_base - 1;

    for (uint8_t i = 0; i < result->count; i++)
        result->std_opcode_lengths[i] = read_uleb128(ptr);
}

static __attribute__((unused))
void print_standard_opcode_lengths(Standard_Opcodes_Lengths *lengths)
{
    printf(" Opcodes:\n");
    for (int i = 0; i < lengths->count; i++) {
        uint64_t standard_opcode_lengths = lengths->std_opcode_lengths[i];
        printf("  Opcode %d has %ld %s\n", i+1,
                standard_opcode_lengths,
                standard_opcode_lengths == 1u ? "arg" : "args");
    }
    putchar('\n');
}

static __attribute__((unused))
void print_line_table(const Dwarf_Line_Rows *table, const Dwarf_Line_File_Info *file_info)
    /* https://dwarfstd.org/issues/210713.1.html */
{
    const Dwarf_Table *files = &file_info->files;
    const Dwarf_Table *dirs  = &file_info->dirs;

    printf("%-32s %-13s %-20s %s\n\n",
            "File name", "Line number", "Starting address", "Stmt");

    for (uint64_t i = 0; i < files->count; i++) {
        uint64_t dir = files->directory_indices[i];
        if (dir < dirs->count)
            printf("%s/%s\n", dirs->paths[dir], files->paths[i]);
    }

    for (size_t i = 0; i < table->count; i++)
        print_line_row(&table->rows[i], file_info);

}

static
void print_line_row(const Dwarf_Line_Row *row, const Dwarf_Line_File_Info *file_info)
{
    const Dwarf_Table *files = &file_info->files;

    unsigned int file = row->file;

    if (file >= files->count)
        printf("%-37s", "<unknown>");
    else
        printf("%-37s", files->paths[file]);

    if (row->end_sequence)
        printf("%-13s", "-");
    else
        printf("%-13d", row->line);
    printf("%#-20lx", row->address);
    printf("%s", row->is_stmt ? "x" : "");
    putchar('\n');

}
