Partial Reductions for multiplications
--------------------------------------

It is possible to get away with partial reductions for multiplications
instead of fully reducing everything. The largest input to square/mult 
will come from an unreduced add, which will double the element values. 
Test values are 1 bit larger than actual maximum values.

    max27 = (1 << 27) - 1
    max26 = (1 << 26) - 1

Largest values from an add of full bit values (max27,max26,max27,max26..)

    m0 0x1f1fffea8000042c
    m1 0x133ffff190000268
    m2 0x185fffef00000354
    m3 0x0ebffff4f00001d8
    m4 0x119ffff38000027c
    m5 0x0a3ffff850000148
    m6 0x0adffff8000001a4
    m7 0x05bffffbb00000b8
    m8 0x041ffffc800000cc
    m9 0x013fffff10000028

Carry values from reducing sums

    c0 0x00000007c7fffaa0
    c1 0x000000099ffffcab
    c2 0x0000000617fffe27
    c3 0x000000075ffffd83
    c4 0x0000000467fffeb7
    c5 0x000000051ffffe5b
    c6 0x00000002b7ffff47
    c7 0x00000002dfffff33
    c8 0x0000000107ffffd7
    c9         0xa000000b
    c0         0x000002f8


The largest carried value r1 could receive is 0x2f8, with everything else 
fitting in 25 or 26 bits. Assuming full values for everything, with 0x2f8
added to r1 (max27,maxr1,max27,max26..):

    max27 = (1 << 27) - 1
    max26 = (1 << 26) - 1
    maxr1 = (((1 << 25) - 1) + 0x2f8) * 2
    
    m0 0x1f2006f77ffc7dac
    m1 0x134000508fffeaa8
    m2 0x1860004e004655d4
    m3 0x0ec00053efffea18
    m4 0x11a000527fffd2fc
    m5 0x0a4000574fffe988
    m6 0x0ae00056ffffd224
    m7 0x05c0005aafffe8f8
    m8 0x0420005b7fffd14c
    m9 0x0140005e0fffe868

Carry values

    c0 0x00000007c801bddf
    c1 0x00000009a0002c2c
    c2 0x00000006180015e8
    c3 0x0000000760002d04
    c4 0x0000000468001678
    c5 0x0000000520002ddc
    c6 0x00000002b8001708
    c7 0x00000002e0002eb4
    c8 0x0000000108001798
    c9         0xa0002f8c
    c0         0x000002f9

The largest carried value is now 0x2f9 (max27,maxr1b,max27,max26..)

    max27 = (1 << 27) - 1
    max26 = (1 << 26) - 1
    maxr1b = (((1 << 25) - 1) + 0x2f9) * 2

    m0 0x1f2006f9dffc7c7c
    m1 0x13400050afffeaa0
    m2 0x1860004e2046854c
    m3 0x0ec000540fffea10
    m4 0x11a000529fffd2ec
    m5 0x0a4000576fffe980
    m6 0x0ae000571fffd214
    m7 0x05c0005acfffe8f0
    m8 0x0420005b9fffd13c
    m9 0x0140005e2fffe860

Carry values

    c0 0x00000007c801be77
    c1 0x00000009a0002c3c
    c2 0x00000006180015f0
    c3 0x0000000760002d14
    c4 0x0000000468001680
    c5 0x0000000520002dec
    c6 0x00000002b8001710
    c7 0x00000002e0002ec4
    c8 0x00000001080017a0
    c9         0xa0002f9c
    c0         0x000002f9

The largest carried value is fixed at 0x2f9. Subtracting the largest values 
from 0 will result in r0 exceeding 26 bits, but r0-r4 are safe for 
multiplications up to 30 bits, so partial reductions throughout the entire 
calculation should be safe to chain. This especially helps with speeding up 
the SSE2 version by freeing it from large serial carry chains. Testing of 
course continues, but no problems as of yet have shown up.


Subtraction
-----------
Subtraction with unsigned elements is done using Emilia Kasper's trick, via 
agl: http://www.imperialviolet.org/2010/12/04/ecc.html

Adding a large enough value that is equivalent to 0 mod p before subracting
ensures no elements underflow.

Compiler
--------
gcc (as of 4.4.5) has a difficult time optimizing the 32 bit C version properly. 
icc produces code that is roughly 40% faster.