// VCS sound without the IOP (docs/NATIVE_PORT_ROADMAP.md milestone 6 step 2; docs/game-internals.md 9).
#pragma once

class PS2Runtime;

namespace vcs
{
    // Serves CDSTREAM.IRX's two sound RPC servers on the host - 0x65686579 (sound effects: banks, the per-frame voice
    // block) and 0x65686578 (streamed music / radio / cutscene audio) - through a native libsd and its own SPU2 core,
    // and makes that SPU2 the audio output. False with VCS_NATIVE_SOUND=0, which keeps the emulated IOP's (the
    // reference).
    bool installSoundHooks(PS2Runtime &runtime);

    // The second output (the police radio on a DualSense's speaker, game/vcs_dualsense.cpp): the effect voices `pick`
    // says yes to - by their sound, its bank and the voice (0-43: core 0's 24, then core 1's 20) - leave the mix and go
    // to ps2AudioAuxPush, asked again at every voice block. Null: none (the mix as it was). Any thread.
    using VoicePick = bool (*)(int sound, int bank, unsigned voice);
    void setSecondOutput(VoicePick pick);
}
