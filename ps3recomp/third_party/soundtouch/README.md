# SoundTouch (vendored)

The time-stretch library RPCS3 uses for its "time stretching" audio option.

- Source: https://github.com/RPCS3/soundtouch.git, commit
  `a0fba77b6f9cfbdb71f8bbec58b6ac4e5e3b1097` (the commit RPCS3 pins as the
  `3rdparty/SoundTouch/soundtouch` submodule).
- Licence: LGPL-2.1, see `COPYING.TXT`.
- Only the files RPCS3's `3rdparty/SoundTouch/CMakeLists.txt` compiles are
  kept, plus their headers. They are unmodified.
- Built with `ST_NO_EXCEPTION_HANDLING USE_MULTICH_ALWAYS SOUNDTOUCH_FLOAT_SAMPLES
  SOUNDTOUCH_ALLOW_SSE`, the same as RPCS3.

Used by `libs/audio/audio_out.cpp`, the host audio output stage.
