| SPDX-License-Identifier: GPL-2.0-or-later
| DIGISEQ for Digitakt: the keyboard's two sites (OS 1.53 and 1.54, the same
| addresses and bytes in both). The keyboard mode (FUNC + TRK) plays a held
| key through one routine and lets it go through another:
|
|   0x4002930e  note on (obj, key, track, note, vel, -1): plays the note and
|               remembers it as the key's
|   0x40028c78  note off (obj, key): lets go of the key's notes
|
| Both are entered by jmp over their first whole instructions. ARP takes a
| key's note (digiseq_kb_on answers 1: the stock note does not play, the arp
| plays it); otherwise the stock routine runs as it was. UI task.

        .section .run, "ax"

| 0x4002930e, over `link.w a6,#-64; moveq #127,d0` (6 bytes)
| in: (sp) the caller's return, 4 obj, 8 key, 12 track, 16 note, 20 vel
        .globl  dsq_site_kbon
dsq_site_kbon:
        move.l  20(%sp), -(%sp)         | vel
        move.l  20(%sp), -(%sp)         | note
        move.l  20(%sp), -(%sp)         | track
        move.l  20(%sp), -(%sp)         | key
        move.l  20(%sp), -(%sp)         | obj
        jsr     digiseq_kb_on
        lea     20(%sp), %sp
        tst.l   %d0
        beq.s   1f
        rts                             | ARP's: the stock note does not play
1:      link.w  %a6, #-64               | the instructions this replaced
        moveq   #127, %d0
        jmp     0x40029314

| 0x40028c78, over `lea -24(sp),sp; movem.l d2/a2-a6,(sp)` (8 bytes)
| in: (sp) the caller's return, 4 obj, 8 key
        .globl  dsq_site_kboff
dsq_site_kboff:
        move.l  8(%sp), -(%sp)          | key
        jsr     digiseq_kb_off
        addq.l  #4, %sp
        lea     -24(%sp), %sp           | the instructions this replaced
        movem.l %d2/%a2-%a6, (%sp)
        jmp     0x40028c80
