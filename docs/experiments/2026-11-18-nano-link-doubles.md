# 2026-11-18: doubled words on the Nano's 40 MHz bus

## Question

The Nano's HDZero spectra stream reached the P4 with 1-60 extra words in most symbols, where the simulation had none. Is it the front end or the link, and do raw 40 MS/s grabs (the event kit's HDZero captures) have it too?

## Setup

- Commits: this one. `hdz_top` builds 5-7 and `fm_top` build 6, loaded to SRAM, then `fm_top` 6 written to flash.
- Boards: the pinned p4vrx P4, C5 and Nano, R4 analog on air. PCLK and data at DRIVE=4, as before.

## Method

- Counted repeated adjacent words in spectra grabs.
- Control bit 7 sends a known 14-bit sequence on the 40 MHz bus in place of the data (`iq grab N count`, `tools/linktest.py`): first a plain count, then the count times 0x2d5b, which toggles about half the lanes every clock as data does. Both P4 sampling edges.
- Grabbed raw words with the flips and extras left in (`iq grab N raw`) and studied where the alternation broke.
- Tried each fix offline on the Nov 4 HDZero capture 191225 and an analog R4 grab, with every other word flipped and 0.4% extra words put in (exact copies and one-bit garbles), comparing with the original.

## Results

- Spectra: about 1.25% of words were exact repeats of the word before, 30 times the chance rate for these bins.
- Link test, plain count: 24-41 doubled and 10-19 jumps in 400k words (about 100 ppm). Count x 0x2d5b: 1,607-1,695 doubled (0.41%) and none lost on hdz_top, 4,630 (1.2%) on fm_top 6; both edges alike. Switching noise gives the P4 an extra PCLK edge, so the same word is sampled again.
- Raw grabs go over the same link: before this fix, the event kit's raw HDZero grabs would have had a one-sample timing slip every few hundred samples. The Nov 4 captures used the C5-clock path and decoded cleanly; analog grabs couldn't show it.
- Of the extras in one raw grab, 953 were exact copies and 9 differed from the word before in one bit (sampled mid-change).
- Flipping every bit of alternate words: 176 ppm of genuine HDZero sample pairs satisfy x[n] = -x[n-1] and were dropped as doubles, and each drop put the rest of the grab on the wrong flip.
- Flipping each lane's top bit (+-64) of alternate words, dropping a word that keeps the previous word's top bits and differs from it in two bits at most, then choosing the flips per 4,096 words by smoothness: the offline tests recovered both captures exactly, from either starting parity.
- On hardware with fm_top 6: grabs drop 1,350-3,800 extra words (0.1-0.3%), CRC ok. Jumps of 64 lane steps or more fell from 44,000-110,000 a grab (smoothness choice made word by word) to 0-268, and the remaining ones are the C5's 7-bit lanes wrapping on a strong signal (Q 61, 63, -62, -58), present either way. Analog decoding is unchanged (sync-tip noise 86-103 kHz).
- Spectra on hdz_top 7 still misparse (about 1 symbol in 9 the right length): there the extras are more often garbled in more than two bits, with random bins and frequent idle words.

## Verdict

Adopted for raw grabs: `fm_top` 6 (now in the Nano's flash) flips each lane's top bit in alternate words in raw and spectra modes, and the P4 (`iq.c`, `undouble`) drops the extras and undoes the flips after every grab, reporting `doubled=N`. The spectra stream at 40 MHz stays a development path.

## Next

- The lasting fix for spectra is stage B: slots need about 7M words a second, which fits the 13.33 MHz PCLK that FM mode already runs without trouble.
- PCLK at DRIVE=8 or 12, and more grounds in the ribbon, may cut the extra edges at the source.
