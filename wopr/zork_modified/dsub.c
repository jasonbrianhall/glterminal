/* RESIDENT SUBROUTINES FOR DUNGEON */

/*COPYRIGHT 1980, INFOCOM COMPUTERS AND COMMUNICATIONS, CAMBRIDGE MA. 02142*/
/* ALL RIGHTS RESERVED, COMMERCIAL USAGE STRICTLY PROHIBITED */
/* WRITTEN BY R. M. SUPNIK */

#include <stdio.h>
#include <string.h>
#include "../zork/funcs.h"
#include "../zork/vars.h"

#ifndef SEEK_SET
#define SEEK_SET (0)
#endif

extern FILE *dbfile;

static void rspsb2nl_ P((int, int, int, int));

/* RSPEAK-- OUTPUT RANDOM MESSAGE ROUTINE */

void rspeak_(int n)
{
    rspsb2nl_(n, 0, 0, 1);
} /* rspeak_ */

/* RSPSUB-- OUTPUT RANDOM MESSAGE WITH SUBSTITUTABLE ARGUMENT */

void rspsub_(int n, int s1)
{
    rspsb2nl_(n, s1, 0, 1);
} /* rspsub_ */

/* RSPSB2-- OUTPUT RANDOM MESSAGE WITH UP TO TWO SUBSTITUTABLE ARGUMENTS */

void rspsb2_(int n, int s1, int s2)
{
    rspsb2nl_(n, s1, s2, 1);
} /* rspsb2_ */

/* rspsb2nl_ Display a substitutable message with an optional newline */

static void rspsb2nl_(int n, int y, int z, int nl)
{
    const char *zkey = "IanLanceTaylorJr";
    long x;
    char linebuf[4096];
    int  linelen = 0;

    x = (long)n;

    if (x > 0) {
	x = rmsg_1.rtext[x - 1];
    }
    if (x == 0) {
	return;
    }
    play_1.telflg = TRUE_;

    x = ((- x) - 1) * 8;
    if (fseek(dbfile, x + (long)rmsg_1.mrloc, SEEK_SET) == EOF) {
	more_output("Error seeking database loc %d\n", x);
	exit_();
    }

    if (nl)
	more_output(NULL);

    while (1) {
	int i;

	i = getc(dbfile);
	if (i == EOF) {
	    more_output("Error reading database loc %d\n", x);
	    exit_();
	}
	i ^= zkey[x & 0xf] ^ (x & 0xff);
	x = x + 1;
	if (i == '\0') {
	    /* flush any remaining partial line */
	    if (linelen > 0) {
		linebuf[linelen] = '\0';
		more_output("%s", linebuf);
		linelen = 0;
	    }
	    break;
	} else if (i == '\n') {
	    linebuf[linelen] = '\0';
	    more_output("%s\n", linebuf);
	    linelen = 0;
	    if (nl)
		more_output(NULL);
	} else if (i == '#' && y != 0) {
	    long iloc;

	    /* flush before recursing */
	    if (linelen > 0) {
		linebuf[linelen] = '\0';
		more_output("%s", linebuf);
		linelen = 0;
	    }
	    iloc = ftell(dbfile);
	    rspsb2nl_(y, 0, 0, 0);
	    if (fseek(dbfile, iloc, SEEK_SET) == EOF) {
		more_output("Error seeking database loc %d\n", iloc);
		exit_();
	    }
	    y = z;
	    z = 0;
	} else {
	    if (linelen < (int)sizeof(linebuf) - 1)
		linebuf[linelen++] = (char)i;
	}
    }

    if (nl) {
	more_output(NULL);
    }
}

/* OBJACT-- APPLY OBJECTS FROM PARSE VECTOR */

int objact_()
{
    int ret_val;

    ret_val = TRUE_;
    if (prsvec_1.prsi == 0) {
	goto L100;
    }
    if (oappli_(objcts_1.oactio[prsvec_1.prsi - 1], 0)) {
	return ret_val;
    }

L100:
    if (prsvec_1.prso == 0) {
	goto L200;
    }
    if (oappli_(objcts_1.oactio[prsvec_1.prso - 1], 0)) {
	return ret_val;
    }

L200:
    ret_val = FALSE_;
    return ret_val;
} /* objact_ */

/* BUG-- REPORT FATAL SYSTEM ERROR */

