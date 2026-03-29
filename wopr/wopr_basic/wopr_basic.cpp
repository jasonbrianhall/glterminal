/*
 WOPR BASIC - A Tiny BASIC Interpreter
 Supports: end, list, load, new, run, save
 gosub/return, goto, if, input, print, multi-statement lines (:)
 a single numeric array: @(n), and rnd(n)

 Supports an interactive mode.

 Note that this is a pure interpreter, e.g., no pre token stream or intermediate form.

 g++ -Wall -Wextra -Wpedantic -s -Os wopr-basic.cpp -o wopr-basic
*/
#include <cmath>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <stack>
#include <vector>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

using namespace std;

// Read one keypress without waiting for Enter.
// blocking=true: wait for a key (GET); blocking=false: return "" if no key ready (INKEY$)
static string getkey(bool blocking) {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(unsigned)(ICANON | ECHO);
    newt.c_cc[VMIN]  = blocking ? 1 : 0;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    char c = 0;
    int n = (int)read(STDIN_FILENO, &c, 1);

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    if (n <= 0) return "";
    return string(1, c);
}

#ifndef CLK_TCK
    #define CLK_TCK CLOCKS_PER_SEC
#endif

enum {c_maxlines = 7000, c_at_max = 1000, c_maxvars = 26};
typedef enum {kNONE, kPUNCT, kIDENT, kNUMBER, kSTRING} toktype_t;

toktype_t toktype;  // type of current token
string tok;         // current token
int num;            // if toktype is number
unsigned textp;     // pointer in current line, 0 based
string thelin;      // current line copied here
char  thech;        // current character
string pgm[c_maxlines+1];
int curline;
bool errors, tracing, need_colon;

stack<int> gln_stack;
stack<int> gtp_stack;

clock_t timestart;

double vars[c_maxvars+1];
double atarry[c_at_max];
string svars[c_maxvars];   // string variables A$..Z$

// DATA/READ/RESTORE support
vector<double> data_store;
int data_ptr = 0;

double forvar[c_maxvars];
double forlimit[c_maxvars];
int forline[c_maxvars];
int forpos[c_maxvars];

bool   accept(const string s);
void   arrassn(void);
void   assign(void);
void   clearvars(void);
void   datastmt(void);
void   dimstmt(void);
void   docmd(void);
bool   expect(const string s);
double expression(int minprec);
void   forstmt(void);
void   getch(void);
string getfilename(const string action);
int    getvarindex(void);
int    getsvarindex(void);
void   gosubstmt(void);
void   gotostmt(void);
void   getstmt(void);
void   help(void);
void   ifstmt(void);
void   initlex(int n);
void   initlex2(void);
void   inputstmt(void);
void   liststmt(void);
void   loadstmt(void);
void   newstmt(void);
void   nextstmt(void);
void   nexttok(void);
void   ongotostmt(void);
int    parenexpr(void);
void   pokestmt(void);
void   printstmt(void);
void   printusingstmt(void);
void   readident(void);
void   readint(void);
void   readstmt(void);
void   readstr(void);
void   restorestmt(void);
void   returnstmt(void);
int    rnd(int range);
void   runstmt(void);
void   savestmt(void);
void   showtime(bool running);
void   skiptoeol(void);
string str_expression(void);
void   rassign(void);
int    validlinenum(int n);

int main(int argc, char *argv[]) {
    if (argc > 1) {
        toktype = kSTRING;
        tok = "\"";
        tok += argv[1];
        loadstmt();
        toktype = kIDENT;
        tok = "run";
        docmd();
    } else {
        newstmt();
        help();
    }
    for (;;) {
        errors = false;
        printf("WOPR> ");
        getline(cin, pgm[0]);
        if (!pgm[0].empty()) {
            initlex(0);
            if (toktype == kNUMBER) {
                if (validlinenum(num))
                    pgm[num] = pgm[0].substr(textp - 1);
            } else
                docmd();
        }
    }
}

