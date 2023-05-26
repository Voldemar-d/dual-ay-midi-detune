// Arduino Pro Micro project: MIDI synthesizer on two AY-3-8910 chips
// based on: https://dogemicrosystems.ca/wiki/Dual_AY-3-8910_MIDI_module
// added support of pitch bend/modulation and detune (several modes)

// pin with potentiometer setting detune ratio
#define PIN_DETUNE_RATIO A3
// pin with button for switching detune mode
#define PIN_DETUNE_SWITCH A2
// led for displaying detune mode
#define PIN_DETUNE_LED LED_BUILTIN_TX
// from MIDI message 0xE: pitch bend (0x00 - lowest, 0x40 - none, 0x7F - highest)
signed char g_pitchBend = 0;
// from MIDI message 0xB: modulation depth (0x00 - none, 0x7F - max)
unsigned char g_modDepth = 0;
// current working mode (no detune by default) switched with button
unsigned char g_detuneType = 0;

enum eDetune {
  eNoDetune,
  eDetuneOn,
  eDetuneOctUp,
  eDetuneOctDn,
  eDetune5,
  eDetune7,
  eDetuneTotal,
};

class CBtn {
    CBtn() {}
  public:
    CBtn(int nPin) {
      m_bPressed = false;
      m_lastTime = 0;
      m_nPin = nPin;
    }
    bool Pressed() {
      bool bPressed = !digitalRead(m_nPin), ret = false;
      if (bPressed && !m_bPressed) {
        long nCurTime = millis();
        if (nCurTime - m_lastTime > 200) {
          ret = true;
          m_lastTime = nCurTime;
        }
      }
      m_bPressed = bPressed;
      return ret;
    }
  private:
    bool m_bPressed;
    unsigned long m_lastTime;
    int m_nPin;
};

CBtn btnDetune(PIN_DETUNE_SWITCH);

// Uncomment to enable traiditonal serial midi
#define SERIALMIDI

// Uncomment to enable USB midi
#define USBMIDI

// Uncomment to enable debugging output over USB serial
//#define DEBUG

#include <avr/io.h>

#ifdef USBMIDI
#include "MIDIUSB.h"
#endif

// We borrow one struct from MIDIUSB for traditional serial midi, so define it if were not using USBMIDI
#ifndef MIDIUSB_h
typedef struct
{
  uint8_t header;
  uint8_t byte1;
  uint8_t byte2;
  uint8_t byte3;
} midiEventPacket_t;
#endif

#ifdef SERIALMIDI
#include <MIDI.h>
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI); //Serial1 on pro micro
#endif

typedef unsigned short int ushort;

typedef unsigned char note_t;
#define NOTE_OFF 0

typedef unsigned char midictrl_t;

// Pin driver ---------------------------------------------

static const int dbus[8] = { 2, 3, 4, 5, 6, 7, 8, 10 };

static const ushort
BC2_A = 16,
BDIR_A = 14,
BC2_B = A0,
BDIR_B = A1,
nRESET = 15,
clkOUT = 9;

static const ushort DIVISOR = 7; // Set for 1MHz clock

static void clockSetup() {
  // Timer 1 setup for Mega32U4 devices
  //
  // Use CTC mode: WGM13..0 = 0100
  // Toggle OC1A on Compare Match: COM1A1..0 = 01
  // Use ClkIO with no prescaling: CS12..0 = 001
  // Interrupts off: TIMSK0 = 0
  // OCR0A = interval value

  TCCR1A = (1 << COM1A0);
  TCCR1B = (1 << WGM12) | (1 << CS10);
  TCCR1C = 0;
  TIMSK1 = 0;
  OCR1AH = 0;
  OCR1AL = DIVISOR; // NB write high byte first
}

static void setData(unsigned char db) {
  unsigned char bit = 1;
  for (ushort i = 0; i < 8; i++) {
    digitalWrite(dbus[i], (db & bit) ? HIGH : LOW);
    bit <<= 1;
  }
}

