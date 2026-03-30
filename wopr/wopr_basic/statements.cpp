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
  double cond = expression(0);
  
  if (cond != 0) {
    // Condition is TRUE - execute THEN part
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
    // Otherwise continue with next statement (which will be after the ELSE)
  } else {
    // Condition is FALSE - skip THEN, look for ELSE
    skiptoeol();  // Skip rest of THEN clause
    // Note: ELSE handling would require parsing the next line or remaining tokens
    // For now, just skip to end of line
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

        // Handle different types of print items
        bool printed_something = false;
        
        if (toktype == kSTRING) {
            printf("%*s", printwidth, tok.substr(1).c_str());
            nexttok();
            printed_something = true;
        } else if (tok == "spc") {
            nexttok();
            int n = parenexpr();
            printf("%*s", n, "");
            printed_something = true;
        } else if (tok == "chr$" || tok == "left$" || tok == "right$" ||
                   tok == "mid$" || tok == "str$" || tok == "string$" ||
                   tok == "inkey$" || tok == "space$") {
            printf("%*s", printwidth, str_expression().c_str());
            printed_something = true;
        } else if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') {
            printf("%*s", printwidth, svars[tok[0] - 'a'].c_str());
            nexttok();
            printed_something = true;
        } else if (toktype == kIDENT || toktype == kNUMBER || tok == "(" || 
                   tok == "-" || tok == "+" || tok == "not" ||
                   tok == "abs" || tok == "asc" || tok == "len" || tok == "val" ||
                   tok == "atn" || tok == "cos" || tok == "exp" || tok == "int" ||
                   tok == "log" || tok == "peek" || tok == "rnd" || tok == "sgn" ||
                   tok == "sin" || tok == "sqr" || tok == "tan" || tok == "usr" ||
                   tok == "@") {
            // This looks like an expression
            if (printwidth)
                printf("%*.6g", printwidth, expression(0));
            else
                printf("%.6g", expression(0));
            printed_something = true;
        } else if (tok == ":" || tok.empty()) {
            // End of statement
            break;
        } else {
            // Unknown token in print statement
            printf("(%d, %d) Unexpected token in PRINT: %s\n", curline, textp, tok.c_str());
            break;
        }

        if (!printed_something) {
            break;
        }

        // Handle separators after the printed item
        if (tok == ",") {
            printf("\t");  // Tab for comma separator
            nexttok();
            printnl = true;
        } else if (tok == ";") {
            // Semicolon means no newline and no space
            nexttok();
            printnl = false;
        } else if (tok == ":" || tok.empty()) {
            // End of statement
            break;
        } else {
            // Space-separated item - just continue the loop
            // Don't consume token, just keep processing
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
  // At this point, tok contains the line number (as a NUMBER token)
  if (toktype != kNUMBER) {
    printf("(%d, %d) GOSUB: expecting line number, found: %s\n", curline, textp, tok.c_str());
    errors = true;
    return;
  }
  
  int target_line = num;
  int save_line = curline;
  
  // Consume the line number token to get the position after it
  nexttok();
  unsigned save_textp = textp;  // Position AFTER the line number token
  
  printf("[GOSUB] Calling line %d from line %d at position %u\n", target_line, save_line, save_textp);
  printf("[GOSUB] Stack before push (depth=%lu):\n", gln_stack.size());
  
  // Print current stack contents
  stack<int> temp_gln = gln_stack;
  stack<int> temp_gtp = gtp_stack;
  int depth = 1;
  while (!temp_gln.empty()) {
    printf("  [%d] line=%d, textp=%u\n", depth, temp_gln.top(), temp_gtp.top());
    temp_gln.pop();
    temp_gtp.pop();
    depth++;
  }
  
  gln_stack.push(save_line);
  gtp_stack.push(save_textp);
  
  printf("[GOSUB] Stack after push (depth=%lu):\n", gln_stack.size());
  temp_gln = gln_stack;
  temp_gtp = gtp_stack;
  depth = 1;
  while (!temp_gln.empty()) {
    printf("  [%d] line=%d, textp=%u\n", depth, temp_gln.top(), temp_gtp.top());
    temp_gln.pop();
    temp_gtp.pop();
    depth++;
  }
  
  // Now jump to the target line
  initlex(target_line);
}

void returnstmt(void) {
  printf("[RETURN] Return statement at line %d, position %u\n", curline, textp);
  printf("[RETURN] Stack before pop (depth=%lu):\n", gln_stack.size());
  
  // Print current stack contents
  stack<int> temp_gln = gln_stack;
  stack<int> temp_gtp = gtp_stack;
  int depth = 1;
  while (!temp_gln.empty()) {
    printf("  [%d] line=%d, textp=%u\n", depth, temp_gln.top(), temp_gtp.top());
    temp_gln.pop();
    temp_gtp.pop();
    depth++;
  }
  
  if (gln_stack.empty()) {
    printf("[RETURN] ERROR - Stack is empty! RETURN without GOSUB\n");
    printf("RETURN without GOSUB\n");
    return;
  }
  
  int return_line = gln_stack.top();
  unsigned return_pos = gtp_stack.top();
  gln_stack.pop();
  gtp_stack.pop();
  
  printf("[RETURN] Popped: return to line %d, position %u\n", return_line, return_pos);
  printf("[RETURN] Stack after pop (depth=%lu):\n", gln_stack.size());
  
  temp_gln = gln_stack;
  temp_gtp = gtp_stack;
  depth = 1;
  while (!temp_gln.empty()) {
    printf("  [%d] line=%d, textp=%u\n", depth, temp_gln.top(), temp_gtp.top());
    temp_gln.pop();
    temp_gtp.pop();
    depth++;
  }
  
  // Jump back to the line with the GOSUB call, positioned to continue after the line number
  initlex_at(return_line, return_pos);
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