void docmd(void) {
    bool running = false;
    for (;;) {
        need_colon = true;
        if (tracing && tok != ":" && !tok.empty() && textp <= thelin.length())
            printf("[%d] %s %s\n", curline, tok.c_str(), thelin.substr(textp - 1).c_str());
        if        (tok == "bye" || tok == "quit") { nexttok(); exit(0);
        } else if (tok == "end" || tok == "stop") { nexttok(); showtime(running); return;
        } else if (tok == "clear")     { nexttok(); clearvars(); return;
        } else if (tok == "help")      { nexttok(); help(); return;
        } else if (tok == "list")      { nexttok(); liststmt(); return;
        } else if (tok == "load")      { nexttok(); loadstmt(); return;
        } else if (tok == "new")       { nexttok(); newstmt(); return;
        } else if (tok == "run")       { nexttok(); runstmt(); running = true;
        } else if (tok == "save")      { nexttok(); savestmt(); return;
        } else if (tok == "tron")      { nexttok(); tracing = true;
        } else if (tok == "troff")     { nexttok(); tracing = false;
        } else if (tok == "data")      { nexttok(); datastmt(); return;
        } else if (tok == "dim")       { nexttok(); dimstmt(); return;
        } else if (tok == "poke")      { nexttok(); pokestmt();
        } else if (tok == "read")      { nexttok(); readstmt();
        } else if (tok == "restore")   { nexttok(); restorestmt(); return;
        } else if (tok == "cls")       { nexttok();
        } else if (tok == "get")       { nexttok(); getstmt();
        } else if (tok == "on")        { nexttok(); ongotostmt();
        } else if (tok == "call")      { nexttok(); expression(0); // stub
        } else if (tok == "himem")     { nexttok(); accept(":"); expression(0); // stub
        } else if (tok == "lomem")     { nexttok(); accept(":"); expression(0); // stub
        } else if (tok == "color")     { nexttok(); // stub: consume up to 3 comma-separated args
                                         expression(0);
                                         if (accept(",")) { expression(0);
                                           if (accept(",")) expression(0); }
        } else if (tok == "locate")    { nexttok(); // stub: consume row,col[,cursor]
                                         expression(0);
                                         if (accept(",")) { expression(0);
                                           if (accept(",")) expression(0); }
        } else if (tok == "screen")    { nexttok(); // stub: consume up to 2 args
                                         expression(0);
                                         if (accept(",")) expression(0);
        } else if (tok == "width")     { nexttok(); expression(0); // stub
        } else if (tok == "key")       { nexttok(); accept("off"); accept("on"); // stub
        } else if (tok == "def")       { nexttok(); // DEF SEG [= expr] or DEF FN — skip
                                         if (accept("seg")) { if (accept("=")) expression(0); }
                                         else skiptoeol(); // DEF FN etc
        } else if (tok == "defdbl" || tok == "defsng" ||
                   tok == "defint" || tok == "defstr") { nexttok(); skiptoeol(); // type decls — stub
        } else if (tok == "line")      { nexttok(); // LINE INPUT "prompt";var$
                                         accept("input");
                                         if (toktype == kSTRING) { printf("%s", tok.substr(1).c_str()); nexttok(); accept(";"); accept(","); }
                                         if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') {
                                             int var = getsvarindex(); nexttok();
                                             getline(cin, svars[var]);
                                         } else { string tmp; getline(cin, tmp); }
        } else if (tok == "print")     { nexttok();
                                         if (tok == "using") { nexttok(); printusingstmt(); }
                                         else printstmt();
        } else if (tok == "?")         { nexttok(); printstmt();
        } else if (tok == "for")       { nexttok(); forstmt();
        } else if (tok == "gosub")     { nexttok(); gosubstmt();
        } else if (tok == "goto")      { nexttok(); gotostmt();
        } else if (tok == "if")        { nexttok(); ifstmt();
        } else if (tok == "input")     { nexttok(); inputstmt();
        } else if (tok == "next")      { nexttok(); nextstmt();
        } else if (tok == "let")       { nexttok();
                                         if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') rassign();
                                         else assign();
        } else if (tok == "return")    { nexttok(); returnstmt();
        } else if (tok == "@")         { nexttok(); arrassn();
        } else if (toktype == kIDENT)  {
            if (tok.size() >= 2 && tok.back() == '$') rassign();
            else assign();
        } else if (tok == ":" || tok.empty()) { /* handled below */
        } else {
            printf("(%d, %d) Unknown token %s: %s\n", curline, textp, tok.c_str(), pgm[curline].c_str());
            errors = true;
        }

        if (errors) return;
        if (tok.empty()) {
          while (tok.empty()) {
              if (curline == 0 || curline >= c_maxlines) { showtime(running); return;}
              initlex(curline + 1);
          }
        } else if (tok == ":") { nexttok();
        } else if (need_colon && !expect(":")) { return;
        }
    }
}

