// [gta-recomp] Host audio output for the emulated SPU2 (48 kHz stereo). PS2X_AUDIO=0 disables it.
#pragma once

#include <cstdint>

void ps2AudioInit();     // after platform::init (main thread)
void ps2AudioPush(const int16_t *stereo, uint32_t frames); // any thread
void ps2AudioShutdown(); // before platform::shutdown
float ps2AudioQueuedMs();          // audio waiting to be played (perf overlay)
uint64_t ps2AudioUnderrunCount();  // underruns since start

// [gta-recomp] The player's audio settings (any thread, applied from the next audio callback): gains 0..1 for the
// whole output and for movie sound, the output mode (0 stereo, 1 mono, 2 left and right swapped), and a mute that
// keeps everything running (the window in the background).
void ps2AudioSetVolume(float master, float movies);
void ps2AudioSetOutputMode(int mode);
void ps2AudioSetMuted(bool muted);

// [gta-recomp] Movie sound (a game's movies played natively): 48 kHz stereo frames mixed into the output 1:1, on top of
// the SPU2. ps2AudioMovieActive: false when there is no audio output (muted / hidden runs) - the player keeps time
// itself then. Push returns the frames taken (the ring holds 1 s); Played counts the frames mixed since the last
// Reset, which also drops what is queued.
// [gta-recomp] A second output (the police radio on a DualSense's speaker): 48 kHz stereo pushed by the SPU2's aux
// voices (IopSpu2::setAuxVoices), played on the playback device whose name contains `deviceSubstr`. Open / close on the
// main thread; push from any thread. Open returns false when no such device is there (nothing is played then).
bool ps2AudioAuxOpen(const char *deviceSubstr);
void ps2AudioAuxClose();
bool ps2AudioAuxActive();
void ps2AudioAuxPush(const int16_t *stereo, uint32_t frames);

bool ps2AudioMovieActive();
uint32_t ps2AudioMoviePush(const int16_t *stereo, uint32_t frames);
uint64_t ps2AudioMoviePlayed();
uint32_t ps2AudioMovieQueued();
void ps2AudioMovieReset();
