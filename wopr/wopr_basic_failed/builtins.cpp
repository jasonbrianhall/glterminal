#include "wopr.h"

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
  if (filename.empty())
    return;

  ifstream infile(filename.c_str());
  if (!infile)
    return;

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
  for (int i = 0; i < c_maxlines; ++i)
    pgm[i].clear();
}

void savestmt(void) {
  string filename;

  filename = getfilename("Save");
  if (filename.empty())
    return;

  ofstream outfile(filename.c_str());
  if (!outfile)
    return;

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
  if (filename.empty())
    return filename;

  if (filename.find(".") == string::npos)
    filename += ".bas";

  return filename;
}