static void writeReg_A(unsigned char reg, unsigned char db) {
  // This is a bit of an odd way to do it, BC1 is kept low and NACT, BAR, IAB, and DWS are used.
  // BC1 is kept low the entire time.

  // Inactive (BDIR BC2 BC1 0 0 0)
  digitalWrite(BDIR_A, LOW);
  digitalWrite(BC2_A, LOW);

  //Set register address
  setData(reg);

  // BAR (Latch) (BDIR BC2 BC1 1 0 0)
  digitalWrite(BDIR_A, HIGH);

  // Inactive (BDIR BC2 BC1 0 0 0)
  digitalWrite(BDIR_A, LOW);

  //Set register contents
  setData(db);

  // Write (BDIR BC2 BC1 1 1 0)
  digitalWrite(BC2_A, HIGH);
  digitalWrite(BDIR_A, HIGH);

  // Inactive (BDIR BC2 BC1 0 0 0)
  digitalWrite(BC2_A, LOW);
  digitalWrite(BDIR_A, LOW);
}

static void writeReg_B(unsigned char reg, unsigned char db) {
  // This is a bit of an odd way to do it, BC1 is kept low and NACT, BAR, IAB, and DWS are used.
  // BC1 is kept low the entire time.

  // Inactive (BDIR BC2 BC1 0 0 0)
  digitalWrite(BDIR_B, LOW);
  digitalWrite(BC2_B, LOW);

  //Set register address
  setData(reg);

  // BAR (Latch) (BDIR BC2 BC1 1 0 0)
  digitalWrite(BDIR_B, HIGH);

  // Inactive (BDIR BC2 BC1 0 0 0)
  digitalWrite(BDIR_B, LOW);

  //Set register contents
  setData(db);

  // Write (BDIR BC2 BC1 1 1 0)
  digitalWrite(BC2_B, HIGH);
  digitalWrite(BDIR_B, HIGH);

  // Inactive (BDIR BC2 BC1 0 0 0)
  digitalWrite(BC2_B, LOW);
  digitalWrite(BDIR_B, LOW);
}

// AY-3-8910 driver ---------------------------------------

class PSGRegs {
  public:
    enum {
      TONEALOW = 0,
      TONEAHIGH,
      TONEBLOW,
      TONEBHIGH,
      TONECLOW,
      TONECHIGH,
      NOISEGEN,
      MIXER,

      TONEAAMPL,
      TONEBAMPL,
      TONECAMPL,
      ENVLOW,
      ENVHIGH,
      ENVSHAPE,
      IOA,
      IOB
    };

    unsigned char regs_A[16];
    unsigned char regs_B[16];

    unsigned char lastregs_A[16];
    unsigned char lastregs_B[16];

    void init() {
      for (int i = 0; i < 16; i++) {
        regs_A[i] = lastregs_A[i] = 0xFF;
        writeReg_A(i, regs_A[i]);

        regs_B[i] = lastregs_B[i] = 0xFF;
        writeReg_B(i, regs_B[i]);
      }
    }

    void update() {
      for (int i = 0; i < 16; i++) {
        if (regs_A[i] != lastregs_A[i]) {
          writeReg_A(i, regs_A[i]);
          lastregs_A[i] = regs_A[i];
        }

        if (regs_B[i] != lastregs_B[i]) {
          writeReg_B(i, regs_B[i]);
          lastregs_B[i] = regs_B[i];
        }
      }
    }

    void setTone(ushort ch, ushort divisor, ushort ampl) {
      if (ch > 2) {
        //reduce channel to usable range
        ch = ch - 3;
        //use regs_B
        regs_B[TONEALOW  + (ch << 1)] = (divisor & 0xFF);
        regs_B[TONEAHIGH + (ch << 1)] = (divisor >> 8);
        regs_B[TONEAAMPL + ch] = ampl;

        ushort mask = (8 + 1) << ch;
        regs_B[MIXER] = (regs_B[MIXER] | mask) ^ (1 << ch);

        return;
      }

      regs_A[TONEALOW  + (ch << 1)] = (divisor & 0xFF);
      regs_A[TONEAHIGH + (ch << 1)] = (divisor >> 8);
      regs_A[TONEAAMPL + ch] = ampl;

      ushort mask = (8 + 1) << ch;
      regs_A[MIXER] = (regs_A[MIXER] | mask) ^ (1 << ch);
    }

