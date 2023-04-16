# Arduino Dual AY MIDI detune
Project for Arduino ProMicro: MIDI synthesizer on two AY-3-8910 chips with detune functionality

Based on the project:
https://dogemicrosystems.ca/wiki/Dual_AY-3-8910_MIDI_module

Added functionality:
- support of MIDI pitch bend and modulation wheels/messages
- button for switching working mode
- modes with detune (adding detuned the same note, or octave up/down, or 5th/7th)
- detune modes are displayed with built-in LED
- potentiometer for setting detune ratio on-the-fly

Fixed scheme for connecting MIDI input (see MIDI_scheme_fix.png)