void showtime(bool running) {
    if (running) {
        clock_t tt = clock() - timestart;
        printf("Took %.2f seconds\n", (float)(tt)/(float)CLK_TCK);
    }
}

void help(void) {
   puts("+--------------------------------------------------------------------------+");
   puts("|                WOPR BASIC  -  Shall we play a game?                     |");
   puts("+--------------------------------------------------------------------------+");
   puts("| bye, clear, cls, end/stop, help, list, load/save, new, run, tron/off    |");
   puts("| call <addr>  himem: <n>  lomem: <n>  (stubs)                            |");
   puts("| data <n>[,<n>...] / read <var> / restore                                |");
   puts("| dim @(n)                                                                 |");
   puts("| for <var> = <e1> to <e2> ... next <var>                                 |");
   puts("| get <var>  (reads one char)                                              |");
   puts("| gosub <e> ... return  /  goto <e>                                       |");
   puts("| if <e> then <stmt>  /  on <e> goto/gosub <n>[,<n>...]                  |");
   puts("| input [prompt,] <var>  /  <var>=<expr>  /  poke <addr>,<val>            |");
   puts("| print <expr|str>[,<expr|str>][;]                                        |");
   puts("| rem <any>  or  ' <any>                                                  |");
   puts("| Operators: ^  * / \\ mod  + -  < <= > >= = <>  not and or               |");
   puts("| Num vars: a..z   String vars: a$..z$   Array: @(n)                      |");
   puts("| Num fns:  abs atn cos exp int len log peek rnd sgn sin sqr tan usr val  |");
   puts("| Str fns:  asc chr$ left$ mid$ right$ str$                               |");
   puts("| Note: trig args/results scaled x1000000 (fixed-point)                   |");
   puts("+--------------------------------------------------------------------------+");
}

void gosubstmt(void) {      // for gosub: save the line and column
    gln_stack.push(curline);
    gtp_stack.push(textp);

    gotostmt();
}

