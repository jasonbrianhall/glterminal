#include "wopr.h"

#ifndef CLK_TCK
#define CLK_TCK CLOCKS_PER_SEC
#endif

// Global program state
string pgm[c_maxlines + 1];
int curline;
bool goto_executed = false;  // Flag set when GOTO/GOSUB changes line
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

// Named array system
Array arrays[MAX_ARRAYS];
int array_count = 0;

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
    //printf("Took %.2f seconds\n", (float)(tt) / (float)CLK_TCK);
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

// Get array by name
Array* get_array(const string& name) {
  for (int i = 0; i < array_count; i++) {
    if (arrays[i].name == name) {
      return &arrays[i];
    }
  }
  return nullptr;
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
  string name = tok;
  int var = getvarindex();
  nexttok();
  
  // Check if this is an array assignment like A(5) = 10 or AMORT(0,1) = 5
  if (tok == "(") {
    nexttok();
    int idx = (int)expression(0);
    int idx2 = 0;
    
    // Handle multi-dimensional arrays
    if (tok == ",") {
      nexttok();
      idx2 = (int)expression(0);
    }
    
    expect(")");
    expect("=");
    double val = expression(0);
    
    // Find and assign to the array
    Array* arr = get_array(name);
    if (arr != nullptr) {
      // For multi-dimensional arrays stored as linear: row*cols + col
      int linear_idx = idx * 500 + idx2;  // Simplified for 500-wide arrays
      
      if (linear_idx >= 0 && linear_idx < arr->size) {
        arr->data[linear_idx] = val;
        if (tracing)
          printf("*** %s(%d,%d) = %.6g\n", name.c_str(), idx, idx2, val);
      } else {
        printf("(%d, %d) Array index out of range: %d\n", curline, textp, linear_idx);
      }
    } else {
      printf("(%d, %d) Array not found: %s\n", curline, textp, name.c_str());
    }
  } else {
    // Regular variable assignment
    expect("=");
    vars[var] = expression(0);
    if (tracing)
      printf("*** %c = %.6g\n", var + 'a', vars[var]);
  }
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

void lineinputstmt(void) {
    string st;
    char *endp;

    // LINE INPUT "prompt"; variable  OR  LINE INPUT prompt, variable
    if (toktype == kSTRING) {
        // Print the prompt (string without quotes)
        printf("%s", tok.substr(1).c_str());
        nexttok();
        if (!accept(";")) {
            expect(",");
        }
    } else {
        // No prompt specified, just get the variable
    }

    // Now read the input
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
  string format_str;
  
  // Get the format string
  if (toktype == kSTRING) {
    format_str = tok.substr(1);  // Remove leading quote
    nexttok();
  } else {
    printf("(%d, %d) PRINT USING: Format string required\n", curline, textp);
    return;
  }
  
  // Expect comma or semicolon
  if (!accept(";")) {
    accept(",");
  }
  
  // Parse and apply format to each expression
  size_t fmt_pos = 0;
  int item_count = 0;
  
  while (tok != ":" && !tok.empty()) {
    double val = 0.0;
    string str_val;
    bool is_string = false;
    
    // Get the value to print
    if (toktype == kSTRING) {
      str_val = tok.substr(1);
      is_string = true;
      nexttok();
    } else if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') {
      str_val = svars[tok[0] - 'a'];
      is_string = true;
      nexttok();
    } else {
      val = expression(0);
      is_string = false;
    }
    
    // Find next format specifier in the format string
    string fmt_spec = "";
    bool found_spec = false;
    size_t spec_start = fmt_pos;
    
    while (fmt_pos < format_str.length()) {
      char c = format_str[fmt_pos];
      
      if (c == '#' || c == '0' || c == '.') {
        if (!found_spec) {
          found_spec = true;
          spec_start = fmt_pos;
        }
        fmt_spec += c;
        fmt_pos++;
        
        // Check if format specifier is complete
        if (fmt_pos < format_str.length()) {
          char next = format_str[fmt_pos];
          if (next != '#' && next != '0' && next != '.') {
            break;
          }
        } else {
          break;
        }
      } else if (found_spec) {
        break;  // End of current specifier
      } else {
        // Print literal characters
        printf("%c", c);
        fmt_pos++;
      }
    }
    
    // Apply formatting to the value
    if (found_spec && !is_string) {
      // Count digits before and after decimal
      int before_decimal = 0, after_decimal = 0;
      bool has_decimal = false;
      
      for (char c : fmt_spec) {
        if (c == '.') {
          has_decimal = true;
        } else if (!has_decimal) {
          before_decimal++;
        } else {
          after_decimal++;
        }
      }
      
      // Format the number
      if (has_decimal) {
        printf("%*.*f", before_decimal, after_decimal, val);
      } else {
        printf("%*d", before_decimal, (int)val);
      }
    } else if (found_spec && is_string) {
      // For strings, use width if available
      int width = fmt_spec.length();
      printf("%*s", width, str_val.c_str());
    } else if (!found_spec && !is_string) {
      // No format specifier, just print the value
      printf("%.6g", val);
    } else if (!found_spec && is_string) {
      printf("%s", str_val.c_str());
    }
    
    item_count++;
    
    // Check for more items to print
    if (!accept(",") && !accept(";")) {
      break;
    }
  }
  
  // Print any remaining format string
  while (fmt_pos < format_str.length()) {
    char c = format_str[fmt_pos];
    if (c != '#' && c != '0' && c != '.') {
      printf("%c", c);
    }
    fmt_pos++;
  }
  
  printf("\n");
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
  // DIM can handle: DIM A(100), B$(50), C(10,10)
  // For now, we support single-dimensional arrays: A(100)
  
  while (tok != ":" && !tok.empty()) {
    if (tok == "@") {
      // Special case: @ array
      nexttok();
      parenexpr();
      if (tok == ",") {
        nexttok();
      }
    } else if (toktype == kIDENT) {
      // Named array: A(100) or A$(50)
      string array_name = tok;
      bool is_string = (tok.back() == '$');
      
      if (is_string) {
        array_name = array_name.substr(0, array_name.length() - 1);  // Remove $
      }
      
      nexttok();
      
      // Expect opening parenthesis
      if (!accept("(")) {
        printf("(%d, %d) DIM: Expected '(' after array name\n", curline, textp);
        return;
      }
      
      // Get array size
      int size = (int)expression(0);
      
      // Check for multi-dimensional (for now we only use first dimension)
      if (accept(",")) {
        int size2 = (int)expression(0);
        // For now, treat multi-dimensional as size1 * size2
        size = size * size2;
      }
      
      // Expect closing parenthesis
      if (!accept(")")) {
        printf("(%d, %d) DIM: Expected ')' after array size\n", curline, textp);
        return;
      }
      
      // Create the array if it doesn't exist
      bool found = false;
      for (int i = 0; i < array_count; i++) {
        if (arrays[i].name == array_name) {
          // Array already exists, skip
          found = true;
          break;
        }
      }
      
      if (!found && array_count < MAX_ARRAYS) {
        arrays[array_count].name = array_name;
        arrays[array_count].size = size;
        arrays[array_count].is_string = is_string;
        arrays[array_count].data.resize(size, 0.0);
        array_count++;
      }
      
      // Handle comma between multiple DIM statements
      if (tok == ",") {
        nexttok();
      }
    } else {
      break;
    }
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
  
  // Print current stack contents
  stack<int> temp_gln = gln_stack;
  stack<int> temp_gtp = gtp_stack;
  int depth = 1;
  while (!temp_gln.empty()) {
    temp_gln.pop();
    temp_gtp.pop();
    depth++;
  }
  
  gln_stack.push(save_line);
  gtp_stack.push(save_textp);
  
  temp_gln = gln_stack;
  temp_gtp = gtp_stack;
  depth = 1;
  while (!temp_gln.empty()) {
    temp_gln.pop();
    temp_gtp.pop();
    depth++;
  }
  
  // Now jump to the target line
  initlex(target_line);
}

void returnstmt(void) {
  // Print current stack contents
  stack<int> temp_gln = gln_stack;
  stack<int> temp_gtp = gtp_stack;
  int depth = 1;
  while (!temp_gln.empty()) {
    temp_gln.pop();
    temp_gtp.pop();
    depth++;
  }
  
  if (gln_stack.empty()) {
    return;
  }
  
  int return_line = gln_stack.top();
  unsigned return_pos = gtp_stack.top();
  gln_stack.pop();
  gtp_stack.pop();
  
  
  temp_gln = gln_stack;
  temp_gtp = gtp_stack;
  depth = 1;
  while (!temp_gln.empty()) {
    temp_gln.pop();
    temp_gtp.pop();
    depth++;
  }
  
  // Jump back to the line with the GOSUB call, positioned to continue after the line number
  initlex_at(return_line, return_pos);
}

void gotostmt(void) {
  int n = expression(0);
  if (validlinenum(n)) {
    initlex(n);
    goto_executed = true;  // Signal that we've jumped to a new line
  }
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
