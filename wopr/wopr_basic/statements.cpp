#include "wopr.h"

#ifndef CLK_TCK
#define CLK_TCK CLOCKS_PER_SEC
#endif

// Global program state
string pgm[c_maxlines + 1];
int curline;
bool errors, tracing, need_colon;

// Stack for gosub/return
stack<int> gln_stack;
stack<int> gtp_stack;

// Timing
clock_t timestart;

// Variables and arrays
double vars[c_maxvars + 1];
double atarry[c_at_max];
string svars[c_maxvars];

// Data statement support
vector<double> data_store;
int data_ptr = 0;

// FOR loop state
double forvar[c_maxvars];
double forlimit[c_maxvars];
int forline[c_maxvars];
int forpos[c_maxvars];

void showtime(bool running) {
  if (running) {
    clock_t tt = clock() - timestart;
    printf("Took %.2f seconds\n", (float)(tt) / (float)CLK_TCK);
  }
}

void help(void) {
  puts("+----------------------------------------------------------------------"
       "----+");
  puts("|                           WOPR BASIC                                 "
       "   |");
  puts("+----------------------------------------------------------------------"
       "----+");
}

void clearvars(void) {
  for (int i = 0; i < c_maxvars; ++i) {
    vars[i] = 0;
    svars[i].clear();
  }
  while (!gln_stack.empty())
    gln_stack.pop();
  while (!gtp_stack.empty())
    gtp_stack.pop();
}

int getvarindex(void) {
  if (toktype != kIDENT) {
    printf("(%d, %d) Not a variable: %s\n", curline, textp, thelin.c_str());
    errors = true;
    return 0;
  }
  return tok[0] - 'a';
}

int getsvarindex(void) {
  if (toktype != kIDENT || tok.size() < 2 || tok.back() != '$') {
    printf("(%d, %d) Not a string variable: %s\n", curline, textp,
           thelin.c_str());
    errors = true;
    return 0;
  }
  return tok[0] - 'a';
}

int validlinenum(int n) {
  if (n <= 0 || n > c_maxlines) {
    printf("(%d, %d) Line number out of range", curline, textp);
    errors = true;
    return false;
  }
  return true;
}

void assign(void) {
  int var;

  var = getvarindex();
  nexttok();
  expect("=");
  vars[var] = expression(0);
  if (tracing)
    printf("*** %c = %d\n", var + 'a', vars[var]);
}

void rassign(void) {
  int var = getsvarindex();
  nexttok();
  expect("=");
  svars[var] = str_expression();
  if (tracing)
    printf("*** %c$ = \"%s\"\n", var + 'a', svars[var].c_str());
}

void arrassn(void) {
  int n, atndx;

  atndx = parenexpr();
  if (!accept("=")) {
    printf("(%d, %d) Array Assign: Expecting '=', found: %s", curline, textp, tok.c_str());
    errors = true;
  } else {
    n = expression(0);
    atarry[atndx] = n;
    if (tracing)
      printf("*** @(%d) = %d\n", atndx, n);
  }
}

void forstmt(void) {
  int var, forndx, n;

  var = getvarindex();
  assign();
  forndx = var;
  forvar[forndx] = vars[var];
  if (!accept("to")) {
    printf("(%d, %d) For: Expecting 'to', found: %s\n", curline, textp, tok.c_str());
    errors = true;
  } else {
    n = expression(0);
    forlimit[forndx] = n;
    forline[forndx] = curline;
    if (tok.empty())
      forpos[forndx] = textp;
    else
      forpos[forndx] = textp - 2;
  }
}

void nextstmt(void) {
  int forndx;

  forndx = getvarindex();
  forvar[forndx] = forvar[forndx] + 1;
  vars[forndx] = forvar[forndx];
  if (tracing)
    printf("*** %c = %d\n", forndx + 'a', vars[forndx]);
  if (forvar[forndx] <= forlimit[forndx]) {
    curline = forline[forndx];
    textp = forpos[forndx];
    initlex2();
  } else
    nexttok();
}

void ifstmt(void) {
  need_colon = false;
  if (expression(0) == 0)
    skiptoeol();
  else {
    accept("then");
    // Handle GOTO, GOSUB, or direct line number in IF...THEN statements
    if (tok == "goto") {
      nexttok();
      gotostmt();
    } else if (tok == "gosub") {
      nexttok();
      gosubstmt();
    } else if (toktype == kNUMBER) {
      gotostmt();
    }
  }
}

void inputstmt(void) {
    string st;
    char *endp;

    if (toktype == kSTRING) {
        printf("%s", tok.substr(1).c_str());
        nexttok();
        if (!accept(";"))
            expect(",");
        printf("? ");
    } else {
        printf("? ");
    }

    if (toktype == kIDENT && tok.back() == '$') {
        int var = getsvarindex();
        nexttok();
        getline(cin, svars[var]);
    } else {
        int var = getvarindex();
        nexttok();
        getline(cin, st);
        if (st.empty())
            vars[var] = 0;
        else if (isdigit(st[0]) || (st[0] == '-' && st.size() > 1))
            vars[var] = strtod(st.c_str(), &endp);
        else
            vars[var] = st[0];
    }
}

