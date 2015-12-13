# mod_dahdi_timer for FreeSWITCH

mod_dahdi_timer provides a master 1mS timer source for FreeSWITCH, synchronized to dahdi's master T1 span.  It is useful for sites that use [FreeSWITCH](https://freeswitch.org/) and [FreeTDM in Dahdi mode](https://wiki.freeswitch.org/wiki/FreeTDM#DAHDI_mode), and Dahdi-compatible T1 boards, such as Digium and Rhino boards.  If that description matches your FreeSWITCH environment, and you hear periodic audible artifacts on conferences, this project may be useful for you.

For more information about the problem that this solves, please read this message thread on the freeswitch-dev mailing list:

http://lists.freeswitch.org/pipermail/freeswitch-dev/2013-October/006853.html

> I ran across a problem with a Dahdi-based FreeTDM system a few weeks ago.
 Because the timer for mod_conference was not synchronized to the T1 clock
source, there was significant periodic on-conference noise.  The problem
was not that the "soft" timer was inaccurate; on the contrary, it was a
very accurate 1 mS timer in a system that had a pretty inaccurate T1 clock
coming in.

> My solution was to create a new "mod_dahdi_timer", which derives the ~1 mS
timer from 8 ticks of the 8 kHz /dev/dahdi/timer device.  Then a one-line
change to mod_conference.c to change "soft" to "dahdi", and the conference
sounds perfectly clean.