void bug_(int a, int b)
{
    more_output(NULL);
    more_output("PROGRAM ERROR %d, PARAMETER=%d\n", a, b);

    if (debug_1.dbgflg != 0) {
	return;
    }
    exit_();
} /* bug_ */

/* NEWSTA-- SET NEW STATUS FOR OBJECT */

void newsta_(int o, int r, int rm, int cn, int ad)
{
    rspeak_(r);
    objcts_1.oroom[o - 1] = rm;
    objcts_1.ocan[o - 1] = cn;
    objcts_1.oadv[o - 1] = ad;
} /* newsta_ */

/* QHERE-- TEST FOR OBJECT IN ROOM */

int qhere_(int obj, int rm)
{
    int i__1;
    int ret_val;
    int i;

    ret_val = TRUE_;
    if (objcts_1.oroom[obj - 1] == rm) {
	return ret_val;
    }
    i__1 = oroom2_1.r2lnt;
    for (i = 1; i <= i__1; ++i) {
	if (oroom2_1.oroom2[i - 1] == obj && oroom2_1.rroom2[i - 1] == rm) {
	    return ret_val;
	}
    }
    ret_val = FALSE_;
    return ret_val;
} /* qhere_ */

/* QEMPTY-- TEST FOR OBJECT EMPTY */

int qempty_(int obj)
{
    int i__1;
    int ret_val;
    int i;

    ret_val = FALSE_;
    i__1 = objcts_1.olnt;
    for (i = 1; i <= i__1; ++i) {
	if (objcts_1.ocan[i - 1] == obj) {
	    return ret_val;
	}
    }
    ret_val = TRUE_;
    return ret_val;
} /* qempty_ */

/* JIGSUP- YOU ARE DEAD */

void jigsup_(int desc)
{
    static const int rlist[9] = { 8,6,36,35,34,4,34,6,5 };

    int i__1;
    int nonofl;
    int f;
    int i, j;

    rspeak_(desc);
    prsvec_1.prscon = 1;
    if (debug_1.dbgflg != 0) {
	return;
    }
    advs_1.avehic[play_1.winner - 1] = 0;
    if (play_1.winner == aindex_1.player) {
	goto L100;
    }
    rspsub_(432, objcts_1.odesc2[advs_1.aobj[play_1.winner - 1] - 1]);
    newsta_(advs_1.aobj[play_1.winner - 1], 0, 0, 0, 0);
    return;

L100:
    if (findex_1.endgmf) {
	goto L900;
    }

    // always exit for plopbot's purposes
    goto L1000;

    if (! yesno_(10, 9, 8)) {
	goto L1100;
    }

    i__1 = objcts_1.olnt;
    for (j = 1; j <= i__1; ++j) {
	if (qhere_(j, play_1.here)) {
	    objcts_1.oflag2[j - 1] &= ~ FITEBT;
	}
    }

    ++state_1.deaths;
    scrupd_(- 10);
    f = moveto_(rindex_1.fore1, play_1.winner);
    findex_1.egyptf = TRUE_;
    if (objcts_1.oadv[oindex_1.coffi - 1] == play_1.winner) {
	newsta_(oindex_1.coffi, 0, rindex_1.egypt, 0, 0);
    }
    objcts_1.oflag2[oindex_1.door - 1] &= ~ TCHBT;
    objcts_1.oflag1[oindex_1.robot - 1] = (objcts_1.oflag1[oindex_1.robot - 1]
	     | VISIBT) & ~ NDSCBT;
    if (objcts_1.oroom[oindex_1.lamp - 1] != 0 || objcts_1.oadv[oindex_1.lamp
	    - 1] == play_1.winner) {
	newsta_(oindex_1.lamp, 0, rindex_1.lroom, 0, 0);
    }

    i = 1;
    i__1 = objcts_1.olnt;
    for (j = 1; j <= i__1; ++j) {
	if (objcts_1.oadv[j - 1] != play_1.winner || objcts_1.otval[j - 1] !=
		0) {
	    goto L200;
	}
	++i;
	if (i > 9) {
	    goto L400;
	}
	newsta_(j, 0, rlist[i - 1], 0, 0);
L200:
	;
    }

L400:
    i = rooms_1.rlnt + 1;
    nonofl = RAIR + RWATER + RSACRD + REND;
    i__1 = objcts_1.olnt;
    for (j = 1; j <= i__1; ++j) {
	if (objcts_1.oadv[j - 1] != play_1.winner || objcts_1.otval[j - 1] ==
		0) {
	    goto L300;
	}
L250:
	--i;
	if ((rooms_1.rflag[i - 1] & nonofl) != 0) {
	    goto L250;
	}
	newsta_(j, 0, i, 0, 0);
L300:
	;
    }

    i__1 = objcts_1.olnt;
    for (j = 1; j <= i__1; ++j) {
	if (objcts_1.oadv[j - 1] != play_1.winner) {
	    goto L500;
	}
L450:
	--i;
	if ((rooms_1.rflag[i - 1] & nonofl) != 0) {
	    goto L450;
	}
	newsta_(j, 0, i, 0, 0);
L500:
	;
    }
    return;

L900:
    rspeak_(625);
    goto L1100;

L1000:
    rspeak_(7);
L1100:
    score_(0);
    (void) fclose(dbfile);
    exit_();

} /* jigsup_ */