// Evaluate a string expression: string literal or string variable or string function
string str_expression(void) {
    string s;
    if (toktype == kSTRING) {
        s = tok.substr(1);   // strip leading "
        nexttok();
    } else if (tok == "chr$") {
        nexttok(); s = string(1, (char)parenexpr());
    } else if (tok == "left$") {
        nexttok(); expect("(");
        string base = str_expression(); expect(",");
        int n = expression(0); expect(")");
        s = base.substr(0, (size_t)n);
    } else if (tok == "right$") {
        nexttok(); expect("(");
        string base = str_expression(); expect(",");
        int n = expression(0); expect(")");
        s = n >= (int)base.size() ? base : base.substr(base.size() - n);
    } else if (tok == "mid$") {
        nexttok(); expect("(");
        string base = str_expression(); expect(",");
        int start = expression(0) - 1;  // 1-based
        int len = -1;
        if (accept(",")) len = expression(0);
        expect(")");
        if (start < 0) start = 0;
        if (start >= (int)base.size()) s = "";
        else s = (len < 0) ? base.substr(start) : base.substr(start, len);
    } else if (tok == "str$") {
        nexttok(); s = to_string(parenexpr());
    } else if (tok == "string$") {
        nexttok(); expect("(");
        int n = expression(0); expect(",");
        if (toktype == kSTRING) { char c = tok.size() > 1 ? tok[1] : ' '; nexttok(); s = string(n, c); }
        else { s = string(n, (char)expression(0)); }
        expect(")");
    } else if (tok == "inkey$") {
        nexttok(); s = getkey(false);
    } else if (tok == "space$") {
        nexttok(); s = string(parenexpr(), ' ');
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

// String variable assignment:  A$ = <str_expr>
void rassign(void) {
    int var = getsvarindex();
    nexttok();
    expect("=");
    svars[var] = str_expression();
    if (tracing) printf("*** %c$ = \"%s\"\n", var + 'a', svars[var].c_str());
}

void assign(void) {
  int var;

  var = getvarindex();
  nexttok();
  expect("=");
  vars[var] = expression(0);
  if (tracing) printf("*** %c = %d\n", var + 'a', vars[var]);
}

void arrassn(void) {        // array assignment: @(expr) {} = expr
    int n, atndx;

    atndx = parenexpr();
    if (!accept("=")) {
        printf("(%d, %d) Array Assign: Expecting '=', found: %s", curline, textp, tok.c_str());
        errors = true;
    } else {
        n = expression(0);
        atarry[atndx] = n;
        if (tracing) printf("*** @(%d) = %d\n", atndx, n);
    }
}

void forstmt(void) {    // for i = expr to expr
    int var, forndx, n;

    var = getvarindex();
    assign();
    // vars(var) has the value; var has the number value of the variable in 0..25
    forndx = var;
    forvar[forndx] = vars[var];
    if (!accept("to")) {
        printf("(%d, %d) For: Expecting 'to', found: %s\n", curline, textp, tok.c_str()); errors = true;
    } else {
        n = expression(0);
        forlimit[forndx] = n;
        // need to store iter, limit, line, and col
        forline[forndx] = curline;
        if (tok.empty()) forpos[forndx] = textp; else forpos[forndx] = textp - 2;
        //forpos[forndx] textp; if (tok != "") forpos[forndx] -=2;
    }
}

void ifstmt(void) {
    need_colon = false;
    if (expression(0) == 0)
        skiptoeol();
    else {
        accept("then");      // "then" is optional
        if (toktype == kNUMBER) gotostmt();
    }
}

void inputstmt(void) {      // "input" [string ","] var
    string st;
    char *endp;

    if (toktype == kSTRING) {
        printf("%s", tok.substr(1).c_str());
        nexttok();
        if (!accept(";")) expect(",");  // accept either ";" or "," after prompt
    } else
        printf("? ");

    if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') {
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
            vars[var] = strtol(st.c_str(), &endp, 10);
        else
            vars[var] = st[0];
    }
}

void liststmt(void) {
    for (int i = 1; i < c_maxlines; ++i) {
        if (!pgm[i].empty())
            printf("%d %s\n", i, pgm[i].c_str());
    }
    printf("\n");
}

void loadstmt(void) {
    int n;
    string filename;

    newstmt();
    filename = getfilename("Load");
    if (filename.empty()) return;

    ifstream infile(filename.c_str());
    if (!infile) return;

    n = 0;
    while (getline(infile, pgm[0])) {
        initlex(0);
        if (toktype == kNUMBER && validlinenum(num)) {
            pgm[num] = pgm[0].substr(textp - 1);
            n = num;
        } else {
            n++;
            pgm[n] = pgm[0];
        }
    }
    infile.close();
    curline = 0;
}

void newstmt(void) {
    clearvars();
    for (int i = 0; i < c_maxlines; ++i) pgm[i].clear();
}

void nextstmt(void) {
    int forndx;

    // tok needs to have the variable
    forndx = getvarindex();
    forvar[forndx] = forvar[forndx] + 1;
    vars[forndx] = forvar[forndx];
    if (tracing) printf("*** %c = %d\n", forndx + 'a', vars[forndx]);
    if (forvar[forndx] <= forlimit[forndx]) {
        curline = forline[forndx];
        textp   = forpos[forndx];
        initlex2();
    } else
        nexttok(); //' skip the ident for now
}

void printstmt(void) {
    int printwidth, printnl = true;

    while (tok != ":" && !tok.empty()) {
        printnl = true;
        printwidth = 0;

        if (accept("#")) {
            if (num <= 0) {printf("Expecting a print width, found: %s\n", pgm[curline].c_str()); return;}
            printwidth = num;
            nexttok();
            if (!accept(",")) {printf("Print: Expecting a ',', found: %s\n", pgm[curline].c_str()); return;}
        }

        if (toktype == kSTRING) {
            printf("%*s", printwidth, tok.substr(1).c_str());
            nexttok();
        } else if (tok == "spc") {
            nexttok(); int n = parenexpr(); printf("%*s", n, "");
        } else if (tok == "chr$" || tok == "left$" || tok == "right$" || tok == "mid$" ||
                   tok == "str$" || tok == "string$" || tok == "inkey$" || tok == "space$") {
            printf("%*s", printwidth, str_expression().c_str());
        } else if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') {
            printf("%*s", printwidth, svars[tok[0]-'a'].c_str());
            nexttok();
        } else {
            printf("%*d", printwidth, expression(0));
        }

        if (accept(",") || accept(";")) {printnl = false;} else {break; }
    }
    if (printnl) printf("\n");
}