    void setToneAndNoise(ushort ch, ushort divisor, ushort noisefreq, ushort ampl) {
      if (ch > 2) {
        //reduce channel to usable range
        ch = ch - 3;
        //use regs_B
        regs_B[TONEALOW  + (ch << 1)] = (divisor & 0xFF);
        regs_B[TONEAHIGH + (ch << 1)] = (divisor >> 8);
        regs_B[NOISEGEN] = noisefreq;
        regs_B[TONEAAMPL + ch] = ampl;

        ushort mask = (8 + 1) << ch;
        ushort bits = (noisefreq < 16 ? 8 : 0) + (divisor > 0 ? 1 : 0);
        regs_B[MIXER] = (regs_B[MIXER] | mask) ^ (bits << ch);

        return;
      }

      regs_A[TONEALOW  + (ch << 1)] = (divisor & 0xFF);
      regs_A[TONEAHIGH + (ch << 1)] = (divisor >> 8);
      regs_A[NOISEGEN] = noisefreq;
      regs_A[TONEAAMPL + ch] = ampl;

      ushort mask = (8 + 1) << ch;
      ushort bits = (noisefreq < 16 ? 8 : 0) + (divisor > 0 ? 1 : 0);
      regs_A[MIXER] = (regs_A[MIXER] | mask) ^ (bits << ch);
    }

    void setEnvelope(ushort divisor, ushort shape) {
      regs_A[ENVLOW]  = (divisor & 0xFF);
      regs_A[ENVHIGH] = (divisor >> 8);
      regs_A[ENVSHAPE] = shape;

      regs_B[ENVLOW]  = (divisor & 0xFF);
      regs_B[ENVHIGH] = (divisor >> 8);
      regs_B[ENVSHAPE] = shape;
    }

    void setOff(ushort ch) {
      if (ch > 2) {
        //reduce channel to usable range
        ch = ch - 3;
        //use regs_B
        ushort mask = (8 + 1) << ch;
        regs_B[MIXER] = (regs_A[MIXER] | mask);
        regs_B[TONEAAMPL + ch] = 0;
        if (regs_B[ENVSHAPE] != 0) {
          regs_B[ENVSHAPE] = 0;
          update(); // Force flush
        }
        return;
      }

      ushort mask = (8 + 1) << ch;
      regs_A[MIXER] = (regs_A[MIXER] | mask);
      regs_A[TONEAAMPL + ch] = 0;
      if (regs_A[ENVSHAPE] != 0) {
        regs_A[ENVSHAPE] = 0;
        update(); // Force flush
      }
    }
};

static PSGRegs psg;

// Voice generation ---------------------------------------

static const ushort
MIDI_MIN = 24,
MIDI_MAX = 96,
N_NOTES = (MIDI_MAX + 1 - MIDI_MIN);

