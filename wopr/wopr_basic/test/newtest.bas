SCREEN 1
CLS

PRINT "TEST 1: LINE / CIRCLE / PAINT / PSET"

' Crosshair
LINE (150, 0)-(150, 199), 2
LINE (0, 100)-(319, 100), 2

' Box
LINE (50, 50)-(100, 100), 3, B

' Filled box
LINE (120, 50)-(170, 100), 4, BF

' Circle + paint
CIRCLE (240, 80), 25, 5
PAINT (240, 80), 5, 5

' Pixel test
PSET (10, 10), 1
IF POINT(10,10) = 1 THEN PRINT "PASS: PSET/POINT" ELSE PRINT "FAIL: PSET/POINT"

SLEEP 2

SCREEN 1
CLS

PRINT "TEST 2: PUT / GET round-trip"

DIM sprite(200)

' Draw a test pattern
LINE (50,50)-(100,100), 2, BF
CIRCLE (75,75), 20, 3

' Capture it
GET (50,50)-(100,100), sprite

' Erase original
LINE (50,50)-(100,100), 0, BF

' PUT copy
PUT (150,50), sprite, PSET

' Visual confirmation
PRINT "Left should be blank. Right should show captured sprite."

SLEEP 3

' bananatest.bas — Rotating banana sprite test for Felix BASIC
' Uses the exact same EGA banana DATA and loading technique as gorilla.bas
' Run with: load "bananatest.bas" then RUN

SCREEN 9
CLS 0

' ── Load EGA banana sprites from DATA (same values as gorilla.bas) ──
REDIM LBan&(8), DBan&(8), UBan&(8), RBan&(8)

RESTORE BanLeft
FOR i = 0 TO 8: READ LBan&(i): NEXT i

RESTORE BanDown
FOR i = 0 TO 8: READ DBan&(i): NEXT i

RESTORE BanUp
FOR i = 0 TO 8: READ UBan&(i): NEXT i

RESTORE BanRight
FOR i = 0 TO 8: READ RBan&(i): NEXT i

' ── Draw each rotation frame once as a label row ──
LOCATE 1, 1: PRINT "Banana rotation frames: Left  Down   Up   Right"

cx = 80
FOR r = 0 TO 3
    SELECT CASE r
        CASE 0: PUT (cx, 30), LBan&, PSET
        CASE 1: PUT (cx, 30), DBan&, PSET
        CASE 2: PUT (cx, 30), UBan&, PSET
        CASE 3: PUT (cx, 30), RBan&, PSET
    END SELECT
    cx = cx + 120
NEXT r

SLEEP 2

' ── Animate: spin banana at center ──
CLS 0
LOCATE 1, 1: PRINT "Spinning banana (press any key to stop)"

x = 300
y = 160
r = 0
DO
    ' Erase previous frame with XOR
    SELECT CASE r
        CASE 0: PUT (x, y), LBan&, XOR
        CASE 1: PUT (x, y), DBan&, XOR
        CASE 2: PUT (x, y), UBan&, XOR
        CASE 3: PUT (x, y), RBan&, XOR
    END SELECT

    r = (r + 1) MOD 4

    ' Draw next frame
    SELECT CASE r
        CASE 0: PUT (x, y), LBan&, PSET
        CASE 1: PUT (x, y), DBan&, PSET
        CASE 2: PUT (x, y), UBan&, PSET
        CASE 3: PUT (x, y), RBan&, PSET
    END SELECT

    ' Delay ~150ms
    FOR d = 1 TO 5000: NEXT d

LOOP WHILE INKEY$ = ""

CLS 0
LOCATE 10, 28: PRINT "Banana test complete."
SLEEP 1

' ── EGA banana DATA (from gorilla.bas) ──
BanLeft:
DATA 458758,202116096,471604224,943208448,943208448,943208448,471604224,202116096,0

BanDown:
DATA 262153,-2134835200,-2134802239,-2130771968,-2130738945,8323072,8323199,4063232,4063294

BanUp:
DATA 262153,4063232,4063294,8323072,8323199,-2130771968,-2130738945,-2134835200,-2134802239

BanRight:
DATA 458758,-1061109760,-522133504,1886416896,1886416896,1886416896,-522133504,-1061109760,0