// PRINT USING "fmt"; expr — minimal: skip format string, print value(s)
void printusingstmt(void) {
    // consume format string
    if (toktype == kSTRING) nexttok();
    accept(";"); accept(",");
    // print remaining items without formatting
    printstmt();
}

void datastmt(void) {   // DATA val, val, ... — scan program for all DATA lines at run time
    // DATA is collected by readstmt; here we just skip the line
    skiptoeol();
}

void restorestmt(void) {
    data_ptr = 0;
}

// Scan entire program collecting all DATA values into data_store
void collect_data(void) {
    data_store.clear();
    data_ptr = 0;
    for (int i = 1; i < c_maxlines; ++i) {
        if (pgm[i].empty()) continue;
        // quick scan: does the line contain "data" keyword?
        string line = pgm[i];
        // find "data" token (simple scan)
        size_t pos = 0;
        // skip line number
        while (pos < line.size() && isspace(line[pos])) pos++;
        // check for "data" keyword
        string lline = line;
        for (auto &c : lline) c = tolower(c);
        size_t dp = lline.find("data");
        if (dp == string::npos) continue;
        // make sure it's a keyword (preceded by space or start, followed by space/digit/sign)
        if (dp > 0 && isalnum(lline[dp-1])) continue;
        pos = dp + 4;
        // parse comma-separated integers
        while (pos < line.size()) {
            while (pos < line.size() && isspace(line[pos])) pos++;
            if (pos >= line.size() || line[pos] == ':') break;
            int sign = 1;
            if (line[pos] == '-') { sign = -1; pos++; }
            else if (line[pos] == '+') { pos++; }
            if (pos < line.size() && isdigit(line[pos])) {
                int val = 0;
                while (pos < line.size() && isdigit(line[pos]))
                    val = val * 10 + (line[pos++] - '0');
                data_store.push_back(sign * val);
            }
            while (pos < line.size() && isspace(line[pos])) pos++;
            if (pos < line.size() && line[pos] == ',') pos++;
        }
    }
}