static const ushort freq_table[N_NOTES] = {
  327, // MIDI 24, 32.70 Hz
  346, // MIDI 25, 34.65 Hz
  367, // MIDI 26, 36.71 Hz
  389, // MIDI 27, 38.89 Hz
  412, // MIDI 28, 41.20 Hz
  436, // MIDI 29, 43.65 Hz
  463, // MIDI 30, 46.25 Hz
  490, // MIDI 31, 49.00 Hz
  519, // MIDI 32, 51.91 Hz
  550, // MIDI 33, 55.00 Hz
  583, // MIDI 34, 58.27 Hz
  617, // MIDI 35, 61.74 Hz
  654, // MIDI 36, 65.41 Hz
  693, // MIDI 37, 69.30 Hz
  734, // MIDI 38, 73.42 Hz
  778, // MIDI 39, 77.78 Hz
  824, // MIDI 40, 82.41 Hz
  873, // MIDI 41, 87.31 Hz
  925, // MIDI 42, 92.50 Hz
  980, // MIDI 43, 98.00 Hz
  1038, // MIDI 44, 103.83 Hz
  1100, // MIDI 45, 110.00 Hz
  1165, // MIDI 46, 116.54 Hz
  1235, // MIDI 47, 123.47 Hz
  1308, // MIDI 48, 130.81 Hz
  1386, // MIDI 49, 138.59 Hz
  1468, // MIDI 50, 146.83 Hz
  1556, // MIDI 51, 155.56 Hz
  1648, // MIDI 52, 164.81 Hz
  1746, // MIDI 53, 174.61 Hz
  1850, // MIDI 54, 185.00 Hz
  1960, // MIDI 55, 196.00 Hz
  2077, // MIDI 56, 207.65 Hz
  2200, // MIDI 57, 220.00 Hz
  2331, // MIDI 58, 233.08 Hz
  2469, // MIDI 59, 246.94 Hz
  2616, // MIDI 60, 261.63 Hz
  2772, // MIDI 61, 277.18 Hz
  2937, // MIDI 62, 293.66 Hz
  3111, // MIDI 63, 311.13 Hz
  3296, // MIDI 64, 329.63 Hz
  3492, // MIDI 65, 349.23 Hz
  3670, // MIDI 66, 369.99 Hz
  3920, // MIDI 67, 392.00 Hz
  4153, // MIDI 68, 415.30 Hz
  4400, // MIDI 69, 440.00 Hz
  4662, // MIDI 70, 466.16 Hz
  4939, // MIDI 71, 493.88 Hz
  5233, // MIDI 72, 523.25 Hz
  5544, // MIDI 73, 554.37 Hz
  5873, // MIDI 74, 587.33 Hz
  6223, // MIDI 75, 622.25 Hz
  6593, // MIDI 76, 659.26 Hz
  6985, // MIDI 77, 698.46 Hz
  7400, // MIDI 78, 739.99 Hz
  7840, // MIDI 79, 783.99 Hz
  8306, // MIDI 80, 830.61 Hz
  8800, // MIDI 81, 880.00 Hz
  9323, // MIDI 82, 932.33 Hz
  9878, // MIDI 83, 987.77 Hz
  10465, // MIDI 84, 1046.50 Hz
  11087, // MIDI 85, 1108.73 Hz
  11747, // MIDI 86, 1174.66 Hz
  12445, // MIDI 87, 1244.51 Hz
  13185, // MIDI 88, 1318.51 Hz
  13969, // MIDI 89, 1396.91 Hz
  14800, // MIDI 90, 1479.98 Hz
  15680, // MIDI 91, 1567.98 Hz
  16612, // MIDI 92, 1661.22 Hz
  17600, // MIDI 93, 1760.00 Hz
  18647, // MIDI 94, 1864.66 Hz
  19755, // MIDI 95, 1975.53 Hz
  20930, // MIDI 96, 2093.00 Hz
};

struct FXParams {
  ushort noisefreq;
  ushort tonefreq;
  ushort envdecay;
  ushort freqdecay;
  ushort timer;
};

struct ToneParams {
  ushort decay;
  ushort sustain; // Values 0..32
  ushort release;
};

static const ushort MAX_TONES = 4;
static const ToneParams tones[MAX_TONES] = {
  { 30, 24, 10 },
  { 30, 12, 8 },
  { 5,  8,  7 },
  { 10, 31, 30 }
};

class Voice {
  public:
    ushort m_chan;  // Index to psg channel
    note_t m_note;
    ushort m_pitch, m_modstep;
    eDetune m_detune;
    int m_ampl, m_decay, m_sustain, m_release;
    static const int AMPL_MAX = 1023;
    ushort m_adsr;

    void init(ushort chan) {
      m_chan = chan;
      m_ampl = m_sustain = 0;
      kill();
    }

    typedef double freq_t;
    const freq_t ayf = 625000.0, pf = 1.0009172817958015637819657653483, // (2^(1/12))^(1/63)
                 pf5 = 1.3348398541700343648308318811845, // (2^(1/12))^5
                 pf7 = 1.4983070768766814987992807320298, // (2^(1/12))^7
                 dr = 50.0; // detune ratio coefficient
    const ushort modlength = 20, modmax = 10; // modulation rate and max depth

