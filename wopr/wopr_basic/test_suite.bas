100 REM Test Suite for WOPR BASIC Bug Fixes
110 PRINT "=== Test 1: Simple MID$ comparison ==="
120 I$="1"
130 IF MID$(I$,1,1)="1" THEN GOTO 135
131 PRINT "FAIL: MID$ comparison failed"
132 GOTO 150
135 PRINT "PASS: MID$ comparison works"
150 REM Test 2: Multiple string functions
160 PRINT "=== Test 2: LEFT$ function ==="
170 S$="HELLO"
180 IF LEFT$(S$,1)="H" THEN GOTO 185
181 PRINT "FAIL: LEFT$ failed"
182 GOTO 200
185 PRINT "PASS: LEFT$ works"
200 PRINT "=== Test 2b: RIGHT$ function ==="
210 IF RIGHT$(S$,1)="O" THEN GOTO 215
211 PRINT "FAIL: RIGHT$ failed"
212 GOTO 230
215 PRINT "PASS: RIGHT$ works"
230 REM Test 3: IF THEN GOTO with numbers
240 PRINT "=== Test 3: IF THEN GOTO explicit ==="
250 X=1
260 IF X=1 THEN GOTO 270
261 PRINT "FAIL: IF THEN GOTO didn't work"
262 GOTO 290
270 PRINT "PASS: IF THEN GOTO works"
290 REM Test 4: Multiple conditions
300 PRINT "=== Test 4: Multiple MID$ conditions ==="
310 CH$="2"
320 IF MID$(CH$,1,1)="1" THEN GOTO 330
321 IF MID$(CH$,1,1)="2" THEN GOTO 350
330 PRINT "FAIL: Should have gone to 350"
335 GOTO 370
350 PRINT "PASS: Multiple MID$ conditions work"
370 REM Test 5: GOSUB with string comparisons
380 PRINT "=== Test 5: GOSUB with string comparisons ==="
390 GOSUB 500
400 PRINT "PASS: Returned from GOSUB successfully"
410 END
500 REM Menu subroutine
510 I$="1"
520 IF MID$(I$,1,1)="1" THEN GOTO 550
540 PRINT "FAIL: GOTO in GOSUB didn't work"
545 RETURN
550 PRINT "PASS: GOTO in GOSUB works"
560 RETURN