void readstmt(void) {   // READ var [, var ...]
    do {
        int var = getvarindex();
        nexttok();
        if ((int)data_store.size() == 0) collect_data();
        if (data_ptr >= (int)data_store.size()) {
            printf("Out of DATA\n"); errors = true; return;
        }
        vars[var] = data_store[data_ptr++];
    } while (accept(","));
}

void dimstmt(void) {    // DIM @(n) — resize the single array; other DIM forms are no-ops
    // We only support @(n); named array dims are accepted and ignored
    if (tok == "@") {
        nexttok();
        // array size is fixed at c_at_max; just consume the expression
        parenexpr();
    } else {
        // consume "A(n)" style dim — just skip to end of statement
        skiptoeol();
    }
}

void pokestmt(void) {   // POKE addr, val — stub (no real memory access)
    expression(0);
    expect(",");
    expression(0);
    // no-op in a hosted interpreter
}

void getstmt(void) {    // GET var — blocking single keypress
    if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') {
        int var = getsvarindex(); nexttok();
        svars[var] = getkey(true);
    } else {
        int var = getvarindex(); nexttok();
        string k = getkey(true);
        vars[var] = k.empty() ? 0 : (int)(unsigned char)k[0];
    }
}

void ongotostmt(void) { // ON expr GOTO/GOSUB line1, line2, ...
    int n = expression(0);
    bool is_gosub = (tok == "gosub");
    if (!accept("goto") && !accept("gosub")) {
        printf("(%d, %d) ON: expecting GOTO or GOSUB\n", curline, textp);
        errors = true; return;
    }
    // collect line numbers
    vector<int> targets;
    do {
        if (toktype != kNUMBER) { errors = true; return; }
        targets.push_back(num);
        nexttok();
    } while (accept(","));

    if (n >= 1 && n <= (int)targets.size()) {
        int target = targets[n - 1];
        if (is_gosub) {
            gln_stack.push(curline);
            gtp_stack.push(textp);
        }
        if (validlinenum(target)) initlex(target);
    }
    // if n out of range, fall through to next statement
}

void returnstmt(void) {     // return from a subroutine
    curline = gln_stack.top(); gln_stack.pop();
    textp   = gtp_stack.top(); gtp_stack.pop();
    initlex2();
}

void runstmt(void) {
    timestart = clock();
    clearvars();
    data_store.clear();
    data_ptr = 0;
    initlex(1);
}

void gotostmt(void) {
    int n = expression(0);
    if (validlinenum(n)) initlex(n);
}

void savestmt(void) {
    string filename;

    filename = getfilename("Load");
    if (filename.empty()) return;

    ofstream outfile(filename.c_str());
    if (!outfile) return;

    for (int i = 1; i < c_maxlines; ++i) {
        if (!pgm[i].empty()) {
            outfile << i << " " << pgm[i] << endl;
        }
    }
    outfile.close();
}

string getfilename(const string action) {
    string filename;

    if (toktype == kSTRING)
        filename = tok.substr(1);
    else {
        printf("%s: ", action.c_str());
        getline(cin, filename);
    }
    if (filename.empty()) return filename;

    if (filename.find(".") == string::npos)
        filename += ".bas";

    return filename;
}

int validlinenum(int n) {
    if (n <= 0 || n > c_maxlines) {
        printf("(%d, %d) Line number out of range", curline, textp);
        errors = true; return false;
    }
    return true;
}

void clearvars(void) {
    for (int i = 0; i < c_maxvars; ++i) { vars[i] = 0; svars[i].clear(); }
    while (!gln_stack.empty()) gln_stack.pop();
    while (!gtp_stack.empty()) gtp_stack.pop();
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
        printf("(%d, %d) Not a string variable: %s\n", curline, textp, thelin.c_str());
        errors = true;
        return 0;
    }
    return tok[0] - 'a';
}

bool expect(const string s) {
    if (!accept(s)) {
        printf("(%d, %d) Expecting %s, but found %s, %s\n", curline, textp, s.c_str(), tok.c_str(), thelin.c_str());
        return errors = true;
    }
    return false;
}

