/** includes **/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/** defines **/
#define EDITOR_VERSION "0.0.1"
#define EDITOR_TAB_STOP 8

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  PAGE_UP,
  PAGE_DOWN,
  HOME_KEY,
  END_KEY,
  DEL_KEY
};

/** data **/
typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

struct editorConfig {
  int cx;
  int cy;
  int rx;

  int rowoff;
  int coloff;

  int screenrows;
  int screencols;

  int numrows;
  erow *row;

  struct termios orig_termios;
};

struct editorConfig E;

/** terminal **/
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(EXIT_FAILURE);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
    die("tcgetattr");
  }

  atexit(disableRawMode);

  struct termios raw = E.orig_termios;

  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= CS8;
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
}

int editorReadKey() {
  ssize_t nread;
  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
  }

  if (c != '\x1b') {
    return c;
  }

  char seq[3];

  if (read(STDIN_FILENO, &seq[0], 1) != 1) {
    return '\x1b';
  }

  if (read(STDIN_FILENO, &seq[1], 1) != 1) {
    return '\x1b';
  }

  if (seq[0] == '[') {
    if (seq[1] >= '0' && seq[1] <= '9') {
      if (read(STDIN_FILENO, &seq[2], 1) != 1) {
        return '\x1b';
      }

      if (seq[2] == '~') {
        switch (seq[1]) {
        case '1':
          return HOME_KEY;

        case '3':
          return DEL_KEY;

        case '4':
          return END_KEY;

        case '5':
          return PAGE_UP;

        case '6':
          return PAGE_DOWN;

        case '7':
          return HOME_KEY;

        case '8':
          return END_KEY;

        default:
          break;
        }
      }
    } else {
      switch (seq[1]) {
      case 'A':
        return ARROW_UP;

      case 'B':
        return ARROW_DOWN;

      case 'C':
        return ARROW_RIGHT;

      case 'D':
        return ARROW_LEFT;

      case 'H':
        return HOME_KEY;

      case 'F':
        return END_KEY;

      default:
        break;
      }
    }
  } else if (seq[0] == 'O') {
    switch (seq[1]) {
    case 'H':
      return HOME_KEY;

    case 'F':
      return END_KEY;

    default:
      break;
    }
  }

  return '\x1b';
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) {
      break;
    }

    if (buf[i] == 'R') {
      break;
    }

    i++;
  }

  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') {
    return -1;
  }

  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    return -1;
  }

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }

    return getCursorPosition(rows, cols);
  }

  *cols = ws.ws_col;
  *rows = ws.ws_row;

  return 0;
}

/** row operations **/
int editorRowCxToRx(const erow *row, int cx) {
  int rx = 0;

  for (int j = 0; j < cx && j < row->size; j++) {
    if (row->chars[j] == '\t') {
      rx += (EDITOR_TAB_STOP - 1) - (rx % EDITOR_TAB_STOP);
    }

    rx++;
  }

  return rx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;

  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      tabs++;
    }
  }

  size_t render_size =
      (size_t)row->size + ((size_t)tabs * (EDITOR_TAB_STOP - 1)) + 1;

  char *new_render = malloc(render_size);

  if (new_render == NULL) {
    die("malloc");
  }

  free(row->render);
  row->render = new_render;

  int idx = 0;

  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';

      while (idx % EDITOR_TAB_STOP != 0) {
        row->render[idx++] = ' ';
      }
    } else {
      row->render[idx++] = row->chars[j];
    }
  }

  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAppendRow(const char *s, size_t len) {
  erow *new_rows = realloc(E.row, sizeof(erow) * (size_t)(E.numrows + 1));

  if (new_rows == NULL) {
    die("realloc");
  }

  E.row = new_rows;

  int at = E.numrows;

  E.row[at].size = (int)len;
  E.row[at].chars = malloc(len + 1);

  if (E.row[at].chars == NULL) {
    die("malloc");
  }

  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;

  editorUpdateRow(&E.row[at]);

  E.numrows++;
}

