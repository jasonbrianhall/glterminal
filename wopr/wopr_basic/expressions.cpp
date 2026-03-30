#include "wopr.h"

double expression(int minprec) {
    double n = 0.0;

    // String comparison: A$ = "x", A$ <> "x", MID$(x,y,z)="a", etc.
    // Check if this is a string variable or string function
    bool is_strexpr =
        (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') ||
        tok == "mid$" || tok == "left$" || tok == "right$" || 
        tok == "chr$" || tok == "str$" || tok == "string$" || 
        tok == "inkey$" || tok == "space$";
    
    if (is_strexpr && minprec == 0) {
        string lhs = str_expression();
        if (tok == "=" || tok == "<>") {
            string op = tok;
            nexttok();
            string rhs = str_expression();
            return (op == "=") ? (lhs == rhs ? 1.0 : 0.0)
                               : (lhs != rhs ? 1.0 : 0.0);
        }
        return (double)lhs.size();
    }

    // prefix / primary
    if (toktype == kNUMBER) {
        n = numval;  // Use the full double precision value
        nexttok();
    } else if (tok == "-") {
        nexttok();
        n = -expression(7);
    } else if (tok == "+") {
        nexttok();
        n = expression(7);
    } else if (tok == "not") {
        nexttok();
        n = (expression(3) == 0.0) ? 1.0 : 0.0;
    } else if (tok == "abs") {
        nexttok();
        n = fabs(parenexpr());
    } else if (tok == "asc") {
        nexttok();
        expect("(");
        if (toktype == kSTRING) {
            n = (unsigned char)tok[1];
            nexttok();
        } else if (toktype == kIDENT && tok.back() == '$') {
            string s = svars[getsvarindex()];
            nexttok();
            n = s.empty() ? 0.0 : (unsigned char)s[0];
        } else {
            n = (unsigned char)tok[0];
            nexttok();
        }
        expect(")");
    } else if (tok == "len") {
        nexttok();
        expect("(");
        if (toktype == kIDENT && tok.back() == '$') {
            n = (double)svars[getsvarindex()].size();
            nexttok();
        } else {
            n = (double)str_expression().size();
        }
        expect(")");
    } else if (tok == "val") {
        nexttok();
        expect("(");
        string s = str_expression();
        expect(")");
        char *endp = nullptr;
        n = strtod(s.c_str(), &endp);
        if (endp == s.c_str())
            n = 0.0;
    } else if (tok == "inkey$") {
        nexttok();
        n = getkey(false).empty() ? 0.0 : 1.0;
    } else if (tok == "atn") {
        nexttok();
        n = atan(parenexpr());
    } else if (tok == "cos") {
        nexttok();
        n = cos(parenexpr());
    } else if (tok == "exp") {
        nexttok();
        n = exp(parenexpr());
    } else if (tok == "int") {
        nexttok();
        n = floor(parenexpr());
    } else if (tok == "log") {
        nexttok();
        {
            double v = parenexpr();
            n = (v > 0.0) ? log(v) : 0.0;
        }
    } else if (tok == "peek") {
        nexttok();
        int addr = (int)parenexpr();
        // Simulate BIOS memory for equipment detection (0x410)
        // Bit 5-4 indicate display type: 11b = 80-column (0x30)
        if (addr == 0x410) {
            n = 0x30;  // Report 80-column display available
        } else {
            n = 0.0;   // Default to 0 for other addresses
        }
    } else if (tok == "rnd" || tok == "irnd") {
        nexttok();
        int r = (int)parenexpr();
        if (r <= 0) r = 1;
        n = (double)rnd(r);
    } else if (tok == "sgn") {
        nexttok();
        double v = parenexpr();
        n = (v > 0.0) ? 1.0 : (v < 0.0 ? -1.0 : 0.0);
    } else if (tok == "sin") {
        nexttok();
        n = sin(parenexpr());
    } else if (tok == "sqr") {
        nexttok();
        {
            double v = parenexpr();
            n = (v >= 0.0) ? sqrt(v) : 0.0;
        }
    } else if (tok == "tan") {
        nexttok();
        n = tan(parenexpr());
    } else if (tok == "usr") {
        nexttok();
        parenexpr();
        n = 0.0;
    } else if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') {
        n = (double)svars[tok[0] - 'a'].size();
        nexttok();
    } else if (toktype == kIDENT) {
        // Check if this is a named array access like A(5)
        string name = tok;
        nexttok();
        
        if (tok == "(") {
            // This is an array access
            nexttok();
            int idx = (int)expression(0);
            int idx2 = 0;
            
            // Handle multi-dimensional arrays like AMORT(0,1)
            if (tok == ",") {
                nexttok();
                idx2 = (int)expression(0);
            }
            
            expect(")");
            
            // Find the array
            Array* arr = get_array(name);
            if (arr != nullptr) {
                // For multi-dimensional arrays stored as linear: row*cols + col
                // We assume 500,2 means 500 rows, 2 cols = linear size 1000
                int linear_idx = idx * 500 + idx2;  // Simplified for 500-wide arrays
                
                if (linear_idx < 0 || linear_idx >= arr->size) {
                    printf("(%d, %d) Array index out of range: %d (max: %d)\n",
                           curline, textp, linear_idx, arr->size - 1);
                    n = 0.0;
                } else {
                    n = arr->data[linear_idx];
                }
            } else {
                printf("(%d, %d) Array not found: %s\n", curline, textp, name.c_str());
                n = 0.0;
            }
        } else {
            // Regular variable
            n = vars[name[0] - 'a'];
        };
    } else if (tok == "@") {
        nexttok();
        int idx = (int)parenexpr();
        if (idx < 0 || idx >= c_at_max) {
            printf("(%d, %d) Array index out of range: %d\n",
                   curline, textp, idx);
            n = 0.0;
        } else {
            n = atarry[idx];
        }
    } else if (tok == "(") {
        n = parenexpr();
    } else {
        printf("(%d, %d) Syntax error: expecting an operand, found: %s toktype: %d\n",
               curline, textp, tok.c_str(), toktype);
        return n;
    }

    // infix
    for (;;) {
        if (minprec <= 1 && tok == "or") {
            nexttok();
            n = ((n != 0.0) || (expression(2) != 0.0)) ? 1.0 : 0.0;
        } else if (minprec <= 2 && tok == "and") {
            nexttok();
            n = ((n != 0.0) && (expression(3) != 0.0)) ? 1.0 : 0.0;
        } else if (minprec <= 4 && tok == "=") {
            nexttok();
            n = (n == expression(5)) ? 1.0 : 0.0;
        } else if (minprec <= 4 && tok == "<") {
            nexttok();
            n = (n < expression(5)) ? 1.0 : 0.0;
        } else if (minprec <= 4 && tok == ">") {
            nexttok();
            n = (n > expression(5)) ? 1.0 : 0.0;
        } else if (minprec <= 4 && tok == "<>") {
            nexttok();
            n = (n != expression(5)) ? 1.0 : 0.0;
        } else if (minprec <= 4 && tok == "<=") {
            nexttok();
            n = (n <= expression(5)) ? 1.0 : 0.0;
        } else if (minprec <= 4 && tok == ">=") {
            nexttok();
            n = (n >= expression(5)) ? 1.0 : 0.0;
        } else if (minprec <= 5 && tok == "+") {
            nexttok();
            n += expression(6);
        } else if (minprec <= 5 && tok == "-") {
            nexttok();
            n -= expression(6);
        } else if (minprec <= 6 && tok == "*") {
            nexttok();
            n *= expression(7);
        } else if (minprec <= 6 && tok == "/") {
            nexttok();
            {
                double rhs = expression(7);
                n = (rhs == 0.0) ? 0.0 : n / rhs;
            }
        } else if (minprec <= 6 && tok == "\\") {
            nexttok();
            {
                double rhs = expression(7);
                if (rhs == 0.0) {
                    n = 0.0;
                } else {
                    n = floor(n / rhs);
                }
            }
        } else if (minprec <= 6 && tok == "mod") {
            nexttok();
            {
                double rhs = expression(7);
                n = fmod(n, rhs);
            }
        } else if (minprec <= 8 && tok == "^") {
            nexttok();
            // right-associative exponent
            double rhs = expression(8);
            n = pow(n, rhs);
        } else {
            break;
        }
    }

    return n;
}

