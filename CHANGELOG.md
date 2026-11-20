# Changelog: DIGISEQ for Digitakt

**This is the Digitakt mk1 version** (`digiseq-dt`). For the Digitone, see
[DIGISEQ for Digitone](https://github.com/ClaidArremer/digitone-digiseq).

Each version's files are in `elemods/` (`digiseq-dt-<version>-os1.54.elemod` and `-os1.53`); the newest is there,
older ones are in the history of this repo.

## 0.12

- **TYPE OFF** shows TYPE alone, on knob A: no SNAKE pages or controls while a track is off.
- Every type's settings follow **SNAKE's PATH page**: A TYPE, B the note length, C the direction or mode, E the
  clock, F SCALE, G RANGE, H SHIFT or its nearest. 185's SETUP and ARP are rearranged to match; EUCLID's, TURING's
  and GRIDS' SETUP put LEN on B and CLOCK on E.
- **ARP**: one page. The OFFSETS page and WALK are gone. New **INT** (knob H): the interval the arp climbs by,
  OCT as before or +1 to +11, so INT +1 steps through a sample chain's slices. RNG goes up to 8.
- **EUCLID**: SETUP's four new knobs: **CHNC** (the chance a hit plays), **GHST** (ghost notes on the rests),
  **VSPR** (velocity spread) and **SPIN** (the pattern turns a few steps more each time round).
- **EUCLID, TURING, GRIDS**: the knobs' values under the steps are in boxes, as on SNAKE's pages. GRIDS draws its
  32 steps as EUCLID does, in two rows of 16 over the trig keys. TURING's current note moves to the title line.
- With the new settings left as they are, every type plays exactly as in 0.11.
- **Names**: the same thing has the same name on every type, spelt out where it fits in the box:
  - STEPS: the pattern's length (SNAKE's LEN, TURING's LEN). 185 keeps STAGES.
  - CLOCK: how often it moves (SNAKE's DIV, ARP's SPD). ARP's CLOCK reads 1X, 1/2, 1/3, 1/4, 1/8, 1/16
    (a note every 1, 2, 3, 4, 8, 16 steps, as before) and turns right for faster, like the others.
  - FILL: how many steps play (TURING's DENS). LEN is always the note length.
  - ARP: REPEAT, RANGE, INTRVL. EUCLID: ROTATE, ACCENT, A.VEL, A.ROT; SETUP: CHANCE, GHOST, A.NOTE,
    SPREAD. TURING: CHANGE, SPREAD. GRIDS: MAP X, MAP Y, A.VEL.
  - Pages: SNAKE's PATH is SETUP, 185's PITCH is NOTES, as on the other types.

## 0.11

- **GRIDS**, a new type, after Mutable Instruments Grids: the track plays one part (kick, snare or hat) of Grids'
  map of drum patterns at a place X, Y on the map, with FILL, CHAOS and accents. Each track has its own place.
- Grids' map is GPL-3.0-or-later, so DIGISEQ as a whole is now GPL-3.0-or-later.
- DIGISEQ is built for size (`-Os`), and keeps one undo copy (PAGE + NO, FUNC + NO) instead of one a track:
  together they free about 23 KB, so a build with GESTALT, MASHUP and GLIDE fits in the mods' RAM.
  FUNC + NO now undoes the last track that ran FUNC + YES.

## 0.10

- **TURING**, a new type, after the Music Thing Turing Machine: a loop of up to 16 bits plays notes in a scale,
  rests and velocities, each bit flipping by CHG as it comes round.

## 0.9

- **EUCLID**, a new type: a Euclidean rhythm of up to 16 steps, with ROT, accents (ACC, AROT, AVL, ANOT), NOTE,
  VEL, CLOCK and LEN. Its page shows the pattern as a row of steps; trig keys set STEPS, FUNC + trig keys HITS.

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