/* OACTOR- GET ACTOR ASSOCIATED WITH OBJECT */

int oactor_(int obj)
{
    int ret_val = 1, i__1;
    int i;

    i__1 = advs_1.alnt;
    for (i = 1; i <= i__1; ++i) {
	ret_val = i;
	if (advs_1.aobj[i - 1] == obj) {
	    return ret_val;
	}
    }
    bug_(40, obj);
    return ret_val;
} /* oactor_ */

/* PROB- COMPUTE PROBABILITY */

int prob_(int g, int b)
{
    int ret_val;
    int i;

    i = g;
    if (findex_1.badlkf) {
	i = b;
    }
    ret_val = rnd_(100) < i;
    return ret_val;
} /* prob_ */

/* RMDESC-- PRINT ROOM DESCRIPTION */

int rmdesc_(int full)
{
    int ret_val, L__1;
    int i, ra;

    ret_val = TRUE_;
    if (prsvec_1.prso < xsrch_1.xmin) {
	goto L50;
    }
    screen_1.fromdr = prsvec_1.prso;
    prsvec_1.prso = 0;
L50:
    if (play_1.here == advs_1.aroom[aindex_1.player - 1]) {
	goto L100;
    }
    rspeak_(2);
    prsvec_1.prsa = vindex_1.walkiw;
    return ret_val;

L100:
    if (lit_(play_1.here)) {
	goto L300;
    }
    rspeak_(430);
    ret_val = FALSE_;
    return ret_val;

L300:
    ra = rooms_1.ractio[play_1.here - 1];
    if (full == 1) {
	goto L600;
    }
    i = rooms_1.rdesc2[play_1.here - 1];
    if (full == 0 && (findex_1.superf || (rooms_1.rflag[play_1.here - 1] &
	    RSEEN) != 0 && findex_1.brieff)) {
	goto L400;
    }
    i = rooms_1.rdesc1[play_1.here - 1];
    if (i != 0 || ra == 0) {
	goto L400;
    }
    prsvec_1.prsa = vindex_1.lookw;
    if (! rappli_(ra)) {
	goto L100;
    }
    prsvec_1.prsa = vindex_1.foow;
    goto L500;

L400:
    rspeak_(i);
L500:
    if (advs_1.avehic[play_1.winner - 1] != 0) {
	rspsub_(431, objcts_1.odesc2[advs_1.avehic[play_1.winner - 1] - 1]);
    }

L600:
    if (full != 2) {
	L__1 = full != 0;
	princr_(L__1, play_1.here);
    }
    rooms_1.rflag[play_1.here - 1] |= RSEEN;
    if (full != 0 || ra == 0) {
	return ret_val;
    }
    prsvec_1.prsa = vindex_1.walkiw;
    if (! rappli_(ra)) {
	goto L100;
    }
    prsvec_1.prsa = vindex_1.foow;
    return ret_val;

} /* rmdesc_ */

/* RAPPLI- ROUTING ROUTINE FOR ROOM APPLICABLES */

int rappli_(int ri)
{
    const int newrms = 38;
    int ret_val;

    ret_val = TRUE_;
    if (ri == 0) {
	return ret_val;
    }
    if (ri < newrms) {
	ret_val = rappl1_(ri);
    }
    if (ri >= newrms) {
	ret_val = rappl2_(ri);
    }
    return ret_val;
} /* rappli_ */