double parenexpr(void) {
    double n = 0.0;

    if (!accept("(")) {
        printf("(%d, %d) Paren Expr: Expecting '(', found: %s\n",
               curline, textp, tok.c_str());
    } else {
        n = expression(0);
        if (!accept(")")) {
            printf("(%d, %d) Paren Expr: Expecting ')', found: %s\n",
                   curline, textp, tok.c_str());
        }
    }
    return n;
}

int rnd(int range) { return rand() % range + 1; }

string str_expression(void) {
  string s;
  if (toktype == kSTRING) {
    s = tok.substr(1); // strip leading "
    nexttok();
  } else if (tok == "chr$") {
    nexttok();
    s = string(1, (char)parenexpr());
  } else if (tok == "left$") {
    nexttok();
    expect("(");
    string base = str_expression();
    expect(",");
    int n = expression(0);
    expect(")");
    s = base.substr(0, (size_t)n);
  } else if (tok == "right$") {
    nexttok();
    expect("(");
    string base = str_expression();
    expect(",");
    int n = expression(0);
    expect(")");
    s = n >= (int)base.size() ? base : base.substr(base.size() - n);
  } else if (tok == "mid$") {
    nexttok();
    expect("(");
    string base = str_expression();
    expect(",");
    int start = expression(0) - 1; // 1-based
    int len = -1;
    if (accept(","))
      len = expression(0);
    expect(")");
    if (start < 0)
      start = 0;
    if (start >= (int)base.size())
      s = "";
    else
      s = (len < 0) ? base.substr(start) : base.substr(start, len);
  } else if (tok == "str$") {
    nexttok();
    s = to_string(parenexpr());
  } else if (tok == "string$") {
    nexttok();
    expect("(");
    int n = expression(0);
    expect(",");
    if (toktype == kSTRING) {
      char c = tok.size() > 1 ? tok[1] : ' ';
      nexttok();
      s = string(n, c);
    } else {
      s = string(n, (char)expression(0));
    }
    expect(")");
  } else if (tok == "inkey$") {
    nexttok();
    s = getkey(false);
  } else if (tok == "space$") {
    nexttok();
    s = string(parenexpr(), ' ');
  } else if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') {
    s = svars[getsvarindex()];
    nexttok();
  } else {
    printf("(%d, %d) String expr expected, found: %s\n", curline, textp, tok.c_str());
    errors = true;
  }
  // string concatenation with +
  while (tok == "+") {
    nexttok();
    s += str_expression();
  }
  return s;
}

bool accept(const string s) {
  if (tok == s) {
    nexttok();
    return true;
  }
  return false;
}

bool expect(const string s) {
  if (!accept(s)) {
    printf("(%d, %d) Expecting %s, but found %s, %s\n", curline, textp,
           s.c_str(), tok.c_str(), thelin.c_str());
    return errors = true;
  }
  return false;
}
