1010 COLOR 4, 0
1020 a$ = "*    *    *    *    *    *    *    *    *    *    *    *    *    *    *    *    *    "

1030 '--- Center text ---
1040 LOCATE 12, 1
1050 PRINT SPACE$((80 - LEN("Press any key to continue")) \ 2);
1060 PRINT "Press any key to continue";

1070 '--- Bottom text ---
1080 LOCATE 24, 1
1090 PRINT SPACE$((80 - LEN("QBasic Gorillas")) \ 2);
1100 PRINT "QBasic Gorillas";

1110 WHILE INKEY$ <> "": WEND

1120 WHILE INKEY$ = ""
1130   FOR a = 1 TO 5

1140     '--- Top border ---
1150     LOCATE 1, 1
1160     PRINT MID$(a$, a, 80);

1170     '--- Bottom border ---
1180     LOCATE 22, 1
1190     PRINT MID$(a$, 6 - a, 80);

1200     '--- Vertical borders ---
1210     FOR b = 2 TO 21
1220       c = (a + b) MOD 5
1230       IF c = 1 THEN
1240         LOCATE b, 80: PRINT "*";
1250         LOCATE 23 - b, 1: PRINT "*";
1260       ELSE
1270         LOCATE b, 80: PRINT " ";
1280         LOCATE 23 - b, 1: PRINT " ";
1290       END IF
1300     NEXT b

1310   NEXT a
1320 WEND

