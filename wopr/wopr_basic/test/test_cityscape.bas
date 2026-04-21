' test_cityscape.bas
' Minimal test for city building layout.
' Expected result: buildings spread across full 640px width.
' Bug was: FnRan(x) clobbered outer loop var x (all vars are global).
' Fix: renamed loop var to CityX, FnRan param to n.

Screen 9
Randomize Timer

ScrWidth  = 640
ScrHeight = 350

GoSub DrawCity
Sleep
System

DrawCity:
    BottomLine   = 335
    HtInc        = 10
    DefBWidth    = 37
    RandomHeight = 120
    WWidth       = 3
    WHeight      = 6
    WDifV        = 15
    WDifh        = 10
    MaxHeight    = 0
    GHeight      = 25
    CityX        = 2
    NewHt        = 15

    Do
        NewHt = NewHt + 2 * HtInc

        BWidth = FnRan(DefBWidth) + DefBWidth
        If CityX + BWidth > ScrWidth Then BWidth = ScrWidth - CityX - 2

        BHeight = FnRan(RandomHeight) + NewHt
        If BHeight < HtInc Then BHeight = HtInc
        If BottomLine - BHeight <= MaxHeight + GHeight Then _
            BHeight = MaxHeight + GHeight - 5

        BuildingColor = FnRan(3) + 4

        Line (CityX - 1, BottomLine + 1)-(CityX + BWidth + 1, BottomLine - BHeight - 1), 0, B
        Line (CityX, BottomLine)-(CityX + BWidth, BottomLine - BHeight), BuildingColor, BF

        WinX = CityX + 3
        Do
            For i = BHeight - 3 To 7 Step -WDifV
                If FnRan(4) = 1 Then
                    Line (WinX, BottomLine - i)-(WinX + WWidth, BottomLine - i + WHeight), 8, BF
                Else
                    Line (WinX, BottomLine - i)-(WinX + WWidth, BottomLine - i + WHeight), 14, BF
                End If
            Next i
            WinX = WinX + WDifh
        Loop Until WinX >= CityX + BWidth - 3

        Locate 1, 1
        Print "CityX="; CityX; " BWidth="; BWidth; "   "

        CityX = CityX + BWidth + 2

    Loop Until CityX > ScrWidth - HtInc

    Locate 2, 1
    Print "Done - buildings should span full width"
Return

Function FnRan (n)
    FnRan = Int(Rnd(1) * n) + 1
End Function
