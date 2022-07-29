#include <stddef.h>
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 2

/* *** DATA TYPES *** */

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_open_comment;
} erow;

struct editor_config {
    int cx, cy;
    int rx;
    int row_off;
    int col_off;
    int screen_rows;
    int screen_cols;
    int num_rows;
    erow *row;
    int dirty;
    char *filename;
    char status_msg[80];
    time_t status_msg_time;
    struct editor_syntax *syntax;
    struct termios original_termios;
};

struct editor_config econf;

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT { NULL , 0 }

enum editor_key {
    BACKSPACE = 127,
    ARROW_LEFT  = 1000,
    ARROW_DOWN,
    ARROW_UP,
    ARROW_RIGHT,
    HOME_KEY,
    END_KEY,
    DELETE_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum editor_highlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

struct editor_syntax {
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

/* *** FILETYPES *** */

char *C_HL_extensions[] = { ".c" , ".h" , ".cpp" , NULL };
char *FS_HL_extensions[] = { ".fs" , ".fsx" , NULL };
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else", "struct",
    "union", "typedef", "static", "enum", "class", "case", "#define", "#include",

    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|", "void|", NULL
};

char *FS_HL_keywords[] = {
    "match", "if", "while", "for", "with", "return", "else", "elif",
    "type", "and", "static", "open", "let",

    "int|", "int16|", "int64|", "float|", "double|", "uint32|", "uint64|", "uint16|", "|>|", NULL
};

struct editor_syntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
    {
        "F#",
        FS_HL_extensions,
        FS_HL_keywords,
        "//", NULL, NULL,
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    }
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))


/* *** PROTOTYPES *** */

void editor_set_status_message(const char *fmt, ...);
void editor_refresh_screen();
char *editor_prompt(char *prompt, void (*callback)(char *, int));

/* *** ABUF *** */

void ab_append(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void ab_free(struct abuf *ab) {
    free(ab->b);
}

/* *** TERMINAL *** */

void clear_screen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

void die(const char *s) {
    clear_screen();

    perror(s);
    exit(1);
}

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &econf.original_termios) == -1)
        die("tcsetattr");
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &econf.original_termios) == -1)
        die("tcgetattr");

    atexit(disable_raw_mode);

    struct termios raw = econf.original_termios;
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | ICRNL | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editor_read_key() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1': return HOME_KEY;
                    case '3': return DELETE_KEY;
                    case '4': return END_KEY;
                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;
                    case '7': return HOME_KEY;
                    case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

/* *** SYNTAX HIGHLIGHTING *** */

int is_seperator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editor_update_syntax(erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if (econf.syntax == NULL) return;

    char **keywords = econf.syntax->keywords;

    char *scs = econf.syntax-> singleline_comment_start;
    char *mcs = econf.syntax-> multiline_comment_start;
    char *mce = econf.syntax-> multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && econf.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i -1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (econf.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (econf.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;

                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_seperator(row->render[i + klen])) {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_seperator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < econf.num_rows)
        editor_update_syntax(&econf.row[row->idx + 1]);
}

int editor_syntax_to_color(int hl) {
    switch (hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 36;
    case HL_KEYWORD1: return 33;
    case HL_KEYWORD2: return 32;
    case HL_STRING: return 35;
    case HL_NUMBER: return 31;
    case HL_MATCH: return 34;
    default: return 37;
    }
}

void editor_select_syntax_highlight() {
    econf.syntax = NULL;
    if (econf.filename == NULL) return;

    char *ext = strrchr(econf.filename, '.');

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editor_syntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(econf.filename, s->filematch[i]))) {
                econf.syntax = s;

                int file_row;
                for (file_row = 0; file_row < econf.num_rows; file_row++) {
                    editor_update_syntax(&econf.row[file_row]);
                }

                return;
            }
            i++;
        }
    }
}

/* *** ROW OPERATIONS *** */

int editor_row_cx_to_rx(erow *row, int cx) {
    int rx = 0;
    int j = 0;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

int editor_row_rx_to_cx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        cur_rx++;

        if (cur_rx > rx) return cx;
    }
    return rx;
}

void editor_update_row(erow *row) {
    int tabs = 0;
    int j;

    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t')
            tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0)
                row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;

    editor_update_syntax(row);
}

