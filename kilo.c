#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"

/* *** DATA TYPES *** */

struct editor_config {
    int cx, cy;
    int screen_rows;
    int screen_cols;
    struct termios original_termios;
};

struct editor_config conf;

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
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &conf.original_termios) == -1)
        die("tcsetattr");
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &conf.original_termios) == -1)
        die("tcgetattr");

    atexit(disable_raw_mode);

    struct termios raw = conf.original_termios;
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

/* *** INPUT *** */

void editor_move_cursor(int key) {
    switch (key) {
    case 'h':
    case ARROW_LEFT:
        if (conf.cx > 0) {
            conf.cx--;
        }
        break;
    case 'j':
    case ARROW_DOWN:
        if (conf.cy < conf.screen_rows - 1) {
            conf.cy++;
        }
        break;
    case 'k':
    case ARROW_UP:
        if (conf.cy > 0) {
            conf.cy--;
        }
        break;
    case 'l':
    case ARROW_RIGHT:
        if (conf.cx < conf.screen_cols - 1) {
            conf.cx++;
        }
        break;
    }
}

void editor_process_keypress() {
    int c = editor_read_key();
    switch (c) {
    case CTRL_KEY('q'):
        clear_screen();
        exit(0);
        break;
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
        conf.cx = 0;
        break;
    case END_KEY:
        conf.cx = conf.screen_cols - 1;
        break;

    case PAGE_UP:
    case PAGE_DOWN:
        {
            int times = conf.screen_rows;
            while (times--)
                editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;
    }
}

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

/* *** OUTPUT *** */

void editor_draw_rows(struct abuf *ab) {
    int y;
    for (y = 0; y < conf.screen_rows; y++) {
        if (y == conf.screen_rows / 3) {
            char welcome[80];
            int welcome_len = snprintf(welcome, sizeof(welcome),
                "Kilo editor -- version %s", KILO_VERSION);
            if (welcome_len > conf.screen_cols)
                welcome_len = conf.screen_cols;
            int padding = (conf.screen_cols - welcome_len) / 2;
            if (padding) {
                char str[11];
                int total = sprintf(str, "%d", y + 1);
                ab_append(ab, str, total);
                padding--;
            }
            while (padding--) ab_append(ab, " ", 1);
            ab_append(ab, welcome, welcome_len);
        } else {
            char str[11];
            int total = sprintf(str, "%d", y + 1);
            ab_append(ab, str, total);
        }
        // TODO: Get the size of the buffer based on how many digits in the text file
        ab_append(ab, "\x1b[K", 3);
        if (y < conf.screen_rows - 1 )
            ab_append(ab, "\r\n", 2);
    }
}

void editor_refresh_screen() {
    struct abuf ab = ABUF_INIT;

    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", conf.cy + 1, conf.cx + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

/* *** INIT *** */

void init_editor() {
    conf.cx = 0;
    conf.cy = 0;

    if (get_window_size(&conf.screen_rows, &conf.screen_cols) == -1)
        die("get_window_size");
}

int main() {
    enable_raw_mode();
    init_editor();

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}