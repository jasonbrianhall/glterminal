/*
 WOPR BASIC - A Tiny BASIC Interpreter
 Supports: end, list, load, new, run, save
 gosub/return, goto, if, input, print, multi-statement lines (:)
 a single numeric array: @(n), and rnd(n)

 Supports an interactive mode.

 Note that this is a pure interpreter, e.g., no pre token stream or intermediate
 form.

 g++ -Wall -Wextra -Wpedantic -s -Os -std=c++11 main.cpp lexer.cpp expressions.cpp statements.cpp builtins.cpp -o wopr-basic
*/

#include "wopr.h"

bool debug_mode = false;
FILE *debug_log = nullptr;

void debug_log_line(void) {
  if (!debug_mode || !debug_log) return;
  
  fprintf(debug_log, "Line %d: %s\n", curline, pgm[curline].c_str());
  fprintf(debug_log, "  Variables: ");
  for (int i = 0; i < 26; i++) {
    if (vars[i] != 0.0)
      fprintf(debug_log, "%c=%.6g ", 'a' + i, vars[i]);
  }
  fprintf(debug_log, "\n");
  fprintf(debug_log, "  Strings: ");
  for (int i = 0; i < 26; i++) {
    if (!svars[i].empty())
      fprintf(debug_log, "%c$=\"%s\" ", 'a' + i, svars[i].c_str());
  }
  fprintf(debug_log, "\n");
  fflush(debug_log);
}

