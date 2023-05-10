# Arduino dual AY MIDI detune
Project for Arduino Pro Micro: MIDI synthesizer on two AY-3-8910 chips with detune functionality.

Based on (and great thanks to the author):
https://dogemicrosystems.ca/wiki/Dual_AY-3-8910_MIDI_module

Added functionality:
- support of MIDI pitch bend and modulation wheels/messages
- button for switching working mode
- modes with detune (adding the same note detuned, or octave up/down, or 5th/7th)
- switching to detune modes is displayed with built-in LED
- potentiometer for setting detune ratio on-the-fly

Fixed circuit part for connecting MIDI input (see MIDI_input_fix.png)

Demo video:
https://youtu.be/jKoCbbaBYAo
