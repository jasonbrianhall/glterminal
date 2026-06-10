REM Kaleidoscope Simulator
REM BBC BASIC
MODE 8
VDU 23,1,0;0;0;0;
DIM px(49), py(49), vx(49), vy(49), col(49)
centreX = 640
centreY = 512
radius = 400
particles = 50
FOR i = 0 TO particles-1
  px(i) = RND(radius) - radius/2
  py(i) = RND(radius) - radius/2
  vx(i) = RND(7)-4
  vy(i) = RND(7)-4
  col(i) = RND(63)
NEXT
REPEAT
  GCOL 0,0
  CLG
  FOR i = 0 TO particles-1
    px(i) += vx(i)
    py(i) += vy(i)
    IF px(i) > radius OR px(i) < -radius THEN vx(i) = -vx(i)
    IF py(i) > radius OR py(i) < -radius THEN vy(i) = -vy(i)
    x = px(i)
    y = py(i)
    FOR s = 0 TO 5
      angle = s * PI / 3
      xr = x * COS(angle) - y * SIN(angle)
      yr = x * SIN(angle) + y * COS(angle)
      GCOL 0,col(i)
      CIRCLE FILL centreX + xr, centreY + yr, 8
      xr = x * COS(angle) + y * SIN(angle)
      yr = x * SIN(angle) - y * COS(angle)
      CIRCLE FILL centreX + xr, centreY + yr, 8
    NEXT
  NEXT
  WAIT 1
UNTIL INKEY(-99)
END
