/* RDLINE-	READ INPUT LINE */

/*COPYRIGHT 1980, INFOCOM COMPUTERS AND COMMUNICATIONS, CAMBRIDGE MA. 02142*/
/* ALL RIGHTS RESERVED, COMMERCIAL USAGE STRICTLY PROHIBITED */
/* WRITTEN BY R. M. SUPNIK */

#include <stdio.h>
#include <ctype.h>
#include "funcs.h"
#include "vars.h"

extern int system P((const char *));
extern char *zork_shim_fgets(char *buf, int n);
extern int g_zork_game_over;

static int lex_ P((char *, int *, int *, int));

void rdline_(char *buffer, int who)
{
    char *z, *zlast;

L5:
    /* If game is over, return immediately so the thread can unwind */
    if (g_zork_game_over) {
        buffer[0] = '\0';
        return;
    }

    switch (who + 1) {
	case 1:  goto L90;
	case 2:  goto L10;
    }
L10:
    more_output(">");
L90:
    if (zork_shim_fgets(buffer, 78) == NULL)
	exit_();
    more_input();

    /* Check again after unblocking in case game ended while waiting */
    if (g_zork_game_over) {
        buffer[0] = '\0';
        return;
    }

    if (buffer[0] == '!') {
	system(buffer + 1);
	goto L5;
    }

    zlast = buffer - 1;
    for (z = buffer; *z != '\0' && *z != '\n'; z++) {
	if (*z != ' ')
	    zlast = z;
	if (islower(*z))
		*z = toupper(*z);
    }
    z = zlast + 1;
    if (z == buffer)
	goto L5;
    *z = '\0';

    prsvec_1.prscon = 1;
} /* rdline_ */

/* PARSE-	TOP LEVEL PARSE ROUTINE */

int parse_(char *inbuf, int vbflag)
{
    int i__1;
    int ret_val;
    int outbuf[40], outlnt;

    --inbuf;

    ret_val = FALSE_;
    prsvec_1.prsa = 0;
    prsvec_1.prsi = 0;
    prsvec_1.prso = 0;

    if (! lex_(inbuf + 1, outbuf, &outlnt, vbflag)) {
	goto L100;
    }
    if ((i__1 = sparse_(outbuf, outlnt, vbflag)) < 0) {
	goto L100;
    } else if (i__1 == 0) {
	goto L200;
    } else {
	goto L300;
    }

L200:
    if (! (vbflag)) {
	goto L350;
    }
    if (! synmch_()) {
	goto L100;
    }
    if (prsvec_1.prso > 0 & prsvec_1.prso < xsrch_1.xmin) {
	last_1.lastit = prsvec_1.prso;
    }

L300:
    ret_val = TRUE_;
L350:
    orphan_(0, 0, 0, 0, 0);
    return ret_val;

L100:
    prsvec_1.prscon = 1;
    return ret_val;

} /* parse_ */

/* ORPHAN- SET UP NEW ORPHANS */

void orphan_(int o1, int o2, int o3, int o4, int o5)
{
    orphs_1.oflag = o1;
    orphs_1.oact = o2;
    orphs_1.oslot = o3;
    orphs_1.oprep = o4;
    orphs_1.oname = o5;
} /* orphan_ */

/* LEX-	LEXICAL ANALYZER */

static int lex_(char *inbuf, int *outbuf, int *op, int vbflag)
{
    static const char dlimit[9] = { 'A', 'Z', 'A' - 1,
				    '1', '9', '1' - 31,
				    '-', '-', '-' - 27 };

    int ret_val;
    int i;
    char j;
    int k, j1, j2, cp;

    --outbuf;
    --inbuf;

    for (i = 1; i <= 40; ++i) {
	outbuf[i] = 0;
    }

    ret_val = FALSE_;
    *op = -1;
L50:
    *op += 2;
    cp = 0;

L200:
    j = inbuf[prsvec_1.prscon];

    if (j == '\0')
	goto L1000;

    ++prsvec_1.prscon;

    if (j == '.') {
	goto L1000;
    }
    if (j == ',') {
	goto L1000;
    }
    if (j == ' ') {
	goto L6000;
    }
    for (i = 1; i <= 9; i += 3) {
	if (j >= dlimit[i - 1] & j <= dlimit[i]) {
	    goto L4000;
	}
    }

    if (vbflag) {
	rspeak_(601);
    }
    return ret_val;

L1000:
    if (inbuf[prsvec_1.prscon] == '\0') {
	prsvec_1.prscon = 1;
    }
    if (cp == 0 & *op == 1) {
	return ret_val;
    }
    if (cp == 0) {
	*op += -2;
    }
    ret_val = TRUE_;
    return ret_val;

L4000:
    j1 = j - dlimit[i + 1];
    if (cp >= 6) {
	goto L200;
    }
    k = *op + cp / 3;
    switch (cp % 3 + 1) {
	case 1:  goto L4100;
	case 2:  goto L4200;
	case 3:  goto L4300;
    }
L4100:
    j2 = j1 * 780;
    outbuf[k] = outbuf[k] + j2 + j2;
L4200:
    outbuf[k] += j1 * 39;
L4300:
    outbuf[k] += j1;
    ++cp;
    goto L200;

L6000:
    if (cp == 0) {
	goto L200;
    }
    goto L50;

} /* lex_ */