bool accept(const string s) {
    if (tok == s) { nexttok(); return true;}
    return false;
}

double expression(int minprec) {
    double n = 0;

    // String comparison: A$ = "x", A$ <> "x", INKEY$ = "", etc.
    // Detect: string-var or string-func followed by = or <>
    bool is_strexpr = (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') ||
                      tok == "inkey$" || tok == "chr$" || tok == "left$" ||
                      tok == "right$" || tok == "mid$" || tok == "str$" || tok == "string$";
    if (is_strexpr && minprec == 0) {
        string lhs = str_expression();
        if (tok == "=" || tok == "<>") {
            string op = tok; nexttok();
            string rhs = str_expression();
            return (op == "=") ? (lhs == rhs ? 1 : 0) : (lhs != rhs ? 1 : 0);
        }
        // used in non-comparison context: return length
        return lhs.size();
    }

    // handle numeric operands, unary operators, functions, variables
    if        (toktype == kNUMBER) { n = num; nexttok();
    } else if (tok == "-")         { nexttok(); n = -expression(7);
    } else if (tok == "+")         { nexttok(); n =  expression(7);
    } else if (tok == "not")       { nexttok(); n = !expression(3);
    } else if (tok == "abs")       { nexttok(); n = abs(parenexpr());
    } else if (tok == "asc")       { nexttok(); expect("(");
                                     if (toktype == kSTRING) { n = (unsigned char)tok[1]; nexttok(); }
                                     else if (toktype == kIDENT && tok.back() == '$') { string s = svars[getsvarindex()]; nexttok(); n = s.empty() ? 0 : (unsigned char)s[0]; }
                                     else { n = tok[1]; nexttok(); }
                                     expect(")");
    } else if (tok == "len")       { nexttok(); expect("(");
                                     if (toktype == kIDENT && tok.back() == '$') { n = svars[getsvarindex()].size(); nexttok(); }
                                     else { n = str_expression().size(); }
                                     expect(")");
    } else if (tok == "val")       { nexttok(); expect("("); string s = str_expression(); expect(")"); n = atoi(s.c_str());
    // String comparisons: treat string expr as integer 0, but handle = and <> against string literals
    } else if (tok == "inkey$")    { nexttok(); n = (getkey(false).empty() ? 0 : 1); // 0=no key
    } else if (tok == "atn")       { nexttok(); n = (atan((double)parenexpr()) * 1000000);
    } else if (tok == "cos")       { nexttok(); n = (cos((double)parenexpr() / 1000000.0) * 1000000);
    } else if (tok == "exp")       { nexttok(); n = (exp((double)parenexpr() / 1000000.0) * 1000000);
    } else if (tok == "int")       { nexttok(); n = parenexpr(); // already integer
    } else if (tok == "log")       { nexttok(); { int v = parenexpr(); n = v > 0 ? (log((double)v) * 1000000) : 0; }
    } else if (tok == "peek")      { nexttok(); parenexpr(); n = 0; // stub: returns 0
    } else if (tok == "rnd" || tok == "irnd" ) { nexttok(); n = rnd(parenexpr());
    } else if (tok == "sgn")       { nexttok(); n = parenexpr(); n = (n > 0) - (n < 0);
    } else if (tok == "sin")       { nexttok(); n = (sin((double)parenexpr() / 1000000.0) * 1000000);
    } else if (tok == "sqr")       { nexttok(); { int v = parenexpr(); n = v >= 0 ? sqrt((double)v) : 0; }
    } else if (tok == "tan")       { nexttok(); n = (tan((double)parenexpr() / 1000000.0) * 1000000);
    } else if (tok == "usr")       { nexttok(); parenexpr(); n = 0; // stub: returns 0
    } else if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$') {
        // string variable used in numeric context — treat as its length (unusual but safe)
        n = svars[tok[0]-'a'].size(); nexttok();
    } else if (toktype == kIDENT)  { n = vars[getvarindex()]; nexttok();
    } else if (tok == "@")         { nexttok(); n = atarry[parenexpr()];
    } else if (tok == "(")         { n = parenexpr();
    } else {
        printf("(%d, %d) Syntax error: expecting an operand, found: %s toktype: %d\n", curline, textp, tok.c_str(), toktype);
        return n;
    }

    for (;;) {  // while binary operator and precedence of tok >= minprec
        if        (minprec <= 1 && tok == "or")  { nexttok(); n = (n != 0 || expression(2) != 0) ? 1.0 : 0.0;
        } else if (minprec <= 2 && tok == "and") { nexttok(); n = (n != 0 && expression(3) != 0) ? 1.0 : 0.0;
        } else if (minprec <= 4 && tok == "=")   { nexttok(); n = n == expression(5);
        } else if (minprec <= 4 && tok == "<")   { nexttok(); n = n <  expression(5);
        } else if (minprec <= 4 && tok == ">")   { nexttok(); n = n >  expression(5);
        } else if (minprec <= 4 && tok == "<>")  { nexttok(); n = n != expression(5);
        } else if (minprec <= 4 && tok == "<=")  { nexttok(); n = n <= expression(5);
        } else if (minprec <= 4 && tok == ">=")  { nexttok(); n = n >= expression(5);
        } else if (minprec <= 5 && tok == "+")   { nexttok(); n += expression(6);
        } else if (minprec <= 5 && tok == "-")   { nexttok(); n -= expression(6);
        } else if (minprec <= 6 && tok == "*")   { nexttok(); n *= expression(7);
        } else if (minprec <= 6 && tok == "/")   { nexttok(); n /= expression(7);
        } else if (minprec <= 6 && tok == "\\")  { nexttok(); n /= expression(7);
        } else if (minprec <= 6 && tok == "mod") { nexttok(); n = fmod(n, expression(7));
        } else if (minprec <= 8 && tok == "^")   { nexttok(); n = pow(n, expression(9));
        } else { break; }
    }
    return n;
}

