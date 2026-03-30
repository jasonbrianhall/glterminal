#ifndef WOPR_H
#define WOPR_H

#include <cmath>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stack>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

using namespace std;

// Constants
enum { c_maxlines = 7000, c_at_max = 1000, c_maxvars = 26 };

// Token types
typedef enum { kNONE, kPUNCT, kIDENT, kNUMBER, kSTRING } toktype_t;

// Global lexer state
extern toktype_t toktype;
extern string tok;
extern int num;
extern unsigned textp;
extern string thelin;
extern char thech;

// Global program state
extern string pgm[c_maxlines + 1];
extern int curline;
extern bool errors, tracing, need_colon;

// Stack for gosub/return
extern stack<int> gln_stack;
extern stack<int> gtp_stack;

// Timing
extern clock_t timestart;

// Variables and arrays
extern double vars[c_maxvars + 1];
extern double atarry[c_at_max];
extern string svars[c_maxvars];

// Data statement support
extern vector<double> data_store;
extern int data_ptr;

// FOR loop state
extern double forvar[c_maxvars];
extern double forlimit[c_maxvars];
extern int forline[c_maxvars];
extern int forpos[c_maxvars];

// Utility functions
string getkey(bool blocking);
void help(void);
void clearvars(void);
void showtime(bool running);
void skiptoeol(void);
int rnd(int range);
int validlinenum(int n);

// Lexer functions
void initlex(int n);
void initlex2(void);
void nexttok(void);
void getch(void);
void readstr(void);
void readint(void);
void readident(void);

// Parser functions
bool accept(const string s);
bool expect(const string s);
double expression(int minprec);
double parenexpr(void);
string str_expression(void);

// Statement functions
void docmd(void);
void ifstmt(void);
void forstmt(void);
void nextstmt(void);
void gosubstmt(void);
void returnstmt(void);
void gotostmt(void);
void ongotostmt(void);
void inputstmt(void);
void printstmt(void);
void printusingstmt(void);
void datastmt(void);
void readstmt(void);
void restorestmt(void);
void dimstmt(void);
void pokestmt(void);
void getstmt(void);
void rassign(void);

// Assignment functions
void assign(void);
void arrassn(void);
int getvarindex(void);
int getsvarindex(void);

// Built-in statement functions
void liststmt(void);
void runstmt(void);
void loadstmt(void);
void savestmt(void);
void newstmt(void);
string getfilename(const string action);

#endif // WOPR_H