    ushort getPitch(note_t note, eDetune detune, note_t modstep) {
      freq_t freq = freq_table[note - MIDI_MIN], fp = 1.0;
      int modval = (modstep > modlength / 2) ? modlength - modstep : modstep,
          pitchBend = g_pitchBend + modval * modmax * g_modDepth / 0x7F;
      if (pitchBend != 0)
        fp = pow(pf, freq_t(pitchBend));
      if (detune > eNoDetune) {
        fp *= pow(pf, freq_t(analogRead(PIN_DETUNE_RATIO)) / dr);
        switch (detune) {
          case eDetuneOctUp:
            fp *= 2.0;
            break;
          case eDetuneOctDn:
            fp *= 0.5;
            break;
          case eDetune5:
            fp *= pf5;
            break;
          case eDetune7:
            fp *= pf7;
            break;
        }
      }
      freq *= fp;
      float divider = float(ayf / freq);
      return (ushort)divider;
    }

    void start(note_t note, midictrl_t vel, midictrl_t chan, eDetune detune) {
      const ToneParams *tp = &tones[chan % MAX_TONES];
      m_note = note;
      m_modstep = 0;
      m_pitch = getPitch(note, detune, m_modstep);
      m_detune = detune;
      if (vel > 127) {
        m_ampl = AMPL_MAX;
      }
      else {
        m_ampl = 768 + (vel << 1);
      }
      m_decay = tp->decay;
      m_sustain = (m_ampl * tp->sustain) >> 5;
      m_release = tp->release;
      m_adsr = 'D';
      psg.setTone(m_chan, m_pitch, m_ampl >> 6);
    }

    struct FXParams m_fxp;

    void startFX(const struct FXParams &fxp) {
      m_fxp = fxp;

      if (m_ampl > 0) {
        psg.setOff(m_chan);
      }
      m_ampl = AMPL_MAX;
      m_adsr = 'X';
      m_decay = fxp.timer;

      psg.setEnvelope(fxp.envdecay, 9);
      psg.setToneAndNoise(m_chan, fxp.tonefreq, fxp.noisefreq, 31);
    }

    void stop() {
      if (m_adsr == 'X') {
        return; // Will finish when ready...
      }

      if (m_ampl > 0) {
        m_adsr = 'R';
      }
      else {
        psg.setOff(m_chan);
      }
    }

    void update100Hz() {
      if (m_ampl == 0) {
        return;
      }

      switch (m_adsr) {
        case 'D':
          m_ampl -= m_decay;
          if (m_ampl <= m_sustain) {
            m_adsr = 'S';
            m_ampl = m_sustain;
          }
          break;

        case 'S':
          break;

        case 'R':
          if ( m_ampl < m_release ) {
            m_ampl = 0;
          }
          else {
            m_ampl -= m_release;
          }
          break;

        case 'X':
          // FX is playing.
          if (m_fxp.freqdecay > 0) {
            m_fxp.tonefreq += m_fxp.freqdecay;
            psg.setToneAndNoise(m_chan, m_fxp.tonefreq, m_fxp.noisefreq, 31);
          }

          m_ampl -= m_decay;
          if (m_ampl <= 0) {
            psg.setOff(m_chan);
            m_ampl = 0;
          }
          return;

        default:
          break;
      }

      if (m_ampl > 0) {
        m_modstep++;
        m_modstep = m_modstep % modlength;
        m_pitch = getPitch(m_note, (eDetune)m_detune, m_modstep);
        psg.setTone(m_chan, m_pitch, m_ampl >> 6);
      }
      else {
        psg.setOff(m_chan);
      }
    }

    bool isPlaying() {
      return (m_ampl > 0);
    }

    void kill() {
      psg.setOff(m_chan);
      m_ampl = 0;
      m_detune = eNoDetune;
    }
};


static const ushort MAX_VOICES = 6;

static Voice voices[MAX_VOICES];

// MIDI synthesiser ---------------------------------------

// Deals with assigning note on/note off to voices

static const uint8_t PERC_CHANNEL = 9;

static const note_t
PERC_MIN = 35,
PERC_MAX = 50;

