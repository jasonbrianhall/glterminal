10 pi = 0
20 sign = 1
30 FOR i = 0 TO 100000
40     term = 1 / (2 * i + 1)
50     pi = pi + sign * term
60     sign = -sign
70 NEXT i
80 pi = pi * 4
90 PRINT "Approximation of pi = "; pi
