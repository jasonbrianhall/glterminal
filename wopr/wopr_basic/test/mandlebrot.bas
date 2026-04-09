5  ' FAST COLOR MANDELBROT FOR SCREEN 21 (INCREMENTAL, NO MOD)
9  CLS 0
10 SCREEN 21
20 W = 640: H = 480
30 MAXITER = 48
40 SKIP = 3          ' higher skip = faster, lower detail

50 DX = 3.5 / W      ' width of complex plane
60 DY = 2.4 / H      ' height of complex plane

70 CY = -1.2
80 FOR PY = 0 TO H-1 STEP SKIP
90   CX = -2.5
100  FOR PX = 0 TO W-1 STEP SKIP
110    X = 0: Y = 0: ITER = 0

120    WHILE ITER < MAXITER AND (X*X + Y*Y) < 4
130      XT = X*X - Y*Y + CX
140      Y = 2*X*Y + CY
150      X = XT
160      ITER = ITER + 1
170    WEND

180    COL = ITER
190    WHILE COL >= 16
200      COL = COL - 16
210    WEND
220    IF ITER = MAXITER THEN COL = 15

230    PSET PX, PY, COL
240    CX = CX + DX * SKIP
250  NEXT PX

260  CY = CY + DY * SKIP
270 NEXT PY

280 PRINT "DONE"