static const struct FXParams perc_params[PERC_MAX - PERC_MIN + 1] = {
  // Mappings are from the General MIDI spec at https://www.midi.org/specifications-old/item/gm-level-1-sound-set

  // Params are: noisefreq, tonefreq, envdecay, freqdecay, timer

  { 9, 900, 800, 40, 50 },   // 35 Acoustic bass drum
  { 8, 1000, 700, 40, 50 },  // 36 (C) Bass Drum 1
  { 4, 0, 300, 0, 80 },      // 37 Side Stick
  { 6, 0, 1200, 0, 30  },    // 38 Acoustic snare

  { 5, 0, 1500, 0, 90 },     // 39 (D#) Hand clap
  { 6, 400, 1200, 11, 30  }, // 40 Electric snare
  { 16, 700, 800, 20, 30 },  // 41 Low floor tom
  { 0, 0, 300, 0, 80 },      // 42 Closed Hi Hat

  { 16, 400, 800, 13, 30 },   // 43 (G) High Floor Tom
  { 0, 0, 600, 0, 50 },      // 44 Pedal Hi-Hat
  { 16, 800, 1400, 30, 25 }, // 45 Low Tom
  { 0, 0, 800, 0, 40 },      // 46 Open Hi-Hat

  { 16, 600, 1400, 20, 25 }, // 47 (B) Low-Mid Tom
  { 16, 450, 1500, 15, 22 }, // 48 Hi-Mid Tom
  { 1, 0, 1800, 0, 25 },     // 49 Crash Cymbal 1
  { 16, 300, 1500, 10, 22 }, // 50 High Tom
};


static const int REQ_MAP_SIZE = (N_NOTES + 7) / 8;
static uint8_t m_requestMap[REQ_MAP_SIZE];
// Bit is set for each note being requested
static  midictrl_t m_velocity[N_NOTES];
// Requested velocity for each note
static  midictrl_t m_chan[N_NOTES];
// Requested MIDI channel for each note
static uint8_t m_highest, m_lowest;
// Highest and lowest requested notes

static const uint8_t NO_NOTE = 0xFF;
static const uint8_t PERC_NOTE = 0xFE;
static uint8_t m_playing[MAX_VOICES];
// Which note each voice is playing

static const uint8_t NO_VOICE = 0xFF;
static uint8_t m_voiceNo[N_NOTES];
// Which voice is playing each note


static bool startNote(ushort idx) {
  const bool bDetuneOn = (g_detuneType > eNoDetune);
  const ushort nv = bDetuneOn ? 3 : MAX_VOICES;
  for (ushort i = 0; i < nv; i++) {
    if (m_playing[i] == NO_NOTE) {
      voices[i].start(MIDI_MIN + idx, m_velocity[idx], m_chan[idx], eNoDetune);
      m_playing[i] = idx;
      m_voiceNo[idx] = i;
      if (bDetuneOn) { // start second detuned voice
        i += 3;
        voices[i].start(MIDI_MIN + idx, m_velocity[idx], m_chan[idx], (eDetune)g_detuneType);
        m_playing[i] = idx;
      }
      return true;
    }
  }
  return false;
}

static bool startPercussion(note_t note) {
  const ushort nv = (g_detuneType > eNoDetune) ? 3 : MAX_VOICES;
  for (ushort i = 0; i < nv; i++) {
    if (m_playing[i] == NO_NOTE || m_playing[i] == PERC_NOTE) {
      if (note >= PERC_MIN && note <= PERC_MAX) {
        voices[i].startFX(perc_params[note - PERC_MIN]);
        m_playing[i] = PERC_NOTE;
      }
      return true;
    }
  }
  return false;
}

static bool stopNote(ushort idx) {
  uint8_t v = m_voiceNo[idx];
  if (v != NO_VOICE) {
    voices[v].stop();
    m_playing[v] = NO_NOTE;
    m_voiceNo[idx] = NO_VOICE;
    if (g_detuneType > eNoDetune && v < 3) { // stop second detuned voice
      v += 3;
      voices[v].stop();
      m_playing[v] = NO_NOTE;
    }
    return true;
  }
  return false;
}

static void stopOneNote() {
  uint8_t v, chosen = NO_NOTE, nv = (g_detuneType > eNoDetune) ? 3 : MAX_VOICES;

  // At this point we have run out of voices.
  // Pick a voice and stop it. We leave a voice alone
  // if it's playing the highest requested note. If it's
  // playing the lowest requested note we look for a 'better'
  // note, but stop it if none found.

  for (v = 0; v < nv; v++) {
    uint8_t idx = m_playing[v];
    if (idx == NO_NOTE) // Uh? Perhaps called by mistake.
      return;

    if (idx == m_highest)
      continue;

    if (idx == PERC_NOTE)
      continue;

    chosen = idx;
    if (idx != m_lowest)
      break;
    // else keep going, we may find a better one
  }

  if (chosen != NO_NOTE)
    stopNote(chosen);
}