void printstmt(void) {
    int printwidth, printnl = true;

    while (tok != ":" && !tok.empty()) {
        printnl = true;
        printwidth = 0;

        if (accept("#")) {
            if (num <= 0) {
                printf("Expecting a print width, found: %s\n", pgm[curline].c_str());
                return;
            }
            printwidth = num;
            nexttok();
            if (!accept(",")) {
                printf("Print: Expecting a ',', found: %s\n", pgm[curline].c_str());
                return;
            }
        }

        if (toktype == kSTRING) {
            printf("%*s", printwidth, tok.substr(1).c_str());
            nexttok();
        } else if (tok == "spc") {
            nexttok();
            int n = parenexpr();
            printf("%*s", n, "");
        } else if (tok == "chr$" || tok == "left$" || tok == "right$" ||
                   tok == "mid$" || tok == "str$" || tok == "string$" ||
                   tok == "inkey$" || tok == "space$") {
            printf("%*s", printwidth, str_expression().c_str());
        } else if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') {
            printf("%*s", printwidth, svars[tok[0] - 'a'].c_str());
            nexttok();
        } else {
            if (printwidth)
                printf("%*.6g", printwidth, expression(0));
            else
                printf("%.6g", expression(0));
        }

        if (accept(",") || accept(";")) {
            printnl = false;
        } else {
            break;
        }
    }
    if (printnl)
        printf("\n");
}

void printusingstmt(void) {
  if (toktype == kSTRING)
    nexttok();
  accept(";");
  accept(",");
  printstmt();
}

void datastmt(void) {
  skiptoeol();
}

void restorestmt(void) { data_ptr = 0; }

void collect_data(void) {
  data_store.clear();
  data_ptr = 0;
  for (int i = 1; i < c_maxlines; ++i) {
    if (pgm[i].empty())
      continue;
    string line = pgm[i];
    string lline = line;
    for (auto &c : lline)
      c = tolower(c);
    size_t dp = lline.find("data");
    if (dp == string::npos)
      continue;
    if (dp > 0 && isalnum(lline[dp - 1]))
      continue;
    size_t pos = dp + 4;
    while (pos < line.size()) {
      while (pos < line.size() && isspace(line[pos]))
        pos++;
      if (pos >= line.size() || line[pos] == ':')
        break;
      int sign = 1;
      if (line[pos] == '-') {
        sign = -1;
        pos++;
      } else if (line[pos] == '+') {
        pos++;
      }
      if (pos < line.size() && isdigit(line[pos])) {
        int val = 0;
        while (pos < line.size() && isdigit(line[pos]))
          val = val * 10 + (line[pos++] - '0');
        data_store.push_back(sign * val);
      }
      while (pos < line.size() && isspace(line[pos]))
        pos++;
      if (pos < line.size() && line[pos] == ',')
        pos++;
    }
  }
}

void readstmt(void) {
  do {
    int var = getvarindex();
    nexttok();
    if ((int)data_store.size() == 0)
      collect_data();
    if (data_ptr >= (int)data_store.size()) {
      printf("Out of DATA\n");
      errors = true;
      return;
    }
    vars[var] = data_store[data_ptr++];
  } while (accept(","));
}

void dimstmt(void) {
  if (tok == "@") {
    nexttok();
    parenexpr();
  } else {
    skiptoeol();
  }
}

void pokestmt(void) {
  expression(0);
  expect(",");
  expression(0);
}

void getstmt(void) {
  if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') {
    int var = getsvarindex();
    nexttok();
    svars[var] = getkey(true);
  } else {
    int var = getvarindex();
    nexttok();
    string k = getkey(true);
    vars[var] = k.empty() ? 0 : (int)(unsigned char)k[0];
  }
}

void gosubstmt(void) {
  gln_stack.push(curline);
  gtp_stack.push(textp);
  gotostmt();
}

void returnstmt(void) {
  curline = gln_stack.top();
  gln_stack.pop();
  textp = gtp_stack.top();
  gtp_stack.pop();
  initlex2();
  // Skip past the GOSUB statement to next statement/line
  nexttok();
}

void gotostmt(void) {
  int n = expression(0);
  if (validlinenum(n))
    initlex(n);
}

void ongotostmt(void) {
  int n = expression(0);
  bool is_gosub = (tok == "gosub");
  if (!accept("goto") && !accept("gosub")) {
    printf("(%d, %d) ON: expecting GOTO or GOSUB\n", curline, textp);
    errors = true;
    return;
  }
  vector<int> targets;
  do {
    if (toktype != kNUMBER) {
      errors = true;
      return;
    }
    targets.push_back(num);
    nexttok();
  } while (accept(","));

  if (n >= 1 && n <= (int)targets.size()) {
    int target = targets[n - 1];
    if (is_gosub) {
      gln_stack.push(curline);
      gtp_stack.push(textp);
    }
    if (validlinenum(target))
      initlex(target);
  }
}

void runstmt(void) {
  timestart = clock();
  clearvars();
  data_store.clear();
  data_ptr = 0;
  initlex(1);
}