void editor_insert_row(int at, char *s, size_t len) {
    if (at < 0 || at > econf.num_rows) return;

    econf.row = realloc(econf.row, sizeof(erow) * (econf.num_rows + 1));
    memmove(&econf.row[at + 1], &econf.row[at], sizeof(erow) * (econf.num_rows - at));
    for (int j = at + 1; j <= econf.num_rows; j++) econf.row[j].idx++;

    econf.row[at].idx = at;

    econf.row[at].size = len;
    econf.row[at].chars = malloc(len + 1);
    memcpy(econf.row[at].chars, s, len);
    econf.row[at].chars[len] = '\0';

    econf.row[at].rsize = 0;
    econf.row[at].render = NULL;
    econf.row[at].hl = NULL;
    econf.row[at].hl_open_comment = 0;
    editor_update_row(&econf.row[at]);

    econf.num_rows++;
    econf.dirty = 1;
}

void editor_free_row(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editor_delete_row(int at) {
    if (at < 0 || at >= econf.num_rows)
        return;
    editor_free_row(&econf.row[at]);
    memmove(&econf.row[at], &econf.row[at + 1], sizeof(erow) * (econf.num_rows - at - 1));
    for (int j = at; j < econf.num_rows - 1; j++) econf.row[j].idx--;
    econf.num_rows--;
    econf.dirty = 1;
}

void editor_row_insert_char(erow *row, int at, int c) {
    if (at < 0 || at > row->size)
        at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
    econf.dirty = 1;
}

void editor_row_append_string(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    econf.dirty = 1;
}

void editor_row_delete_char(erow *row, int at) {
    if (at < 0 || at > row->size)
        return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    econf.dirty = 1;
}
/* *** EDITOR OPERATIONS *** */

void editor_insert_char(int c) {
    if (econf.cy == econf.num_rows) {
        editor_insert_row(econf.num_rows, "", 0);
    }
    editor_row_insert_char(&econf.row[econf.cy], econf.cx, c);
    econf.cx++;
}

void editor_insert_newline() {
    if (econf.cx == 0) {
        editor_insert_row(econf.cy, "", 0);
    } else {
        erow *row = &econf.row[econf.cy];
        editor_insert_row(econf.cy + 1, &row->chars[econf.cx], row->size - econf.cx);
        row = &econf.row[econf.cy];
        row->size = econf.cx;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }
    econf.cy++;
    econf.cx = 0;
}

void editor_delete_char() {
    if (econf.cy == econf.num_rows) return;
    if (econf.cx == 0 && econf.cy == 0) return;

    erow *row = &econf.row[econf.cy];
    if (econf.cx > 0) {
        editor_row_delete_char(row, econf.cx - 1);
        econf.cx--;
    } else {
        econf.cx = econf.row[econf.cy -1].size;
        editor_row_append_string(&econf.row[econf.cy - 1], row->chars, row->size);
        editor_delete_row(econf.cy);
        econf.cy--;
    }
}

/* *** FILE I/O *** */

void editor_open(char *filename) {
    free(econf.filename);
    econf.filename = strdup(filename);

    editor_select_syntax_highlight();

    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len = 13;
    while ((line_len = getline(&line, &line_cap, fp)) != -1) {
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
            line_len--;
        editor_insert_row(econf.num_rows, line, line_len);
    }
    free(line);
    fclose(fp);
    econf.dirty = 0;
}

char *editor_rows_to_string(int *buflen) {
    int total_len = 0;
    int j;
    for (j = 0; j < econf.num_rows; j++) {
        total_len += econf.row[j].size + 1;
    }
    *buflen = total_len;

    char *buf = malloc(total_len);
    char *p = buf;
    for (j = 0; j < econf.num_rows; j++) {
        memcpy(p, econf.row[j].chars, econf.row[j].size);
        p += econf.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editor_save() {
    if (econf.filename == NULL) {
        econf.filename = editor_prompt("Save as: %s", NULL);
        if (econf.filename == NULL) {
            editor_set_status_message("Save aborted");
            return;
        }
        editor_select_syntax_highlight();
    }

    int len;
    char *buf = editor_rows_to_string(&len);
    int fd = open(econf.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                econf.dirty = 0;
                editor_set_status_message("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editor_set_status_message("Can't save! I/O error: %s", strerror(errno));
}

/* *** FIND *** */

void editor_find_callback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl) {
        memcpy(econf.row[saved_hl_line].hl, saved_hl, econf.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int current = last_match;
    int i;
    for (i = 0; i < econf.num_rows; i++) {
        current += direction;
        if (current == -1) current = econf.num_rows - 1;
        else if (current == econf.num_rows) current = 0;

        erow *row = &econf.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            econf.cy = current;
            econf.cx = editor_row_rx_to_cx(row, match - row->render);
            econf.row_off = econf.num_rows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editor_find() {
    int saved_cx = econf.cx;
    int saved_cy = econf.cy;
    int saved_col_off = econf.col_off;
    int saved_row_off = econf.row_off;

    char *query = editor_prompt("Search: %s (Use ESC/Arrows/Enter)", editor_find_callback);

    if (query) {
        free(query);
    } else {
        econf.cx = saved_cx;
        econf.cy = saved_cy;
        econf.col_off = saved_col_off;
        econf.row_off = saved_row_off;
    }
}

/* *** INPUT *** */

char *editor_prompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();

        int c = editor_read_key();
        if (c == DELETE_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0)
                buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editor_set_status_message("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editor_set_status_message("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback) callback(buf, c);
    }
}

void editor_move_cursor(int key) {
    erow *row = (econf.cy >= econf.num_rows) ? NULL : &econf.row[econf.cy];

    switch (key) {
    case 'G':
        econf.cy = econf.num_rows;
        break;
    case 'h':
    case ARROW_LEFT:
        if (econf.cx > 0) {
            econf.cx--;
        } else if (econf.cy > 0){
            econf.cy--;
            econf.cx = econf.row[econf.cy].size;
        }
        break;
    case 'j':
    case ARROW_DOWN:
        if (econf.cy < econf.num_rows) {
            econf.cy++;
        }
        break;
    case 'k':
    case ARROW_UP:
        if (econf.cy > 0) {
            econf.cy--;
        }
        break;
    case 'l':
    case ARROW_RIGHT:
        if (row && econf.cx < row->size) {
            econf.cx++;
        } else if (row && econf.cx == row->size) {
            econf.cy++;
            econf.cx = 0;
        }
        break;
    }

    row = (econf.cy >= econf.num_rows) ? NULL : &econf.row[econf.cy];
    int row_len = row ? row->size : 0;
    if (econf.cx > row_len) {
        econf.cx = row->size;
    }
}

void editor_process_keypress() {
    static int quit_times = KILO_QUIT_TIMES;

    int c = editor_read_key();
    switch (c) {
    case '\r':
        editor_insert_newline();
        break;
    case CTRL_KEY('q'):
        if (econf.dirty && quit_times > 0) {
            editor_set_status_message("WARNING!! File has unsaved changes. "
                                      "Press Ctrl-Q %d more times to quit.", quit_times);
            quit_times--;
            return;
        }
        clear_screen();
        exit(0);
        break;
    case CTRL_KEY('s'):
        editor_save();
        break;

    /* case 'G': */
    /* case 'h': */
    /* case 'j': */
    /* case 'k': */
    /* case 'l': */
    case ARROW_LEFT:
    case ARROW_DOWN:
    case ARROW_UP:
    case ARROW_RIGHT:
        editor_move_cursor(c);
        break;

    case DELETE_KEY:
    case CTRL_KEY('h'):
    case BACKSPACE:
        if (c == DELETE_KEY)
            editor_move_cursor(ARROW_RIGHT);
        editor_delete_char();
        break;

    case HOME_KEY:
        econf.cx = 0;
        break;
    case END_KEY:
        if (econf.cy < econf.num_rows)
            econf.cx = econf.row[econf.cy].size;
        break;

    case CTRL_KEY('f'):
        editor_find();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
        {
            if (c == PAGE_UP) {
                econf.cy = econf.row_off;
            } else if (c == PAGE_DOWN) {
                econf.cy = econf.row_off + econf.screen_rows - 1;
                if (econf.cy > econf.num_rows)
                    econf.cy = econf.num_rows;
            }

            int times = econf.screen_rows;
            while (times--)
                editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        break;

    default:
        editor_insert_char(c);
    }

    quit_times = KILO_QUIT_TIMES;
}

/* *** OUTPUT *** */

void editor_scroll() {
    econf.rx = 0;
    if (econf.cy < econf.num_rows) {
        econf.rx = editor_row_cx_to_rx(&econf.row[econf.cy], econf.cx);
    }

    if (econf.cy < econf.row_off) {
        econf.row_off = econf.cy;
    }
    if (econf.cy >= econf.row_off + econf.screen_rows) {
        econf.row_off = econf.cy - econf.screen_rows + 1;
    }
    if (econf.rx < econf.col_off) {
        econf.col_off = econf.rx;
    }
    if (econf.rx >= econf.col_off + econf.screen_cols) {
        econf.col_off = econf.rx - econf.screen_cols + 1;
    }
}

void editor_draw_rows(struct abuf *ab) {
    int y;
    for (y = 0; y < econf.screen_rows; y++) {
        int file_row = y + econf.row_off;
        if (file_row >= econf.num_rows) {
            if (econf.num_rows == 0 && y == econf.screen_rows / 3) {
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome),
                                           "Kilo editor -- version %s", KILO_VERSION);
                if (welcome_len > econf.screen_cols)
                    welcome_len = econf.screen_cols;
                int padding = (econf.screen_cols - welcome_len) / 2;
                if (padding) {
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while (padding--) ab_append(ab, " ", 1);
                ab_append(ab, welcome, welcome_len);
            } else {
                ab_append(ab, "~", 1);
            }
        } else {
            int len = econf.row[file_row].rsize - econf.col_off;
            if (len < 0) len = 0;
            if (len > econf.screen_cols)
                len = econf.screen_cols;
            char *c = &econf.row[file_row].render[econf.col_off];
            unsigned char *hl = &econf.row[file_row].hl[econf.col_off];
            int current_color = -1;
            int j;
            for (j = 0; j < len; j++) {
                if (iscntrl(c[j])) {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    ab_append(ab, "\x1b[7m", 4);
                    ab_append(ab, &sym, 1);
                    ab_append(ab, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        ab_append(ab, buf, clen);
                    }
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        ab_append(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    ab_append(ab, &c[j], 1);
                } else {
                    int color = editor_syntax_to_color(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        ab_append(ab, buf, clen);
                    }
                    ab_append(ab, &c[j], 1);
                }
            }
            ab_append(ab, "\x1b[39m", 5);
        }
        // TODO: Get the size of the buffer based on how many digits in the text file
        ab_append(ab, "\x1b[K", 3);
        ab_append(ab, "\r\n", 2);
    }
}

void editor_draw_status_bar(struct abuf *ab) {
    ab_append(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    char *name = econf.filename ? econf.filename : "[No Name]";
    int len = snprintf(status, sizeof(status), "%.20s%s", name, econf.dirty ? "*" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                        econf.syntax ? econf.syntax->filetype : "no ft", econf.cy + 1, econf.num_rows);

    if (len > econf.screen_cols) len = econf.screen_cols;
    ab_append(ab, status, len);
    while (len < econf.screen_cols) {
        if (econf.screen_cols - len == rlen) {
            ab_append(ab, rstatus, rlen);
            break;
        } else {
            ab_append(ab, " ", 1);
            len++;
        }
    }
    ab_append(ab, "\x1b[m", 3);
    ab_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct abuf *ab) {
    ab_append(ab, "\x1b[K", 3);
    int msg_len = strlen(econf.status_msg);
    if (msg_len > econf.screen_cols)
        msg_len = econf.screen_cols;
    if (msg_len && time(NULL) - econf.status_msg_time < 5)
        ab_append(ab, econf.status_msg, msg_len);
}

void editor_refresh_screen() {
    editor_scroll();

    struct abuf ab = ABUF_INIT;

    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (econf.cy - econf.row_off) + 1, (econf.rx - econf.col_off) + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

void editor_set_status_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(econf.status_msg, sizeof(econf.status_msg), fmt, ap);
    va_end(ap);
    econf.status_msg_time = time(NULL);
}

/* *** INIT *** */

int get_cursor_position(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

int get_window_size(int *rows, int *cols) {
    struct winsize ws;

    int result = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    if (result == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void init_editor() {
    econf.cx = 0;
    econf.cy = 0;
    econf.rx = 0;
    econf.row_off = 0;
    econf.col_off = 0;
    econf.num_rows = 0;
    econf.row = NULL;
    econf.dirty = 0;
    econf.filename = NULL;
    econf.status_msg[0] = '\0';
    econf.status_msg_time = 0;
    econf.syntax = NULL;

    if (get_window_size(&econf.screen_rows, &econf.screen_cols) == -1)
        die("get_window_size");
    econf.screen_rows -= 2;
}

int main(int argc, char *argv[]) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}