static void updateRequestedNotes() {
  m_highest = m_lowest = NO_NOTE;
  ushort i, j;

  // Check highest requested note is playing
  // Return true if note was restarted; false if already playing
  for (i = 0; i < REQ_MAP_SIZE; i++) {
    uint8_t req = m_requestMap[i];
    if (0 == req)
      continue;

    for (j = 0; j < 8; j++) {
      if (req & (1 << j)) {
        ushort idx = i * 8 + j;
        if (m_lowest == NO_NOTE || m_lowest > idx) {
          m_lowest = idx;
        }
        if (m_highest == NO_NOTE || m_highest < idx)  {
          m_highest = idx;
        }
      }
    }
  }
}

static bool restartANote() {
  if (m_highest != NO_NOTE && m_voiceNo[m_highest] == NO_VOICE) {
    return startNote(m_highest);
  }

  if (m_lowest != NO_NOTE && m_voiceNo[m_lowest] == NO_VOICE) {
    return startNote(m_lowest);
  }

  return false;
}

static void synth_init() {
  ushort i;

  for (i = 0; i < REQ_MAP_SIZE; i++) {
    m_requestMap[i] = 0;
  }

  for (i = 0; i < N_NOTES; i++) {
    m_velocity[i] = 0;
    m_voiceNo[i] = NO_VOICE;
  }

  for (i = 0; i < MAX_VOICES; i++) {
    m_playing[i] = NO_NOTE;
  }

  m_highest = m_lowest = NO_NOTE;
}

static void noteOff(midictrl_t chan, note_t note, midictrl_t vel) {
  if (chan == PERC_CHANNEL || note < MIDI_MIN || note > MIDI_MAX) {
    return; // Just ignore it
  }

  ushort idx = note - MIDI_MIN;

  m_requestMap[idx / 8] &= ~(1 << (idx & 7));
  m_velocity[idx] = 0;
  updateRequestedNotes();

  if (stopNote(idx)) {
    restartANote();
  }
}

static void noteOn(midictrl_t chan, note_t note, midictrl_t vel) {
  if (vel == 0) {
    noteOff(chan, note, 0);
    return;
  }

  if (chan == PERC_CHANNEL) {
    if (!startPercussion(note)) {
      stopOneNote();
      startPercussion(note);
    }
    return;
  }

  // Regular note processing now

  if (note < MIDI_MIN || note > MIDI_MAX) {
    return; // Just ignore it
  }

  ushort idx = note - MIDI_MIN;

  if (m_voiceNo[idx] != NO_VOICE) {
    return; // Already playing. Ignore this request.
  }

  m_requestMap[idx / 8] |= 1 << (idx & 7);
  m_velocity[idx] = vel;
  m_chan[idx] = chan;
  updateRequestedNotes();

  if (!startNote(idx)) {
    stopOneNote();
    startNote(idx);
  }
}

static void update100Hz() {
  for (ushort i = 0; i < MAX_VOICES; i++) {
    voices[i].update100Hz();
    if (m_playing[i] == PERC_NOTE && ! (voices[i].isPlaying())) {
      m_playing[i] = NO_NOTE;
      restartANote();
    }
  }
}

// Main code ----------------------------------------------

static unsigned long lastUpdate = 0;

