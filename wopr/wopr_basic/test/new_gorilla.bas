1 DEFINT A-Z
2 TYPE XYPoint
3 XCoor AS INTEGER
4 YCoor AS INTEGER
5 CONST SPEEDCONST = 500
6 CONST TRUE = -1
7 CONST FALSE = NOT TRUE
8 CONST HITSELF = 1
9 CONST BACKATTR = 0
10 CONST OBJECTCOLOR = 1
11 CONST WINDOWCOLOR = 14
12 CONST SUNATTR = 3
13 CONST SUNHAPPY = FALSE
14 CONST SUNSHOCK = TRUE
15 CONST RIGHTUP = 1
16 CONST LEFTUP = 2
17 CONST ARMSDOWN = 3
18 DIM SHARED GorillaX(1 TO 2)  'Location of the two gorillas
19 DIM SHARED GorillaY(1 TO 2)
20 DIM SHARED LastBuilding
21 DIM SHARED pi#
22 DIM SHARED LBan&(x), RBan&(x), UBan&(x), DBan&(x) 'Graphical picture of banana
23 DIM SHARED GorD&(120)        'Graphical picture of Gorilla arms down
24 DIM SHARED GorL&(120)        'Gorilla left arm raised
25 DIM SHARED GorR&(120)        'Gorilla right arm raised
26 DIM SHARED gravity#
27 DIM SHARED Wind
28 DIM SHARED ScrHeight
29 DIM SHARED ScrWidth
30 DIM SHARED Mode
31 DIM SHARED MaxCol
32 DIM SHARED ExplosionColor
33 DIM SHARED SunColor
34 DIM SHARED BackColor
35 DIM SHARED SunHit
36 DIM SHARED SunHt
37 DIM SHARED GHeight
38 DIM SHARED MachSpeed AS SINGLE
39 DEF FnRan (x) = INT(RND(1) * x) + 1
40 DEF SEG = 0                         ' Set NumLock to ON
41 KeyFlags = PEEK(1047)
42 IF (KeyFlags AND 32) = 0 THEN
43 POKE 1047, KeyFlags OR 32
44 END IF
45 DEF SEG
46 GOSUB InitVars
47 Intro
48 GetInputs Name1$, Name2$, NumGames
49 GorillaIntro Name1$, Name2$
50 PlayGame Name1$, Name2$, NumGames
51 LOCATE 11, 24
52 COLOR 5
53 PRINT "Would you like to play again?"
54 COLOR 7
55 a = 1
56 DO
57 again$ = INKEY$
58 LOOP UNTIL (again$ = "y") OR (again$ = "n")
59 CLS
60 IF again$ = "y" THEN GOTO spam
61 DEF SEG = 0                         ' Restore NumLock state
62 POKE 1047, KeyFlags
63 DEF SEG
64 END
65 DATA 327686, -252645316, 60
66 DATA 196618, -1057030081, 49344
67 DATA 196618, -1056980800, 63
68 DATA 327686,  1010580720, 240
69 DATA 458758,202116096,471604224,943208448,943208448,943208448,471604224,202116096,0
70 DATA 262153, -2134835200, -2134802239, -2130771968, -2130738945,8323072, 8323199, 4063232, 4063294
71 DATA 262153, 4063232, 4063294, 8323072, 8323199, -2130771968, -2130738945, -2134835200,-2134802239
72 DATA 458758, -1061109760, -522133504, 1886416896, 1886416896, 1886416896,-522133504,-1061109760,0
73 pi# = 4 * ATN(1#)
74 ON ERROR GOTO ScreenModeError
75 Mode = 9
76 SCREEN Mode
77 ON ERROR GOTO PaletteError
78 IF Mode = 9 THEN PALETTE 4, 0   'Check for 64K EGA
79 ON ERROR GOTO 0
80 MachSpeed = CalcDelay
81 IF Mode = 9 THEN
82 ScrWidth = 640
83 ScrHeight = 350
84 GHeight = 25
85 RESTORE EGABanana
86 REDIM LBan&(8), RBan&(8), UBan&(8), DBan&(8)
87 FOR i = 0 TO 8
88 READ LBan&(i)
89 NEXT i
90 FOR i = 0 TO 8
91 READ DBan&(i)
92 NEXT i
93 FOR i = 0 TO 8
94 READ UBan&(i)
95 NEXT i
96 FOR i = 0 TO 8
97 READ RBan&(i)
98 NEXT i
99 SunHt = 39
100 ELSE
101 ScrWidth = 320
102 ScrHeight = 200
103 GHeight = 12
104 RESTORE CGABanana
105 REDIM LBan&(2), RBan&(2), UBan&(2), DBan&(2)
106 REDIM GorL&(20), GorD&(20), GorR&(20)
107 FOR i = 0 TO 2
108 READ LBan&(i)
109 NEXT i
110 FOR i = 0 TO 2
111 READ DBan&(i)
112 NEXT i
113 FOR i = 0 TO 2
114 READ UBan&(i)
115 NEXT i
116 FOR i = 0 TO 2
117 READ RBan&(i)
118 NEXT i
119 MachSpeed = MachSpeed * 1.3
120 SunHt = 20
121 END IF
122 RETURN
123 IF Mode = 1 THEN
124 CLS
125 LOCATE 10, 5
126 PRINT "Sorry, you must have CGA, EGA color, or VGA graphics to play GORILLA.BAS"
127 END
128 ELSE
129 Mode = 1
130 RESUME
131 END IF
132 Mode = 1            '64K EGA cards will run in CGA mode.
133 RESUME NEXT
134 REM $STATIC
135 FUNCTION CalcDelay!
136 s! = TIMER
137 DO
138 i! = i! + 1
139 LOOP UNTIL TIMER - s! >= .5
140 CalcDelay! = i!
141 END FUNCTION
142 SUB Center (Row, Text$)
143 Col = MaxCol \ 2
144 LOCATE Row, Col - (LEN(Text$) / 2 + .5)
145 PRINT Text$;
146 END SUB
147 SUB DoExplosion (x#, y#)
148 PLAY "MBO0L32EFGEFDC"
149 Radius = ScrHeight / 50
150 IF Mode = 9 THEN Inc# = .5 ELSE Inc# = .41
151 FOR c# = 0 TO Radius STEP Inc#
152 CIRCLE (x#, y#), c#, ExplosionColor
153 NEXT c#
154 FOR c# = Radius TO 0 STEP (-1 * Inc#)
155 CIRCLE (x#, y#), c#, BACKATTR
156 FOR i = 1 TO 100
157 NEXT i
158 Rest .005
159 NEXT c#
160 END SUB
161 FUNCTION DoShot (PlayerNum, x, y)
162 IF PlayerNum = 1 THEN
163 LocateCol = 1
164 ELSE
165 IF Mode = 9 THEN
166 LocateCol = 66
167 ELSE
168 LocateCol = 26
169 END IF
170 END IF
171 LOCATE 2, LocateCol
172 PRINT "Angle:";
173 Angle# = GetNum#(2, LocateCol + 7)
174 LOCATE 3, LocateCol
175 PRINT "Velocity:";
176 Velocity = GetNum#(3, LocateCol + 10)
177 IF PlayerNum = 2 THEN
178 Angle# = 180 - Angle#
179 END IF
180 FOR i = 1 TO 4
181 LOCATE i, 1
182 PRINT SPACE$(30 \ (80 \ MaxCol));
183 LOCATE i, (50 \ (80 \ MaxCol))
184 PRINT SPACE$(30 \ (80 \ MaxCol));
185 NEXT
186 SunHit = FALSE
187 PlayerHit = PlotShot(x, y, Angle#, Velocity, PlayerNum)
188 IF PlayerHit = 0 THEN
189 DoShot = FALSE
190 ELSE
191 DoShot = TRUE
192 IF PlayerHit = PlayerNum THEN PlayerNum = 3 - PlayerNum
193 VictoryDance PlayerNum
194 END IF
195 END FUNCTION
196 SUB DoSun (Mouth)
197 x = ScrWidth \ 2
198 y = Scl(25)
199 LINE (x - Scl(22), y - Scl(18))-(x + Scl(22), y + Scl(18)), BACKATTR, BF
200 CIRCLE (x, y), Scl(12), SUNATTR
201 PAINT (x, y), SUNATTR
202 LINE (x - Scl(20), y)-(x + Scl(20), y), SUNATTR
203 LINE (x, y - Scl(15))-(x, y + Scl(15)), SUNATTR
204 LINE (x - Scl(15), y - Scl(10))-(x + Scl(15), y + Scl(10)), SUNATTR
205 LINE (x - Scl(15), y + Scl(10))-(x + Scl(15), y - Scl(10)), SUNATTR
206 LINE (x - Scl(8), y - Scl(13))-(x + Scl(8), y + Scl(13)), SUNATTR
207 LINE (x - Scl(8), y + Scl(13))-(x + Scl(8), y - Scl(13)), SUNATTR
208 LINE (x - Scl(18), y - Scl(5))-(x + Scl(18), y + Scl(5)), SUNATTR
209 LINE (x - Scl(18), y + Scl(5))-(x + Scl(18), y - Scl(5)), SUNATTR
210 IF Mouth THEN  'draw "o" mouth
211 CIRCLE (x, y + Scl(5)), Scl(2.9), 0
212 PAINT (x, y + Scl(5)), 0, 0
213 ELSE           'draw smile
214 CIRCLE (x, y), Scl(8), 0, (210 * pi# / 180), (330 * pi# / 180)
215 END IF
216 CIRCLE (x - 3, y - 2), 1, 0
217 CIRCLE (x + 3, y - 2), 1, 0
218 PSET (x - 3, y - 2), 0
219 PSET (x + 3, y - 2), 0
220 END SUB
221 SUB DrawBan (xc#, yc#, r, bc)
222 SELECT CASE r
223 CASE 0
224 IF bc THEN PUT (xc#, yc#), LBan&, PSET ELSE PUT (xc#, yc#), LBan&, XOR
225 CASE 1
226 IF bc THEN PUT (xc#, yc#), UBan&, PSET ELSE PUT (xc#, yc#), UBan&, XOR
227 CASE 2
228 IF bc THEN PUT (xc#, yc#), DBan&, PSET ELSE PUT (xc#, yc#), DBan&, XOR
229 CASE 3
230 IF bc THEN PUT (xc#, yc#), RBan&, PSET ELSE PUT (xc#, yc#), RBan&, XOR
231 END SELECT
232 END SUB
233 SUB DrawGorilla (x, y, arms)
234 DIM i AS SINGLE   ' Local index must be single precision
235 LINE (x - Scl(4), y)-(x + Scl(2.9), y + Scl(6)), OBJECTCOLOR, BF
236 LINE (x - Scl(5), y + Scl(2))-(x + Scl(4), y + Scl(4)), OBJECTCOLOR, BF
237 LINE (x - Scl(3), y + Scl(2))-(x + Scl(2), y + Scl(2)), 0
238 IF Mode = 9 THEN
239 FOR i = -2 TO -1
240 PSET (x + i, y + 4), 0
241 PSET (x + i + 3, y + 4), 0
242 NEXT i
243 END IF
244 LINE (x - Scl(3), y + Scl(7))-(x + Scl(2), y + Scl(7)), OBJECTCOLOR
245 LINE (x - Scl(8), y + Scl(8))-(x + Scl(6.9), y + Scl(14)), OBJECTCOLOR, BF
246 LINE (x - Scl(6), y + Scl(15))-(x + Scl(4.9), y + Scl(20)), OBJECTCOLOR, BF
247 FOR i = 0 TO 4
248 CIRCLE (x + Scl(i), y + Scl(25)), Scl(10), OBJECTCOLOR, 3 * pi# / 4, 9 * pi# / 8
249 CIRCLE (x + Scl(-6) + Scl(i - .1), y + Scl(25)), Scl(10), OBJECTCOLOR, 15 * pi# / 8, pi# / 4
250 NEXT
251 CIRCLE (x - Scl(4.9), y + Scl(10)), Scl(4.9), 0, 3 * pi# / 2, 0
252 CIRCLE (x + Scl(4.9), y + Scl(10)), Scl(4.9), 0, pi#, 3 * pi# / 2
253 FOR i = -5 TO -1
254 SELECT CASE arms
255 CASE 1
256 CIRCLE (x + Scl(i - .1), y + Scl(14)), Scl(9), OBJECTCOLOR, 3 * pi# / 4, 5 * pi# / 4
257 CIRCLE (x + Scl(4.9) + Scl(i), y + Scl(4)), Scl(9), OBJECTCOLOR, 7 * pi# / 4, pi# / 4
258 GET (x - Scl(15), y - Scl(1))-(x + Scl(14), y + Scl(28)), GorR&
259 CASE 2
260 CIRCLE (x + Scl(i - .1), y + Scl(4)), Scl(9), OBJECTCOLOR, 3 * pi# / 4, 5 * pi# / 4
261 CIRCLE (x + Scl(4.9) + Scl(i), y + Scl(14)), Scl(9), OBJECTCOLOR, 7 * pi# / 4, pi# / 4
262 GET (x - Scl(15), y - Scl(1))-(x + Scl(14), y + Scl(28)), GorL&
263 CASE 3
264 CIRCLE (x + Scl(i - .1), y + Scl(14)), Scl(9), OBJECTCOLOR, 3 * pi# / 4, 5 * pi# / 4
265 CIRCLE (x + Scl(4.9) + Scl(i), y + Scl(14)), Scl(9), OBJECTCOLOR, 7 * pi# / 4, pi# / 4
266 GET (x - Scl(15), y - Scl(1))-(x + Scl(14), y + Scl(28)), GorD&
267 END SELECT
268 NEXT i
269 END SUB
270 FUNCTION ExplodeGorilla (x#, y#)
271 YAdj = Scl(12)
272 XAdj = Scl(5)
273 SclX# = ScrWidth / 320
274 SclY# = ScrHeight / 200
275 IF x# < ScrWidth / 2 THEN PlayerHit = 1 ELSE PlayerHit = 2
276 PLAY "MBO0L16EFGEFDC"
277 FOR i = 1 TO 8 * SclX#
278 CIRCLE (GorillaX(PlayerHit) + 3.5 * SclX# + XAdj, GorillaY(PlayerHit) + 7 * SclY# + YAdj), i, ExplosionColor, , , -1.57
279 LINE (GorillaX(PlayerHit) + 7 * SclX#, GorillaY(PlayerHit) + 9 * SclY# - i)-(GorillaX(PlayerHit), GorillaY(PlayerHit) + 9 * SclY# - i), ExplosionColor
280 NEXT i
281 FOR i = 1 TO 16 * SclX#
282 IF i < (8 * SclX#) THEN CIRCLE (GorillaX(PlayerHit) + 3.5 * SclX# + XAdj, GorillaY(PlayerHit) + 7 * SclY# + YAdj), (8 * SclX# + 1) - i, BACKATTR, , , -1.57
283 CIRCLE (GorillaX(PlayerHit) + 3.5 * SclX# + XAdj, GorillaY(PlayerHit) + YAdj), i, i MOD 2 + 1, , , -1.57
284 NEXT i
285 FOR i = 24 * SclX# TO 1 STEP -1
286 CIRCLE (GorillaX(PlayerHit) + 3.5 * SclX# + XAdj, GorillaY(PlayerHit) + YAdj), i, BACKATTR, , , -1.57
287 FOR Count = 1 TO 200
288 NEXT
289 NEXT i
290 ExplodeGorilla = PlayerHit
291 END FUNCTION
292 SUB GetInputs (Player1$, Player2$, NumGames)
293 COLOR 7, 0
294 CLS
295 LOCATE 8, 15
296 LINE INPUT "Name of Player 1 (Default = 'Player 1'): "; Player1$
297 IF Player1$ = "" THEN
298 Player1$ = "Player 1"
299 ELSE
300 Player1$ = LEFT$(Player1$, 10)
301 END IF
302 LOCATE 10, 15
303 LINE INPUT "Name of Player 2 (Default = 'Player 2'): "; Player2$
304 IF Player2$ = "" THEN
305 Player2$ = "Player 2"
306 ELSE
307 Player2$ = LEFT$(Player2$, 10)
308 END IF
309 DO
310 LOCATE 12, 56
311 PRINT SPACE$(25);
312 LOCATE 12, 13
313 INPUT "Play to how many total points (Default = 3)"; game$
314 NumGames = VAL(LEFT$(game$, 2))
315 LOOP UNTIL NumGames > 0 AND LEN(game$) < 3 OR LEN(game$) = 0
316 IF NumGames = 0 THEN NumGames = 3
317 DO
318 LOCATE 14, 53
319 PRINT SPACE$(28);
320 LOCATE 14, 17
321 INPUT "Gravity in Meters/Sec (Earth = 9.8)"; grav$
322 gravity# = VAL(grav$)
323 LOOP UNTIL gravity# > 0 OR LEN(grav$) = 0
324 IF gravity# = 0 THEN gravity# = 9.8
325 END SUB
326 FUNCTION GetNum# (Row, Col)
327 Result$ = ""
328 Done = FALSE
329 WHILE INKEY$ <> ""
330 WEND   'Clear keyboard buffer
331 DO WHILE NOT Done
332 LOCATE Row, Col
333 PRINT Result$; CHR$(95); "    ";
334 Kbd$ = INKEY$
335 SELECT CASE Kbd$
336 CASE "0" TO "9"
337 Result$ = Result$ + Kbd$
338 CASE "."
339 IF INSTR(Result$, ".") = 0 THEN
340 Result$ = Result$ + Kbd$
341 END IF
342 CASE CHR$(13)
343 IF VAL(Result$) > 360 THEN
344 Result$ = ""
345 ELSE
346 Done = TRUE
347 END IF
348 CASE CHR$(8)
349 IF LEN(Result$) > 0 THEN
350 Result$ = LEFT$(Result$, LEN(Result$) - 1)
351 END IF
352 CASE ELSE
353 IF LEN(Kbd$) > 0 THEN
354 BEEP
355 END IF
356 END SELECT
357 LOOP
358 LOCATE Row, Col
359 PRINT Result$; " ";
360 GetNum# = VAL(Result$)
361 END FUNCTION
362 SUB GorillaIntro (Player1$, Player2$)
363 LOCATE 16, 34
364 PRINT "--------------"
365 LOCATE 18, 34
366 PRINT "V = View Intro"
367 LOCATE 19, 34
368 PRINT "P = Play Game"
369 LOCATE 21, 35
370 PRINT "Your Choice?"
371 DO WHILE Char$ = ""
372 Char$ = INKEY$
373 LOOP
374 IF Mode = 1 THEN
375 x = 125
376 y = 100
377 ELSE
378 x = 278
379 y = 175
380 END IF
381 SCREEN Mode
382 SetScreen
383 IF Mode = 1 THEN Center 5, "Please wait while gorillas are drawn."
384 VIEW PRINT 9 TO 24
385 IF Mode = 9 THEN PALETTE OBJECTCOLOR, BackColor
386 DrawGorilla x, y, ARMSDOWN
387 CLS 2
388 DrawGorilla x, y, LEFTUP
389 CLS 2
390 DrawGorilla x, y, RIGHTUP
391 CLS 2
392 VIEW PRINT 1 TO 25
393 IF Mode = 9 THEN PALETTE OBJECTCOLOR, 46
394 IF UCASE$(Char$) = "V" THEN
395 Center 2, "Q B A S I C   G O R I L L A S"
396 Center 5, "             STARRING:               "
397 P$ = Player1$ + " AND " + Player2$
398 Center 7, P$
399 PUT (x - 13, y), GorD&, PSET
400 PUT (x + 47, y), GorD&, PSET
401 Rest 1
402 PUT (x - 13, y), GorL&, PSET
403 PUT (x + 47, y), GorR&, PSET
404 PLAY "t120o1l16b9n0baan0bn0bn0baaan0b9n0baan0b"
405 Rest .3
406 PUT (x - 13, y), GorR&, PSET
407 PUT (x + 47, y), GorL&, PSET
408 PLAY "o2l16e-9n0e-d-d-n0e-n0e-n0e-d-d-d-n0e-9n0e-d-d-n0e-"
409 Rest .3
410 PUT (x - 13, y), GorL&, PSET
411 PUT (x + 47, y), GorR&, PSET
412 PLAY "o2l16g-9n0g-een0g-n0g-n0g-eeen0g-9n0g-een0g-"
413 Rest .3
414 PUT (x - 13, y), GorR&, PSET
415 PUT (x + 47, y), GorL&, PSET
416 PLAY "o2l16b9n0baan0g-n0g-n0g-eeen0o1b9n0baan0b"
417 Rest .3
418 FOR i = 1 TO 4
419 PUT (x - 13, y), GorL&, PSET
420 PUT (x + 47, y), GorR&, PSET
421 PLAY "T160O0L32EFGEFDC"
422 Rest .1
423 PUT (x - 13, y), GorR&, PSET
424 PUT (x + 47, y), GorL&, PSET
425 PLAY "T160O0L32EFGEFDC"
426 Rest .1
427 NEXT
428 END IF
429 END SUB
430 SUB Intro
431 SCREEN 0
432 WIDTH 80, 25
433 MaxCol = 80
434 COLOR 15, 0
435 CLS
436 Center 4, "Q B a s i c    G O R I L L A S"
437 COLOR 7
438 Center 6, "Copyright (C) IBM Corporation 1991"
439 Center 8, "Your mission is to hit your opponent with the exploding"
440 Center 9, "banana by varying the angle and power of your throw, taking"
441 Center 10, "into account wind speed, gravity, and the city skyline."
442 Center 11, "The wind speed is shown by a directional arrow at the bottom"
443 Center 12, "of the playing field, its length relative to its strength."
444 Center 24, "Press any key to continue"
445 PLAY "MBT160O1L8CDEDCDL4ECC"
446 SparklePause
447 IF Mode = 1 THEN MaxCol = 40
448 END SUB
449 SUB MakeCityScape (BCoor() AS XYPoint)
450 x = 2
451 Slope = FnRan(6)
452 SELECT CASE Slope
453 CASE 1
454 NewHt = 15                 'Upward slope
455 CASE 2
456 NewHt = 130                'Downward slope
457 CASE 3 TO 5
458 NewHt = 15            '"V" slope - most common
459 CASE 6
460 NewHt = 130                'Inverted "V" slope
461 END SELECT
462 IF Mode = 9 THEN
463 BottomLine = 335                   'Bottom of building
464 HtInc = 10                         'Increase value for new height
465 DefBWidth = 37                     'Default building height
466 RandomHeight = 120                 'Random height difference
467 WWidth = 3                         'Window width
468 WHeight = 6                        'Window height
469 WDifV = 15                         'Counter for window spacing - vertical
470 WDifh = 10                         'Counter for window spacing - horizontal
471 ELSE
472 BottomLine = 190
473 HtInc = 6
474 NewHt = NewHt * 20 \ 35            'Adjust for CGA
475 DefBWidth = 18
476 RandomHeight = 54
477 WWidth = 1
478 WHeight = 2
479 WDifV = 5
480 WDifh = 4
481 END IF
482 CurBuilding = 1
483 DO
484 SELECT CASE Slope
485 CASE 1
486 NewHt = NewHt + HtInc
487 CASE 2
488 NewHt = NewHt - HtInc
489 CASE 3 TO 5
490 IF x > ScrWidth \ 2 THEN
491 NewHt = NewHt - 2 * HtInc
492 ELSE
493 NewHt = NewHt + 2 * HtInc
494 END IF
495 CASE 4
496 IF x > ScrWidth \ 2 THEN
497 NewHt = NewHt + 2 * HtInc
498 ELSE
499 NewHt = NewHt - 2 * HtInc
500 END IF
501 END SELECT
502 BWidth = FnRan(DefBWidth) + DefBWidth
503 IF x + BWidth > ScrWidth THEN BWidth = ScrWidth - x - 2
504 BHeight = FnRan(RandomHeight) + NewHt
505 IF BHeight < HtInc THEN BHeight = HtInc
506 IF BottomLine - BHeight <= MaxHeight + GHeight THEN BHeight = MaxHeight + GHeight - 5
507 BCoor(CurBuilding).XCoor = x
508 BCoor(CurBuilding).YCoor = BottomLine - BHeight
509 IF Mode = 9 THEN BuildingColor = FnRan(3) + 4 ELSE BuildingColor = 2
510 LINE (x - 1, BottomLine + 1)-(x + BWidth + 1, BottomLine - BHeight - 1), BACKGROUND, B
511 LINE (x, BottomLine)-(x + BWidth, BottomLine - BHeight), BuildingColor, BF
512 c = x + 3
513 DO
514 FOR i = BHeight - 3 TO 7 STEP -WDifV
515 IF Mode <> 9 THEN
516 WinColr = (FnRan(2) - 2) * -3
517 ELSEIF FnRan(4) = 1 THEN
518 WinColr = 8
519 ELSE
520 WinColr = WINDOWCOLOR
521 END IF
522 LINE (c, BottomLine - i)-(c + WWidth, BottomLine - i + WHeight), WinColr, BF
523 NEXT
524 c = c + WDifh
525 LOOP UNTIL c >= x + BWidth - 3
526 x = x + BWidth + 2
527 CurBuilding = CurBuilding + 1
528 LOOP UNTIL x > ScrWidth - HtInc
529 LastBuilding = CurBuilding - 1
530 Wind = FnRan(10) - 5
531 IF FnRan(3) = 1 THEN
532 IF Wind > 0 THEN
533 Wind = Wind + FnRan(10)
534 ELSE
535 Wind = Wind - FnRan(10)
536 END IF
537 END IF
538 IF Wind <> 0 THEN
539 WindLine = Wind * 3 * (ScrWidth \ 320)
540 LINE (ScrWidth \ 2, ScrHeight - 5)-(ScrWidth \ 2 + WindLine, ScrHeight - 5), ExplosionColor
541 IF Wind > 0 THEN ArrowDir = -2 ELSE ArrowDir = 2
542 LINE (ScrWidth / 2 + WindLine, ScrHeight - 5)-(ScrWidth / 2 + WindLine + ArrowDir, ScrHeight - 5 - 2), ExplosionColor
543 LINE (ScrWidth / 2 + WindLine, ScrHeight - 5)-(ScrWidth / 2 + WindLine + ArrowDir, ScrHeight - 5 + 2), ExplosionColor
544 END IF
545 END SUB
546 SUB PlaceGorillas (BCoor() AS XYPoint)
547 IF Mode = 9 THEN
548 XAdj = 14
549 YAdj = 30
550 ELSE
551 XAdj = 7
552 YAdj = 16
553 END IF
554 SclX# = ScrWidth / 320
555 SclY# = ScrHeight / 200
556 FOR i = 1 TO 2
557 IF i = 1 THEN BNum = FnRan(2) + 1 ELSE BNum = LastBuilding - FnRan(2)
558 BWidth = BCoor(BNum + 1).XCoor - BCoor(BNum).XCoor
559 GorillaX(i) = BCoor(BNum).XCoor + BWidth / 2 - XAdj
560 GorillaY(i) = BCoor(BNum).YCoor - YAdj
561 PUT (GorillaX(i), GorillaY(i)), GorD&, PSET
562 NEXT i
563 END SUB
564 SUB PlayGame (Player1$, Player2$, NumGames)
565 DIM BCoor(0 TO 30) AS XYPoint
566 DIM TotalWins(1 TO 2)
567 J = 1
568 FOR i = 1 TO NumGames
569 CLS
570 RANDOMIZE (TIMER)
571 CALL MakeCityScape(BCoor())
572 CALL PlaceGorillas(BCoor())
573 DoSun SUNHAPPY
574 Hit = FALSE
575 DO WHILE Hit = FALSE
576 J = 1 - J
577 LOCATE 1, 1
578 PRINT Player1$
579 LOCATE 1, (MaxCol - 1 - LEN(Player2$))
580 PRINT Player2$
581 Center 23, LTRIM$(STR$(TotalWins(1))) + ">Score<" + LTRIM$(STR$(TotalWins(2)))
582 Tosser = J + 1
583 Tossee = 3 - J
584 Hit = DoShot(Tosser, GorillaX(Tosser), GorillaY(Tosser))
585 IF SunHit THEN DoSun SUNHAPPY
586 IF Hit = TRUE THEN CALL UpdateScores(TotalWins(), Tosser, Hit)
587 LOOP
588 SLEEP 1
589 NEXT i
590 SCREEN 0
591 WIDTH 80, 25
592 COLOR 7, 0
593 MaxCol = 80
594 CLS
595 Center 8, "GAME OVER!"
596 Center 10, "Score:"
597 LOCATE 11, 30
598 PRINT Player1$; TAB(50); TotalWins(1)
599 LOCATE 12, 30
600 PRINT Player2$; TAB(50); TotalWins(2)
601 Center 24, "Press any key to continue"
602 SparklePause
603 COLOR 7, 0
604 CLS
605 END SUB
606 FUNCTION PlotShot (StartX, StartY, Angle#, Velocity, PlayerNum)
607 Angle# = Angle# / 180 * pi#  'Convert degree angle to radians
608 Radius = Mode MOD 7
609 InitXVel# = COS(Angle#) * Velocity
610 InitYVel# = SIN(Angle#) * Velocity
611 oldx# = StartX
612 oldy# = StartY
613 IF PlayerNum = 1 THEN
614 PUT (StartX, StartY), GorL&, PSET
615 ELSE
616 PUT (StartX, StartY), GorR&, PSET
617 END IF
618 PLAY "MBo0L32A-L64CL16BL64A+"
619 Rest .1
620 PUT (StartX, StartY), GorD&, PSET
621 adjust = Scl(4)                   'For scaling CGA
622 xedge = Scl(9) * (2 - PlayerNum)  'Find leading edge of banana for check
623 Impact = FALSE
624 ShotInSun = FALSE
625 OnScreen = TRUE
626 PlayerHit = 0
627 NeedErase = FALSE
628 StartXPos = StartX
629 StartYPos = StartY - adjust - 3
630 IF PlayerNum = 2 THEN
631 StartXPos = StartXPos + Scl(25)
632 direction = Scl(4)
633 ELSE
634 direction = Scl(-4)
635 END IF
636 IF Velocity < 2 THEN              'Shot too slow - hit self
637 x# = StartX
638 y# = StartY
639 pointval = OBJECTCOLOR
640 END IF
641 DO WHILE (NOT Impact) AND OnScreen
642 Rest .02
643 IF NeedErase THEN
644 NeedErase = FALSE
645 CALL DrawBan(oldx#, oldy#, oldrot, FALSE)
646 END IF
647 x# = StartXPos + (InitXVel# * t#) + (.5 * (Wind / 5) * t# ^ 2)
648 y# = StartYPos + ((-1 * (InitYVel# * t#)) + (.5 * gravity# * t# ^ 2)) * (ScrHeight / 350)
649 IF (x# >= ScrWidth - Scl(10)) OR (x# <= 3) OR (y# >= ScrHeight - 3) THEN
650 OnScreen = FALSE
651 END IF
652 IF OnScreen AND y# > 0 THEN
653 LookY = 0
654 LookX = Scl(8 * (2 - PlayerNum))
655 DO
656 pointval = POINT(x# + LookX, y# + LookY)
657 IF pointval = 0 THEN
658 Impact = FALSE
659 IF ShotInSun = TRUE THEN
660 IF ABS(ScrWidth \ 2 - x#) > Scl(20) OR y# > SunHt THEN ShotInSun = FALSE
661 END IF
662 ELSEIF pointval = SUNATTR AND y# < SunHt THEN
663 IF NOT SunHit THEN DoSun SUNSHOCK
664 SunHit = TRUE
665 ShotInSun = TRUE
666 ELSE
667 Impact = TRUE
668 END IF
669 LookX = LookX + direction
670 LookY = LookY + Scl(6)
671 LOOP UNTIL Impact OR LookX <> Scl(4)
672 IF NOT ShotInSun AND NOT Impact THEN
673 rot = (t# * 10) MOD 4
674 CALL DrawBan(x#, y#, rot, TRUE)
675 NeedErase = TRUE
676 END IF
677 oldx# = x#
678 oldy# = y#
679 oldrot = rot
680 END IF
681 t# = t# + .1
682 LOOP
683 IF pointval <> OBJECTCOLOR AND Impact THEN
684 CALL DoExplosion(x# + adjust, y# + adjust)
685 ELSEIF pointval = OBJECTCOLOR THEN
686 PlayerHit = ExplodeGorilla(x#, y#)
687 END IF
688 PlotShot = PlayerHit
689 END FUNCTION
690 SUB Rest (t#)
691 s# = TIMER
692 t2# = MachSpeed * t# / SPEEDCONST
693 DO
694 LOOP UNTIL TIMER - s# > t2#
695 END SUB
696 FUNCTION Scl (n!)
697 IF n! <> INT(n!) THEN
698 IF Mode = 1 THEN n! = n! - 1
699 END IF
700 IF Mode = 1 THEN
701 Scl = CINT(n! / 2 + .1)
702 ELSE
703 Scl = CINT(n!)
704 END IF
705 END FUNCTION
706 SUB SetScreen
707 IF Mode = 9 THEN
708 ExplosionColor = 2
709 BackColor = 1
710 PALETTE 0, 1
711 PALETTE 1, 46
712 PALETTE 2, 44
713 PALETTE 3, 54
714 PALETTE 5, 7
715 PALETTE 6, 4
716 PALETTE 7, 3
717 PALETTE 9, 63       'Display Color
718 ELSE
719 ExplosionColor = 2
720 BackColor = 0
721 COLOR BackColor, 2
722 END IF
723 END SUB
724 SUB SparklePause
725 COLOR 4, 0
726 a$ = "*    *    *    *    *    *    *    *    *    *    *    *    *    *    *    *    *    "
727 WHILE INKEY$ <> ""
728 WEND 'Clear keyboard buffer
729 WHILE INKEY$ = ""
730 FOR a = 1 TO 5
731 LOCATE 1, 1                             'print horizontal sparkles
732 PRINT MID$(a$, a, 80);
733 LOCATE 22, 1
734 PRINT MID$(a$, 6 - a, 80);
735 FOR b = 2 TO 21                         'Print Vertical sparkles
736 c = (a + b) MOD 5
737 IF c = 1 THEN
738 LOCATE b, 80
739 PRINT "*";
740 LOCATE 23 - b, 1
741 PRINT "*";
742 ELSE
743 LOCATE b, 80
744 PRINT " ";
745 LOCATE 23 - b, 1
746 PRINT " ";
747 END IF
748 NEXT b
749 NEXT a
750 WEND
751 END SUB
752 SUB UpdateScores (Record(), PlayerNum, Results)
753 IF Results = HITSELF THEN
754 Record(ABS(PlayerNum - 3)) = Record(ABS(PlayerNum - 3)) + 1
755 ELSE
756 Record(PlayerNum) = Record(PlayerNum) + 1
757 END IF
758 END SUB
759 SUB VictoryDance (Player)
760 FOR i# = 1 TO 4
761 PUT (GorillaX(Player), GorillaY(Player)), GorL&, PSET
762 PLAY "MFO0L32EFGEFDC"
763 Rest .2
764 PUT (GorillaX(Player), GorillaY(Player)), GorR&, PSET
765 PLAY "MFO0L32EFGEFDC"
766 Rest .2
767 NEXT
768 END SUB