int parenexpr(void) {
    int n = 0;

    if (!accept("(")) {
        printf("(%d, %d) Paren Expr: Expecting '(', found: %s\n", curline, textp, tok.c_str());
    } else {
        n = expression(0);
        if (!accept(")")) {
            printf("(%d, %d) Paren Expr: Expecting ')', found: %s\n", curline, textp, tok.c_str());
        }
    }
    return n;
}

int rnd(int range) {
    return rand() % range + 1;
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

void nexttok(void) {
    static string punct = "#()*+,-/:;<=>?@\\^";
    toktype = kNONE;
    begin: tok = thech; getch();
    if (tok[0] == '\0') { tok.clear();
    } else if (isspace(tok[0])) { goto begin;
    } else if (isalpha(tok[0])) { readident(); if (tok == "rem") skiptoeol();
    } else if (isdigit(tok[0])) { readint();
    } else if (tok[0] == '"')   { readstr();
    } else if (tok[0] == '\'')  { skiptoeol();
    } else if (punct.find(tok[0]) != string::npos) {
        toktype = kPUNCT;
        if ((tok[0] == '<' && (thech == '>' || thech == '=')) || (tok[0] == '>' && thech == '=')) {
            tok += thech;
            getch();
        }
    } else {
        printf("(%d, %d) What? %c (%d) %s\n", curline, textp, tok[0], tok[0], thelin.c_str());
        getch();
        errors = true;
    }
}

void skiptoeol(void) {
    tok.clear(); toktype = kNONE;
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

void readident(void) {
    tok[0] = tolower(tok[0]); toktype = kIDENT;
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

void getch(void) {
    if (thelin.empty() || textp > thelin.length()) {
        thech = '\0';
    } else {
        thech = thelin[textp - 1];
        textp++;
    }
}