void setup() {
  // Setup detune pins
  pinMode(PIN_DETUNE_RATIO, INPUT);
  pinMode(PIN_DETUNE_SWITCH, INPUT_PULLUP);
  pinMode(PIN_DETUNE_LED, OUTPUT);

  // Hold in reset while we set up the reset
  pinMode(nRESET, OUTPUT);
  digitalWrite(nRESET, LOW);

  pinMode(clkOUT, OUTPUT);
  digitalWrite(clkOUT, LOW);
  clockSetup();

  pinMode(BC2_A, OUTPUT);
  digitalWrite(BC2_A, LOW); // BC2 low
  pinMode(BDIR_A, OUTPUT);
  digitalWrite(BDIR_A, LOW); // BDIR low

  pinMode(BC2_B, OUTPUT);
  digitalWrite(BC2_B, LOW); // BC2 low
  pinMode(BDIR_B, OUTPUT);
  digitalWrite(BDIR_B, LOW); // BDIR low

  for (ushort i = 0; i < 8; i++) {
    pinMode(dbus[i], OUTPUT);
    digitalWrite(dbus[i], LOW); // Set bus low
  }

  delay(100);
  digitalWrite(nRESET, HIGH); // Release Reset
  delay(10);

  lastUpdate = millis();

  psg.init();
  for (ushort i = 0; i < MAX_VOICES; i++) {
    voices[i].init(i);
  }
  synth_init();

#ifdef DEBUG
  Serial.begin(115200);
#endif

#ifdef SERIALMIDI
  // Initiate MIDI communications, listen to all channels
  MIDI.begin(MIDI_CHANNEL_OMNI);
#endif
}

void handleMidiMessage(midiEventPacket_t &rx) {
  if (rx.header == 0xE && (rx.byte1 & 0xF0) == 0xE0) { // Pitch bend
    g_pitchBend = rx.byte3 - 0x40;
  }
  else if (rx.header == 0x9) { // Note on
    noteOn(rx.byte1 & 0xF, rx.byte2, rx.byte3);
  }
  else if (rx.header == 0x8) { // Note off
    noteOff(rx.byte1 & 0xF, rx.byte2, rx.byte3);
  }
  else if (rx.header == 0xB) { // Control Change
    if (rx.byte2 == 0x78 || rx.byte2 == 0x79 || rx.byte2 == 0x7B) // AllSoundOff, ResetAllControllers, or AllNotesOff
      KillVoices();
    else if ((rx.byte1 & 0xF0) == 0xB0 && rx.byte2 == 0x01) // Modulation
      g_modDepth = rx.byte3;
  }
}

void KillVoices() {
  for (ushort i = 0; i < MAX_VOICES; i++)
    voices[i].kill();
  synth_init();
}

void loop() {
  midiEventPacket_t rx;

#ifdef USBMIDI
  rx = MidiUSB.read();

#ifdef DEBUG
  //MIDI debugging
  if (rx.header != 0) {
    Serial.print("Received USB: ");
    Serial.print(rx.header, HEX);
    Serial.print("-");
    Serial.print(rx.byte1, HEX);
    Serial.print("-");
    Serial.print(rx.byte2, HEX);
    Serial.print("-");
    Serial.println(rx.byte3, HEX);
  }
#endif

  handleMidiMessage(rx);
#endif

#ifdef SERIALMIDI
  //Check for serial MIDI messages
  //MIDI.read();
  while (MIDI.read()) {
    // Create midiEventPacket_t
    rx =
    {
      byte(MIDI.getType() >> 4),
      byte(MIDI.getType() | ((MIDI.getChannel() - 1) & 0x0f)), /* getChannel() returns values from 1 to 16 */
      MIDI.getData1(),
      MIDI.getData2()
    };

#ifdef DEBUG
    //MIDI debugging
    if (rx.header != 0 && rx.header != 0xF) {
      Serial.print("Received MIDI: ");
      Serial.print(rx.header, HEX);
      Serial.print("-");
      Serial.print(rx.byte1, HEX);
      Serial.print("-");
      Serial.print(rx.byte2, HEX);
      Serial.print("-");
      Serial.println(rx.byte3, HEX);
    }
#endif

    handleMidiMessage(rx);
  }
#endif

  bool bPressed = false;
  unsigned long now = millis();
  if ((now - lastUpdate) > 10) {
    update100Hz();
    lastUpdate += 10;
    bPressed = btnDetune.Pressed();
  }
  if (bPressed) {
    KillVoices();
    g_detuneType++;
    if (g_detuneType >= eDetuneTotal)
      g_detuneType = eNoDetune;
    digitalWrite(PIN_DETUNE_LED, g_detuneType > eNoDetune ? LOW : HIGH);
  }

  psg.update();
}
