' ============================================
' BASIC Expression + Control Flow Test Suite
' ============================================

CLS
PRINT "=== BASIC EXPRESSION TEST SUITE ==="
PRINT

' --------------------------------------------
PRINT "1. Arithmetic"
PRINT 1+2, "expected 3"
PRINT 10-3*2, "expected 4"
PRINT (10-3)*2, "expected 14"
PRINT 2^3, "expected 8"
PRINT 5/2, "expected 2.5"
PRINT 5\2, "expected 2"
PRINT

' --------------------------------------------
PRINT "2. Comparisons"
PRINT 5=5, "expected -1"
PRINT 5<>5, "expected 0"
PRINT 3<5, "expected -1"
PRINT 7>9, "expected 0"
PRINT 5<=5, "expected -1"
PRINT 6>=7, "expected 0"
PRINT

' --------------------------------------------
PRINT "3. Boolean Logic"
PRINT -1 AND -1, "expected -1"
PRINT -1 AND 0, "expected 0"
PRINT -1 OR 0, "expected -1"
PRINT 0 OR 0, "expected 0"
PRINT -1 XOR -1, "expected 0"
PRINT -1 XOR 0, "expected -1"
PRINT

' --------------------------------------------
PRINT "4. Combined Comparisons + Boolean"
PRINT 5<10 AND 10<20, "expected -1"
PRINT 5<10 AND 10>20, "expected 0"
PRINT 5<10 OR 10>20, "expected -1"
PRINT 5=5 AND 6=7 OR 1=1, "expected -1"
PRINT 5=5 AND (6=7 OR 1=1), "expected -1"
PRINT (5=5 AND 6=7) OR 1=1, "expected -1"
PRINT

' --------------------------------------------
PRINT "5. NOT operator"
PRINT NOT 0, "expected -1"
PRINT NOT -1, "expected 0"
PRINT NOT (5=5), "expected 0"
PRINT NOT (5<>5), "expected -1"
PRINT

' --------------------------------------------
PRINT "6. VAL / ASC edge cases"
PRINT VAL("12"), "expected 12"
PRINT VAL("0"), "expected 0"
PRINT VAL("ABC"), "expected 0"
PRINT ASC("A"), "expected 65"
PRINT ASC(""), "expected 0"
PRINT ASC("12"), "expected 49"
PRINT

' --------------------------------------------
PRINT "7. INT / FIX / CINT"
PRINT INT(5.9), "expected 5"
PRINT INT(-5.9), "expected -6"
PRINT FIX(5.9), "expected 5"
PRINT FIX(-5.9), "expected -5"
PRINT CINT(5.4), "expected 5"
PRINT CINT(5.5), "expected 6"
PRINT

' --------------------------------------------
PRINT "8. WHILE/WEND"
counter = 1
WHILE counter <= 5
    PRINT "counter="; counter
    counter = counter + 1
WEND
PRINT "expected: 1 2 3 4 5"
PRINT

' --------------------------------------------
PRINT "9. IF/THEN"
IF 5<10 THEN PRINT "IF OK (expected)"
IF 5>10 THEN PRINT "IF FAIL (should not print)"
IF 5<10 AND 10<20 THEN PRINT "IF AND OK (expected)"
IF 5<10 AND 10>20 THEN PRINT "IF AND FAIL (should not print)"
PRINT

' --------------------------------------------
PRINT "10. Nested expressions"
PRINT (1+(2*3))=7, "expected -1"
PRINT (10/(2+3))=2, "expected -1"
PRINT (5<10) AND (10<20), "expected -1"
PRINT

' --------------------------------------------
PRINT "11. String comparisons"
PRINT "A"="A", "expected -1"
PRINT "A"<"B", "expected -1"
PRINT "B"<"A", "expected 0"
PRINT "HELLO"="WORLD", "expected 0"
PRINT

PRINT "=== END OF TEST SUITE ==="
END

