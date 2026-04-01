10 gosub 300
15 print "This should be printed third"
20 goto 310
300 print "This should be printed first"
305 gosub 400
306 return
310 END
400 print "This should be printed second"
410 return
