#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
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

/* *** DATA TYPES *** */

typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
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
    char *filename;
    char status_msg[80];
    time_t status_msg_time;
    struct termios original_termios;
};

struct editor_config econf;

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT { NULL , 0 }

enum editor_key {
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
    /* clear_screen(); */

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

/* *** FILE I/O *** */

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
}

void editor_append_row(char *s, size_t len) {
    econf.row = realloc(econf.row, sizeof(erow) * (econf.num_rows + 1));

    int at = econf.num_rows;
    econf.row[at].size = len;
    econf.row[at].chars = malloc(len + 1);
    memcpy(econf.row[at].chars, s, len);
    econf.row[at].chars[len] = '\0';

    econf.row[at].rsize = 0;
    econf.row[at].render = NULL;
    editor_update_row(&econf.row[at]);

    econf.num_rows++;
}

void editor_open(char *filename) {
    free(econf.filename);
    econf.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len = 13;
    while ((line_len = getline(&line, &line_cap, fp)) != -1) {
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
            line_len--;
        editor_append_row(line, line_len);
    }
    free(line);
    fclose(fp);
}

/* *** INPUT *** */

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
    int c = editor_read_key();
    switch (c) {
    case CTRL_KEY('q'):
        clear_screen();
        exit(0);
        break;
    case 'G':
    case 'h':
    case 'j':
    case 'k':
    case 'l':
    case ARROW_LEFT:
    case ARROW_DOWN:
    case ARROW_UP:
    case ARROW_RIGHT:
        editor_move_cursor(c);
        break;

    case DELETE_KEY:
    case HOME_KEY:
        econf.cx = 0;
        break;
    case END_KEY:
        if (econf.cy < econf.num_rows)
            econf.cx = econf.row[econf.cy].size;
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
    }
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
            ab_append(ab, &econf.row[file_row].render[econf.col_off], len);
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
    int len = snprintf(status, sizeof(status), "%.20s", name);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", econf.cy + 1, econf.num_rows);

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
    econf.filename = NULL;
    econf.status_msg[0] = '\0';
    econf.status_msg_time = 0;

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

    editor_set_status_message("HELP: Ctrl-Q = quit");

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}
