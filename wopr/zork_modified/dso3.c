/* FINDXT- FIND EXIT FROM ROOM */

/*COPYRIGHT 1980, INFOCOM COMPUTERS AND COMMUNICATIONS, CAMBRIDGE MA. 02142*/
/* ALL RIGHTS RESERVED, COMMERCIAL USAGE STRICTLY PROHIBITED */
/* WRITTEN BY R. M. SUPNIK */

#include <stdio.h>
#include "../zork/funcs.h"
#include "../zork/vars.h"

extern char *zork_shim_fgets(char *buf, int n);

int findxt_(int dir, int rm)
{
    int ret_val;
    int i, xi;
    int xxxflg;

    ret_val = TRUE_;
    xi = rooms_1.rexit[rm - 1];
    if (xi == 0) {
	goto L1000;
    }

L100:
    i = exits_1.travel[xi - 1];
    curxt_1.xroom1 = i & xpars_1.xrmask;
    xxxflg = ~ xpars_1.xlflag & 65535;
    curxt_1.xtype = ((i & xxxflg) / xpars_1.xfshft & xpars_1.xfmask) + 1;
    switch (curxt_1.xtype) {
	case 1:  goto L110;
	case 2:  goto L120;
	case 3:  goto L130;
	case 4:  goto L130;
    }
    bug_(10, curxt_1.xtype);

L130:
    curxt_1.xobj = exits_1.travel[xi + 1] & xpars_1.xrmask;
    curxt_1.xactio = exits_1.travel[xi + 1] / xpars_1.xashft;
L120:
    curxt_1.xstrng = exits_1.travel[xi];
L110:
    xi += xpars_1.xelnt[curxt_1.xtype - 1];
    if ((i & xpars_1.xdmask) == dir) {
	return ret_val;
    }
    if ((i & xpars_1.xlflag) == 0) {
	goto L100;
    }
L1000:
    ret_val = FALSE_;
    return ret_val;
} /* findxt_ */

/* FWIM- FIND WHAT I MEAN */

int fwim_(int f1, int f2, int rm, int con, int adv, int nocare)
{
    int ret_val, i__1, i__2;
    int i, j;

    ret_val = 0;
    i__1 = objcts_1.olnt;
    for (i = 1; i <= i__1; ++i) {
	if ((rm == 0 || objcts_1.oroom[i - 1] != rm) && (adv == 0 ||
		objcts_1.oadv[i - 1] != adv) && (con == 0 || objcts_1.ocan[
		i - 1] != con)) {
	    goto L1000;
	}
	if ((objcts_1.oflag1[i - 1] & VISIBT) == 0) {
	    goto L1000;
	}
	if (~ (nocare) & (objcts_1.oflag1[i - 1] & TAKEBT) == 0 || (
		objcts_1.oflag1[i - 1] & f1) == 0 && (objcts_1.oflag2[i - 1]
		& f2) == 0) {
	    goto L500;
	}
	if (ret_val == 0) {
	    goto L400;
	}
	ret_val = -ret_val;
	return ret_val;

L400:
	ret_val = i;

L500:
	if ((objcts_1.oflag2[i - 1] & OPENBT) == 0) {
	    goto L1000;
	}
	i__2 = objcts_1.olnt;
	for (j = 1; j <= i__2; ++j) {
	    if (objcts_1.ocan[j - 1] != i || (objcts_1.oflag1[j - 1] &
		    VISIBT) == 0 || (objcts_1.oflag1[j - 1] & f1) ==
		     0 && (objcts_1.oflag2[j - 1] & f2) == 0) {
		goto L700;
	    }
	    if (ret_val == 0) {
		goto L600;
	    }
	    ret_val = -ret_val;
	    return ret_val;

L600:
	    ret_val = j;
L700:
	    ;
	}
L1000:
	;
    }
    return ret_val;
} /* fwim_ */

/* YESNO- OBTAIN YES/NO ANSWER */

int yesno_(int q, int y, int n)
{
    int ret_val;
    char ans[100];

L100:
    rspeak_(q);
    zork_shim_fgets(ans, sizeof ans);
    more_input();
    if (*ans == 'Y' || *ans == 'y') {
	goto L200;
    }
    if (*ans == 'N' || *ans == 'n') {
	goto L300;
    }
    rspeak_(6);
    goto L100;

L200:
    ret_val = TRUE_;
    rspeak_(y);
    return ret_val;

L300:
    ret_val = FALSE_;
    rspeak_(n);
    return ret_val;

} /* yesno_ */
