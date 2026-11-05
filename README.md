# Sample Drop

A VST3 sampler instrument. Drop up to 8 audio files on the window, pick one from the
dropdown, and any MIDI note plays it at its original speed.

## Using it
* **Drop files** anywhere on the window (several at once is fine). Formats: WAV, AIFF, FLAC,
  OGG, MP3 (Windows also reads other Windows Media formats).
* **Dropdown** (top left) lists the loaded files; selecting one switches the waveform and
  the file that MIDI notes play.
* **Clear** removes all files so you can load a new set (the limit is 8).
* **Resize** by dragging the window edge / bottom-right corner. Size is saved with the project.
* The loaded files and the selection are saved with the project and reloaded automatically
  (the files must still exist at the same path).
* **Perf** button toggles the live performance overlay.

## Playback rules (current)
* Any note-on starts the selected file from the beginning at original speed (note number ignored).
* Velocity scales volume (velocity / 127).
* Note-off is ignored: the file plays to its end (one-shot). All-notes-off / all-sound-off stop everything.
* 32 voices; if all are busy the oldest is restarted.
* If the file's sample rate differs from the project's, it is played at the correct pitch using
  linear interpolation.

## Performance design
* Audio thread: no locks, no allocation, no reference counting, no virtual calls. MIDI bytes are
  parsed directly. Same-rate files use SIMD `addWithMultiply`. Notes start sample-accurately.
* Files are decoded on background threads (low priority), in chunks, and published to the audio
  thread through atomic pointers. Removed samples are freed only after the audio thread has
  moved on, so a block in flight can never read freed memory.
* Waveform: peaks (4096 min/max buckets) are computed once at load time. The outline is rebuilt
  only on sample/size change (cost proportional to window width) and the view is cached as an image.

## Performance monitor
Click **Perf**. Numbers refresh twice a second ("window") and peaks are kept for the session
(re-clicking Perf resets them).

* `load avg/max/peak` - audio processing time as a percentage of the time available per block.
* `xruns` - blocks that took longer than their real-time budget (turns red if non-zero).
* `voices` - active voices (window / session peak).
* One row per measured section: `avg`, `max` (this window), `peak` (session), `calls`:
  * audio: `processBlock` (total), `midi`, `voiceRender`
  * loader: `fileDecode`, `peakBuild`
  * UI: `libCommit`, `editorResize`, `wavePath`, `wavePaint`

To measure new code: add an entry to `perf::Id` and `perf::kInfo` in `PerfMonitor.h`, then put
`perf::ScopedTimer t (monitor, perf::Id::MyThing);` at the top of the code to measure.

For a final release build, configure with `-DSAMPLER_PERF_MONITOR=OFF`
(`cmake -B build -DSAMPLER_PERF_MONITOR=OFF`): all measurement code compiles to nothing.
