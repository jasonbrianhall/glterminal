' This program exported from BASIC Anywhere Machine (Version [5.2.3].[2024.09.09.00.00]) on 2026.04.19 at 01:38 (Coordinated Universal Time)
' This program by Charlie Veniot is a port and mod
' of a BazzBasic program by EkBass and shared by
' EkBass on the GotBASIC discord server
' (https://discord.com/channels/682603735515529216/1100100274217042061/1493602898523979778)

  LET SCREEN_W = 1380
  LET SCREEN_H = 824
  SCREEN _NEWIMAGE( SCREEN_W, SCREEN_H, 32 )

  DO
    CLS
    LET ver = INT( RND * 7 )
        ' versions:
        ' 0    - create all orbs as per the color in the original program
        ' 1,2  - create all orbs with one random color
        ' 3    - create each orb with a random color
        ' 4    - create each orb with a 1/5 chance of a random color different from the last orb's color
        ' 5    - create each orb with a 1/30 chance of a random color different from the last orb's color
        ' 6    - create each orb with a 1/30 chance of a random color different from the last orb's color
    IF ver = 0 THEN rf = 16 : gf = 14 : bf = 12
    IF ver > 0 THEN 
       rf = INT( RND * 16 + 1 )
       gf = INT( RND * 16 + 1 )
       bf = INT( RND * 16 + 1 )
    END IF
    FOR i = 1 TO 200
        x = INT( RND * SCREEN_W )
        y = INT( RND * SCREEN_H )
        r = INT( RND * 48 ) + 32
        s = INT( r / 15 )
        IF ( ver = 3 ) _
           OR ( ver = 4 AND INT( RND * 5 ) = 4 ) _
           OR ( ver = 5 AND INT( RND * 30 ) = 29 ) _
           OR ( ver = 6 AND INT( RND * 70 ) = 69 ) _
        THEN 
           rf = INT( RND * 16 + 1 )
           gf = INT( RND * 16 + 1 )
           bf = INT( RND * 16 + 1 )
        END IF

        CIRCLE ( x, y ), r, _RGB( 255, 255, 255 ), , , ,F
        FOR m = 1 TO 15
            col = _RGB( rf * m, gf * m, bf * m )
            CIRCLE (x, y), r, col, , , ,F
            r = r - s
        NEXT

        SLEEP 0.001
    NEXT i
    SLEEP 3
  LOOP


