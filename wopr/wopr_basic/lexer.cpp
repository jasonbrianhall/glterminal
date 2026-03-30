#include "wopr.h"

// Global lexer state
toktype_t toktype;
string tok;
int num;
unsigned textp;
string thelin;
char thech;

#ifndef CLK_TCK
#define CLK_TCK CLOCKS_PER_SEC
#endif

// Read one keypress without waiting for Enter.
// blocking=true: wait for a key (GET); blocking=false: return "" if no key ready
// For non-blocking, add a small timeout to let keystrokes be captured
string getkey(bool blocking) {
  struct termios oldt, newt;
  tcgetattr(STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(unsigned)(ICANON | ECHO);
  newt.c_cc[VMIN] = blocking ? 1 : 0;
  newt.c_cc[VTIME] = blocking ? 0 : 1;  // 0.1 sec timeout for non-blocking
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);

  char c = 0;
  int n = (int)read(STDIN_FILENO, &c, 1);

  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  if (n <= 0)
    return "";
  return string(1, c);
}

void initlex(int n) {
  curline = n;
  textp = 1;
  initlex2();
}

void initlex2(void) {
  need_colon = false;
  thelin = pgm[curline];
  thech = ' ';
  nexttok();
}

void getch(void) {
  if (thelin.empty() || textp > thelin.length()) {
    thech = '\0';
  } else {
    thech = thelin[textp - 1];
    textp++;
  }
}

void nexttok(void) {
  static string punct = "#()*+,-/:;<=>?@\\^";
  toktype = kNONE;
begin:
  tok = thech;
  getch();
  if (tok[0] == '\0') {
    tok.clear();
  } else if (isspace(tok[0])) {
    goto begin;
  } else if (isalpha(tok[0])) {
    readident();
    if (tok == "rem")
      skiptoeol();
  } else if (isdigit(tok[0])) {
    readint();
  } else if (tok[0] == '"') {
    readstr();
  } else if (tok[0] == '\'') {
    skiptoeol();
  } else if (tok[0] == '&' && thech == 'H') {
    readhex();
  } else if (punct.find(tok[0]) != string::npos) {
    toktype = kPUNCT;
    if ((tok[0] == '<' && (thech == '>' || thech == '=')) ||
        (tok[0] == '>' && thech == '=')) {
      tok += thech;
      getch();
    }
  } else {
    printf("(%d, %d) What? %c (%d) %s\n", curline, textp, tok[0], tok[0],
           thelin.c_str());
    getch();
    errors = true;
  }
}

void skiptoeol(void) {
  tok.clear();
  toktype = kNONE;
  textp = thelin.length() + 1;
}

// store double quote as first char of string, to distinguish from idents
void readstr(void) {
  toktype = kSTRING;
  while (thech != '"') {
    if (thech == '\0') {
      printf("(%d, %d) String not terminated\n", curline, textp);
      errors = true;
      return;
    }
    tok += thech;
    getch();
  }
  getch();
}

void readint(void) {
  char *endp;
  toktype = kNUMBER;
  while (isdigit(thech)) {
    tok += thech;
    getch();
  }
  num = strtol(tok.c_str(), &endp, 10);
}

void readhex(void) {
  char *endp;
  toktype = kNUMBER;
  // tok already contains '&', add 'H'
  tok += thech;  // add 'H'
  getch();
  // read hex digits
  while (isxdigit(thech)) {
    tok += thech;
    getch();
  }
  // parse as hex: skip '&H' prefix
  num = strtol(tok.c_str() + 2, &endp, 16);
}

void readident(void) {
  tok[0] = tolower(tok[0]);
  toktype = kIDENT;
  while (isalnum(thech)) {
    tok += tolower(thech);
    getch();
  }
  // absorb $ so A$ becomes a single token "a$"
  if (thech == '$') {
    tok += '$';
    getch();
  }
}
