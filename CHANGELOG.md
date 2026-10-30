# Changelog

## 2.3.0

First public release in the VCV Library.

- **turn everything into a 碎了吧唧的鼓** (Splinter): 32-step slice sampler for glitchy IDM drums.
  - Slice any WAV / AIFF, or resample any patch audio through IN L / IN R.
  - Grid or transient slicing, 1–64 slices (default 32), with bit-depth and sample-rate reduction.
  - Per step: on, slice number, pitch (±24 st), level, ratchet (1–8) and Normal / Perc mode.
  - Perc mode adds drive, an ADSR and a pitch "punch" envelope to make any sound percussive.
  - Each step stores a slice number. By default step N plays slice N, and numbers above the slice count wrap around.
  - Swing, sequence length 1–32, external clock, slice offset CV and a trigger output.
