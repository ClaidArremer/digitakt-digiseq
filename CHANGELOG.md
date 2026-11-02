# Changelog: DIGISEQ for Digitakt

**This is the Digitakt mk1 version** (`digiseq-dt`). For the Digitone, see
[DIGISEQ for Digitone](https://github.com/ClaidArremer/digitone-digiseq).

Each version's files are in `elemods/` (`digiseq-dt-<version>-os1.54.elemod` and `-os1.53`); the newest is there,
older ones are in the history of this repo.

## 0.8 (2026-11-01)

- **Live recording records SNAKE and 185.** With REC + PLAY, every note a type plays goes onto its step as it
  plays: note lock, velocity, no micro timing or condition; 185's held and LEN-set notes keep their LEN. Changes
  made during the take (SNAKE's DIV, SHAPE, DIR; 185's stages) are recorded as heard. When recording stops, TYPE
  goes to OFF so the trigs play back; a take at 2X keeps the track at 2X, as PRINT does.

## 0.7 (2026-11-01)

- **ARP: SCALE** on knob E of the ARP page (where 185's SETUP has it): CHR or one of 11 scales, rooted on the
  track's NOTE. Every arp note (its octaves, offsets and WALK) moves down to the scale's nearest note.
- ARP: N.LEN is gone (SCALE took its knob); each arp note is as long as the track's LEN.

## 0.6 (2026-11-01)

- **FUNC + TRIG** opens the stock QUANTIZE menu again (0.3 to 0.5 opened DIGISEQ with it).
- ARP: setting **WALK to 0** takes the arp back to its untransposed notes at once.
- Messages show in the **stock drop-down**, worded as on the Digitone (*Type=ARP*, *Offset 12=+5*); the ARP
  page's knobs show theirs (*Arp Mode=UP-DN*, *Walk=+3*).

## 0.5 (2026-10-31)

- ARP: **live recording** (REC + PLAY) records the arp's notes from the keys into the track, then sets TYPE to
  OFF. With no key held while recording, the track's trigs play as stored.

## 0.4 (2026-10-31)

- ARP plays the **keys held in keyboard mode** (FUNC + TRK) on an audio track, in the keyboard's scale, root,
  fold and octave; **HOLD** latches them. Stopped, a key plays as usual and is still held for the arp.

## 0.3 (2026-10-30)

- **ARP**, a third type (Digitakt only): an arpeggiator played by the track's note trigs. MODE (UP, DOWN, UP-DN,
  UP+DN, CONV, DIV, PINKY, THUMB, ORDER, RAND, SHUF), SPD, RNG, N.LEN, RPT, WALK, HOLD and a 16-step offset
  sequence. In slice mode it arpeggiates slices. PRINT shows *PRINT N/A FOR ARP*.

## 0.2 (2026-10-26)

- PRINT puts each printed note on its step with no trig condition, so a trig left there from before can no
  longer shift it (micro timing) or mute it on some loops (a condition such as 1:2).

## 0.1

- The first version: SNAKE (from DIGISNAKE for Digitakt 0.3) and 185.