void docmd(void) {
  bool running = false;
  for (;;) {
    need_colon = true;
    goto_executed = false;  // Reset flag at start of statement processing
    
    if (debug_mode && debug_log && curline > 0)
      debug_log_line();
    if (tracing && tok != ":" && !tok.empty() && textp <= thelin.length())
      printf("[%d] %s %s\n", curline, tok.c_str(),
             thelin.substr(textp - 1).c_str());
    if (tok == "bye" || tok == "quit") {
      nexttok();
      exit(0);
    } else if (tok == "end" || tok == "stop") {
      nexttok();
      showtime(running);
      return;
    } else if (tok == "chain") {
      nexttok();
      if (toktype == kSTRING) {
        string filename = tok.substr(1);  // Remove leading quote
        nexttok();
        // Optional: line number to GOTO in the new program
        if (accept(",")) {
          int target_line = (int)expression(0);
        }
        // Load and run the new program
        tok = "\"";
        tok += filename;
        toktype = kSTRING;
        newstmt();
        loadstmt();
        toktype = kIDENT;
        tok = "run";
        docmd();
        exit(0);  // Exit after chaining
      } else {
        printf("(%d, %d) CHAIN: Filename required\n", curline, textp);
      }
    } else if (tok == "clear") {
      nexttok();
      clearvars();
      return;
    } else if (tok == "help") {
      nexttok();
      help();
      return;
    } else if (tok == "list") {
      nexttok();
      liststmt();
      return;
    } else if (tok == "load") {
      nexttok();
      loadstmt();
      return;
    } else if (tok == "new") {
      nexttok();
      newstmt();
      return;
    } else if (tok == "run") {
      nexttok();
      runstmt();
      running = true;
    } else if (tok == "save") {
      nexttok();
      savestmt();
      return;
    } else if (tok == "tron") {
      nexttok();
      tracing = true;
    } else if (tok == "troff") {
      nexttok();
      tracing = false;
    } else if (tok == "data") {
      nexttok();
      datastmt();
      return;
    } else if (tok == "dim") {
      nexttok();
      dimstmt();
    } else if (tok == "poke") {
      nexttok();
      pokestmt();
    } else if (tok == "read") {
      nexttok();
      readstmt();
    } else if (tok == "restore") {
      nexttok();
      restorestmt();
      return;
    } else if (tok == "cls") {
      nexttok();
      term_cls();
    } else if (tok == "get") {
      nexttok();
      getstmt();
    } else if (tok == "on") {
      nexttok();
      ongotostmt();
    } else if (tok == "call") {
      nexttok();
      expression(0); // stub
    } else if (tok == "himem") {
      nexttok();
      accept(":");
      expression(0); // stub
    } else if (tok == "lomem") {
      nexttok();
      accept(":");
      expression(0); // stub
    } else if (tok == "color") {
      nexttok();
      double fg = expression(0);
      double bg = 0, border = 0;
      if (accept(",")) {
        bg = expression(0);
        if (accept(","))
          border = expression(0);
      }
      term_color((int)fg, (int)bg);
    } else if (tok == "locate") {
      nexttok();
      double row = expression(0);
      double col = 0;
      double cursor = 1;
      if (accept(",")) {
        col = expression(0);
        if (accept(","))
          cursor = expression(0);
      }
      term_locate((int)row, (int)col);
    } else if (tok == "screen") {
      nexttok();
      double mode = expression(0);
      double colorswitch = 0;
      if (accept(","))
        colorswitch = expression(0);
      term_screen((int)mode);
    } else if (tok == "width") {
      nexttok();
      expression(0); // stub
    } else if (tok == "key") {
      nexttok();
      accept("off");
      accept("on"); // stub
    } else if (tok == "def") {
      nexttok(); // DEF SEG [= expr] or DEF FN — skip
      if (accept("seg")) {
        if (accept("="))
          expression(0);
      } else
        skiptoeol(); // DEF FN etc
    } else if (tok == "defdbl" || tok == "defsng" || tok == "defint" ||
               tok == "defstr") {
      nexttok();
      skiptoeol(); // type decls — stub
    } else if (tok == "line") {
      nexttok(); // LINE INPUT "prompt";var$
      if (accept("input")) {
        lineinputstmt();
      } else {
        skiptoeol(); // Unknown LINE command
      }
    } else if (tok == "print") {
      nexttok();
      if (tok == "using") {
        nexttok();
        printusingstmt();
      } else
        printstmt();
    } else if (tok == "?") {
      nexttok();
      printstmt();
    } else if (tok == "for") {
      nexttok();
      forstmt();
    } else if (tok == "gosub") {
      nexttok();
      gosubstmt();
    } else if (tok == "goto") {
      nexttok();
      gotostmt();
    } else if (tok == "if") {
      nexttok();
      ifstmt();
    } else if (tok == "input") {
      nexttok();
      inputstmt();
    } else if (tok == "next") {
      nexttok();
      nextstmt();
    } else if (tok == "let") {
      nexttok();
      if (toktype == kIDENT && tok.size() >= 2 && tok.back() == '$')
        rassign();
      else
        assign();
    } else if (tok == "return") {
      nexttok();
      returnstmt();
    } else if (tok == "@") {
      nexttok();
      arrassn();
    } else if (toktype == kIDENT) {
      if (tok.size() >= 2 && tok.back() == '$')
        rassign();
      else
        assign();
    } else if (tok == ":" || tok.empty()) { /* handled below */
    } else {
      printf("(%d, %d) Unknown token %s: %s\n", curline, textp, tok.c_str(),
             pgm[curline].c_str());
      errors = true;
    }

    if (errors)
      return;
    
    // If GOTO or GOSUB was executed, break out of this line and process the new line
    if (goto_executed) {
      continue;  // Go back to start of main loop with new line
    }
    
    if (tok.empty()) {
      while (tok.empty()) {
        if (curline == 0 || curline >= c_maxlines) {
          showtime(running);
          return;
        }
        initlex(curline + 1);
      }
    } else if (tok == ":") {
      nexttok();
    } else if (need_colon && !expect(":")) {
      return;
    }
  }
}

int main(int argc, char *argv[]) {
  srand((unsigned)time(NULL));
  
  // Check for -D debug flag
  int arg_start = 1;
  if (argc > 1 && string(argv[1]) == "-D") {
    debug_mode = true;
    debug_log = fopen("debug.log", "w");
    if (!debug_log) {
      fprintf(stderr, "Error: Could not open debug.log for writing\n");
      return 1;
    }
    arg_start = 2;
  }
  
  if (argc > arg_start) {
    toktype = kSTRING;
    tok = "\"";
    tok += argv[arg_start];
    loadstmt();
    toktype = kIDENT;
    tok = "run";
    docmd();
    if (debug_log)
      fclose(debug_log);
    return 0;  // Exit after running the file
  } else {
    newstmt();
    help();
  }
  for (;;) {
    errors = false;
    printf("> ");
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
  
  if (debug_log)
    fclose(debug_log);
}
