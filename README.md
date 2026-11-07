# Sample Drop

A VST3 sampler instrument. Drop up to 8 audio files on the window, pick one from the
dropdown, optionally select part of it, and any MIDI note (or the Play button) plays it.

## Using it
* **Drop files** anywhere on the window (several at once is fine). Formats: WAV, AIFF, FLAC,
  OGG, MP3 (Windows also reads other Windows Media formats).
* **Dropdown** lists the loaded files; selecting one switches the waveform and the file that plays.
  Click the **x** at the right end of a row to remove just that file (later files move up).
* **Select a part of the file:** drag on the waveform. Drag either edge to adjust it,
  double-click to go back to the whole file. Only the selected part plays; the cut points get a
  ~1.3 ms fade so they never click. Each file remembers its own selection.
* **Play** sends the same trigger as MIDI note 60 (middle C, velocity 100) so you can audition
  without drawing a note. It needs the host to be processing audio (in REAPER: arm/monitor the
  track, or enable "Run FX when stopped").
* **Clear** removes all files. **Perf** toggles the live performance overlay.
* Resize by dragging the window edge / bottom-right corner. Size, files and selections are saved
  with the project (the files must still exist at the same path).

## Playback rules (current)
* Any note-on starts the selected file's region at its original speed (note number ignored).
* Velocity scales volume (velocity / 127). Note-off is ignored: one-shot, the region plays to its
  end. All-notes-off / all-sound-off stop everything.
* 32 voices; if all are busy the oldest is restarted.
* A file at a different sample rate than the project plays at the correct pitch (linear interpolation).

## Performance design
* Audio thread: no locks, no allocation, no reference counting, no virtual calls. MIDI bytes are
  parsed directly. Same-rate playback uses SIMD. Notes start sample-accurately. The Play button and
  the selection reach the audio thread through single atomics.
* Files decode on background threads (low priority), in chunks, and are published to the audio
  thread through atomic pointers. Removed samples are freed only after the audio thread has moved
  on, so a block in flight can never read freed memory.
* Waveform: peaks are computed once at load. The outline is rebuilt only when the file or window
  size changes (cost proportional to window width). The waveform is a cached image; dragging a
  selection repaints only a thin transparent layer.

## How to find what is slow (three tools)

**1. Live overlay (real host, real machine): click Perf.** Refreshes twice a second.
* `load avg/max/peak`: audio time used as a % of the time available per block. `xruns`: blocks
  that took longer than their budget (turns red). `voices`: active voices.
* One row per measured section, with `avg` and `max` (this window), `peak` (whole session), `calls`:
  audio `processBlock`, `midi`, `voiceRender`, `trigLatency` (Play click to the block that starts it);
  loader `fileDecode`, `peakBuild`; UI `libCommit`, `libRemove`, `editorResize`, `menuBuild`,
  `wavePath`, `wavePaint`, `selPaint`.
* To measure new code add an entry to `perf::Id` and `perf::kInfo` in `PerfMonitor.h`, then put
  `perf::ScopedTimer t (monitor, perf::Id::MyThing);` at the top of the code.
* Release build without any measurement code: configure with `-DSAMPLER_PERF_MONITOR=OFF`.

**2. Benchmarks (`SamplerBenchmarks`, run by GitHub on every push).** Each part is timed in
isolation against the real-time budget: idle block, 1/8/32 voices (same rate and rate conversion),
a 32-notes-per-block retrigger storm with fades, MIDI parsing, file decoding speed, waveform outline
at several widths, and the profiler's own overhead. Median and best of several runs; fails if a
budget is exceeded. Results appear on the workflow run page (summary) and as the `benchmark-results`
artifact. Compare the numbers between runs to spot slowdowns early. Add a new case in
`Benchmarks/Benchmarks.cpp` whenever you add a performance-critical feature.

**3. Tests (`SamplerTests`, also run by GitHub on every push).** They check the audio output
sample by sample, region fades, removal while playing, memory safety, project state, and the report.

Local build of the developer targets (optional):
`cmake -B build -DSAMPLER_BUILD_TESTS=ON -DSAMPLER_BUILD_BENCHMARKS=ON`