/** file I/O **/
void editorOpen(const char *filename) {
  FILE *fp = fopen(filename, "r");

  if (fp == NULL) {
    die("fopen");
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 &&
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
      linelen--;
    }

    editorAppendRow(line, (size_t)linelen);
  }

  free(line);

  if (fclose(fp) == EOF) {
    die("fclose");
  }
}

/** append buffer **/
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  if (len <= 0) {
    return;
  }

  char *new_buffer = realloc(ab->b, (size_t)ab->len + (size_t)len);

  if (new_buffer == NULL) {
    die("realloc");
  }

  memcpy(&new_buffer[ab->len], s, (size_t)len);

  ab->b = new_buffer;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);

  ab->b = NULL;
  ab->len = 0;
}

/** output **/
void editorScroll() {
  E.rx = 0;

  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }

  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }

  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }

  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  for (int y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;

    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];

        int welcomelen =
            snprintf(welcome, sizeof(welcome),
                     "Welcome to Nick's editor -- version %s", EDITOR_VERSION);

        if (welcomelen < 0) {
          welcomelen = 0;
        }

        if (welcomelen > E.screencols) {
          welcomelen = E.screencols;
        }

        int padding = (E.screencols - welcomelen) / 2;

        if (padding > 0) {
          abAppend(ab, "~", 1);
          padding--;
        }

        while (padding > 0) {
          abAppend(ab, " ", 1);
          padding--;
        }

        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;

      if (len < 0) {
        len = 0;
      }

      if (len > E.screencols) {
        len = E.screencols;
      }

      if (len > 0) {
        abAppend(ab, &E.row[filerow].render[E.coloff], len);
      }
    }

    abAppend(ab, "\x1b[K", 3);

    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  int len = 0;
  while (len < E.screencols) {
    abAppend(ab, " ", 1);
    len++;
  }
  abAppend(ab, "\x1b[m", 3);
}

void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);

  char buf[32];

  int cursor_length = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                               (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);

  if (cursor_length > 0) {
    abAppend(&ab, buf, cursor_length);
  }

  abAppend(&ab, "\x1b[?25h", 6);

  if (write(STDOUT_FILENO, ab.b, (size_t)ab.len) == -1) {
    abFree(&ab);
    die("write");
  }

  abFree(&ab);
}

/** input **/
void editorMoveCursor(int key) {
  erow *row = E.cy >= E.numrows ? NULL : &E.row[E.cy];

  switch (key) {
  case ARROW_LEFT:
    if (E.cx != 0) {
      E.cx--;
    } else if (E.cy > 0) {
      E.cy--;
      E.cx = E.row[E.cy].size;
    }

    break;

  case ARROW_RIGHT:
    if (row != NULL && E.cx < row->size) {
      E.cx++;
    } else if (row != NULL && E.cx == row->size) {
      E.cy++;
      E.cx = 0;
    }

    break;

  case ARROW_UP:
    if (E.cy != 0) {
      E.cy--;
    }

    break;

  case ARROW_DOWN:
    if (E.cy < E.numrows) {
      E.cy++;
    }

    break;

  default:
    break;
  }

  row = E.cy >= E.numrows ? NULL : &E.row[E.cy];

  int rowlen = row != NULL ? row->size : 0;

  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorProcessKeypress() {
  int c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    exit(EXIT_SUCCESS);

  case HOME_KEY:
    E.cx = 0;
    break;

  case END_KEY:
    if (E.cy < E.numrows) {
      E.cx = E.row[E.cy].size;
    }

    break;

  case PAGE_UP:
  case PAGE_DOWN: {
    if (c == PAGE_UP) {
      E.cy = E.rowoff;
    } else {
      E.cy = E.rowoff + E.screenrows - 1;

      if (E.cy > E.numrows) {
        E.cy = E.numrows;
      }
    }

    int times = E.screenrows;

    while (times > 0) {
      editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);

      times--;
    }

    break;
  }

  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;

  default:
    break;
  }
}

/** init **/
void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;

  E.rowoff = 0;
  E.coloff = 0;

  E.numrows = 0;
  E.row = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
    die("getWindowSize");
  }
  E.screenrows -= 1;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();

  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return EXIT_SUCCESS;
}
