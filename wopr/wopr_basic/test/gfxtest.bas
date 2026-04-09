' gfxtest.bas — Graphics protocol test for Felix BASIC
' Tests: SCREEN, CLS, LINE, CIRCLE, PAINT, PSET, PALETTE, GET/PUT
' Run with: load "gfxtest.bas" then RUN

' ============================================================
' TEST 1: Screen modes and CLS
' ============================================================
SCREEN 9          ' EGA 640x350
CLS 1             ' fill blue

LOCATE 1,1
PRINT "TEST 1: SCREEN 9, CLS 1 (blue bg)"
SLEEP 1

CLS 0             ' clear to transparent
LOCATE 1,1
PRINT "TEST 1b: CLS 0 (transparent - terminal shows through)"
SLEEP 1

' ============================================================
' TEST 2: LINE - bare lines
' ============================================================
CLS 1
LOCATE 1,1: PRINT "TEST 2: Lines"

LINE (0,   50)-(640, 50),  14      ' yellow horizontal
LINE (0,  100)-(640,100),  10      ' green horizontal
LINE (0,  150)-(640,150),  12      ' red horizontal
LINE (320,  0)-(320,350),  15      ' white vertical
LINE (0,    0)-(640,350),  11      ' cyan diagonal
LINE (640,  0)-(0,  350),   9      ' blue diagonal
SLEEP 2

' ============================================================
' TEST 3: LINE B and BF (rectangles)
' ============================================================
CLS 1
LOCATE 1,1: PRINT "TEST 3: Rectangles B and BF"

LINE (10, 20)-(200,120),  14, B    ' yellow outline box
LINE (220,20)-(430,120),   2, BF   ' green filled box
LINE (10,150)-(630,340),   4, BF   ' red filled large box
LINE (20,160)-(620,330),  15, B    ' white outline inside
SLEEP 2

' ============================================================
' TEST 4: CIRCLE
' ============================================================
CLS 1
LOCATE 1,1: PRINT "TEST 4: Circles"

CIRCLE (160, 175), 80,  15     ' white, left
CIRCLE (320, 175), 80,  14     ' yellow, center
CIRCLE (480, 175), 80,  10     ' green, right
CIRCLE (320, 175), 40,  12     ' red, small center
CIRCLE (320, 175),  5,   9     ' blue, tiny center dot
SLEEP 2

' ============================================================
' TEST 5: PAINT flood fill
' ============================================================
CLS 1
LOCATE 1,1: PRINT "TEST 5: PAINT flood fill"

CIRCLE (160, 175), 60, 15          ' white circle
PAINT  (160, 175), 14, 15          ' fill yellow inside white border

CIRCLE (320, 175), 60, 12          ' red circle
PAINT  (320, 175), 10, 12          ' fill green inside red border

LINE (420, 100)-(580, 260), 11, B  ' cyan box
PAINT (500, 180), 9, 11            ' fill blue inside cyan border
SLEEP 2

' ============================================================
' TEST 6: PSET
' ============================================================
CLS 1
LOCATE 1,1: PRINT "TEST 6: PSET (pixel scatter)"

FOR i = 0 TO 639 STEP 2
    PSET (i, 175), 15              ' white horizontal line of dots
NEXT i

FOR i = 0 TO 349 STEP 2
    PSET (320, i), 14              ' yellow vertical line of dots
NEXT i

' Draw a simple diagonal pattern
FOR i = 0 TO 300
    PSET (i, i), 10                ' green diagonal
    PSET (639 - i, i), 12          ' red diagonal
NEXT i
SLEEP 2

' ============================================================
' TEST 7: PALETTE
' ============================================================
CLS 1
LOCATE 1,1: PRINT "TEST 7: PALETTE color remapping"

' Draw color bars using colors 0-15
FOR i = 0 TO 15
    x1 = i * 40
    x2 = x1 + 39
    LINE (x1, 60)-(x2, 200), i, BF
    LOCATE 14, i*5+1: PRINT i
NEXT i

SLEEP 1

' Remap color 4 (red) to bright orange
PALETTE 4, 255, 128, 0
SLEEP 1
LOCATE 22,1: PRINT "Color 4 remapped to orange"
SLEEP 1

' Remap color 2 (green) to purple
PALETTE 2, 180, 0, 255
SLEEP 1
LOCATE 23,1: PRINT "Color 2 remapped to purple"
SLEEP 2

' Restore
PALETTE 4, 170, 0, 0
PALETTE 2, 0, 170, 0

' ============================================================
' TEST 8: GET / PUT sprites
' ============================================================
CLS 1
LOCATE 1,1: PRINT "TEST 8: GET/PUT sprites"

' Draw a small shape to capture
LINE (280,150)-(360,200), 14, BF   ' yellow rect
CIRCLE (320,175), 20, 12           ' red circle on top

' Capture it as sprite 1
DIM spr&(100)
GET (280,150)-(360,200), spr&

' Erase original
LINE (280,150)-(360,200), 1, BF

' PUT it at several locations
PUT (50,  50), spr&, PSET
PUT (200, 50), spr&, PSET
PUT (400, 50), spr&, PSET
PUT (50, 250), spr&, PSET
PUT (500,250), spr&, PSET
SLEEP 2

' Test XOR mode - draw then erase
PUT (300,150), spr&, PSET
SLEEP 1
PUT (300,150), spr&, XOR    ' should erase
SLEEP 1

' ============================================================
' TEST 9: Mixed scene - mini cityscape
' ============================================================
CLS 1
LOCATE 1,1: PRINT "TEST 9: Mini scene"

' Sky gradient (bands)
LINE (0,  0)-(640, 60),  1, BF    ' dark blue sky top
LINE (0, 60)-(640,120),  9, BF    ' blue sky middle

' Ground
LINE (0,280)-(640,350),  2, BF    ' green ground

' Buildings
LINE (20, 180)-(120,280),  7, BF   ' gray building 1
LINE (25, 190)-(50,  210), 14, BF  ' yellow window
LINE (70, 190)-(95,  210), 14, BF  ' yellow window
LINE (25, 230)-(50,  250), 14, BF  ' yellow window
LINE (70, 230)-(95,  250), 14, BF  ' yellow window

LINE (160,140)-(280,280),  8, BF   ' dark gray building 2
LINE (170,155)-(205,185), 14, BF   ' window row 1
LINE (220,155)-(255,185), 14, BF
LINE (170,200)-(205,230), 14, BF   ' window row 2
LINE (220,200)-(255,230), 14, BF

LINE (330,160)-(500,280),  7, BF   ' building 3
LINE (345,175)-(390,205), 14, BF
LINE (405,175)-(450,205), 14, BF
LINE (345,220)-(390,250), 14, BF
LINE (405,220)-(450,250), 14, BF

' Sun
CIRCLE (580, 60), 35,  14          ' yellow sun
PAINT  (580, 60), 14, 14           ' filled

' Road
LINE (0,300)-(640,300),  8         ' road top edge
LINE (0,350)-(640,350),  8         ' road bottom

SLEEP 3

' ============================================================
' DONE
' ============================================================
CLS 0
LOCATE 10, 30
PRINT "All tests complete."
SLEEP 1
CLS 0
