/**
 * @file ArduboyPlaytune.cpp
 * \brief An Arduino library that plays a one or two part musical score and
 * generates tones. Intended for the Arduboy game system.
 */


#if defined(__SAMD51__)

#include "ArduboyPlaytune.h"
#include <Audio.h>
#include <Adafruit_Arcada.h>

extern AudioSynthWaveformSine   sine1, sine2;
extern Adafruit_Arcada arcada;
static volatile boolean playing_chan0=false, playing_chan1=false;

static byte _tune_num_chans = 0;
static volatile boolean tune_playing = false; // is the score still playing?
static volatile unsigned wait_timer_frequency2;       /* its current frequency */
static volatile boolean wait_timer_playing = false;   /* is it currently playing a note? */
static volatile unsigned long wait_toggle_count;      /* countdown score waits */
static volatile boolean all_muted = false; // indicates all sound is muted
static volatile boolean tone_playing = false;
static volatile boolean tone_mutes_score = false;
static volatile boolean tone_only = false; // indicates don't play score on tone channel
static volatile boolean mute_score = false; // indicates tone playing so mute other channels

// pointer to a function that indicates if sound is enabled
static boolean (*outputEnabled)();

// pointers to your musical score and your position in said score
static volatile const byte *score_start = 0;
static volatile const byte *score_cursor = 0;

// Table of midi note frequencies * 2
//   They are times 2 for greater accuracy, yet still fits in a word.
//   Generated from Excel by =ROUND(2*440/32*(2^((x-9)/12)),0) for 0<x<128
// The lowest notes might not work, depending on the Arduino clock frequency
// Ref: http://www.phy.mtu.edu/~suits/notefreqs.html
const uint8_t _midi_byte_note_frequencies[48] PROGMEM = {
16,17,18,19,21,22,23,24,26,28,29,31,33,35,37,39,41,44,46,49,52,55,58,62,65,
69,73,78,82,87,92,98,104,110,117,123,131,139,147,156,165,175,185,196,208,220,
233,247
};
const unsigned int _midi_word_note_frequencies[80] PROGMEM = {
262,277,294,311,330,349,370,392,415,440,466,494,523,554,587,622,659,
698,740,784,831,880,932,988,1047,1109,1175,1245,1319,1397,1480,1568,1661,1760,
1865,1976,2093,2217,2349,2489,2637,2794,2960,3136,3322,3520,3729,3951,4186,
4435,4699,4978,5274,5588,5920,6272,6645,7040,7459,7902,8372,8870,9397,9956,
10548,11175,11840,12544,13290,14080,14917,15804,16744,17740,18795,19912,21096,
22351,23680,25088
};


volatile int32_t duration = 0;
volatile int32_t second = 0;
void TimerCallback()
{
  /*
  if (second == 0) {
    second = 1000;
    Serial.print(".");
  } else {
    second--;
  }*/
 
  if (duration > 0) {
    duration--;
    if (duration == 0) {
      if (tone_playing) {
	sine1.amplitude(0);
	sine2.amplitude(0);
	playing_chan0 = playing_chan1 = false;
	arcada.enableSpeaker(false);
      }
      if (tune_playing) {
	ArduboyPlaytune::step();  // execute commands
      }
    }
  }
}




ArduboyPlaytune::ArduboyPlaytune(boolean (*outEn)())
{
  outputEnabled = outEn;
}

void ArduboyPlaytune::initChannel(byte pin)
{
  AudioMemory(2);
  _tune_num_chans = 2;
  arcada.enableSpeaker(false);
  pinMode(LED_BUILTIN, OUTPUT);   // Onboard LED can be used for precise
  digitalWrite(LED_BUILTIN, LOW); // benchmarking with an oscilloscope
  arcada.timerCallback(1000, TimerCallback);
}

void ArduboyPlaytune::playNote(byte chan, byte note)
{
  // we can't play on a channel that does not exist
  if (chan >= _tune_num_chans) {
    return;
  }
  // we only have frequencies for 128 notes
  if (note > 127) {
    return;
  }

  float freq;
  if (note < 48) {
    freq = _midi_word_note_frequencies[note];
  } else {
    freq = _midi_word_note_frequencies[note-48];
  }
  Serial.print("Play note "); Serial.print(note);
  Serial.print(" = freq "); Serial.print(freq); Serial.print(" on channel "); Serial.println(chan);
  
  if (chan == 0) {
    sine1.amplitude(ARCADA_MAX_VOLUME);
    sine1.frequency(freq);
    playing_chan0 = true;
  } 
  if (chan == 1) {
    sine2.amplitude(ARCADA_MAX_VOLUME);
    sine2.frequency(freq);
    playing_chan1 = true;
  } 
  if (playing_chan0 || playing_chan1) {
    arcada.enableSpeaker(true);
  } else {
    arcada.enableSpeaker(false);
  }
}

void ArduboyPlaytune::stopNote(byte chan)
{
  Serial.print("Stop channel "); Serial.println(chan);
  if (chan == 0) {
    sine1.amplitude(0);
    playing_chan0 = false;
  } 
  if (chan == 1) {
    sine2.amplitude(0);
    playing_chan1 = false;
  } 
  if (playing_chan0 || playing_chan1) {
    arcada.enableSpeaker(true);
  } else {
    arcada.enableSpeaker(false);
  }
}

void ArduboyPlaytune::playScore(const byte *score)
{
  Serial.println("playing score");
  score_start = score;
  score_cursor = score_start;
  step();  /* execute initial commands */
  tune_playing = true;  /* release the interrupt routine */
}

void ArduboyPlaytune::stopScore()
{
  for (uint8_t i = 0; i < _tune_num_chans; i++)
    stopNote(i);
  tune_playing = false;
}

boolean ArduboyPlaytune::playing()
{
  return tune_playing;
}

/* Do score commands until a "wait" is found, or the score is stopped.
This is called initially from playScore(), but then is called
from the interrupt routine when waits expire.

If CMD < 0x80, then the other 7 bits and the next byte are a
15-bit big-endian number of msec to wait
*/
void ArduboyPlaytune::step()
{
  byte command, opcode, chan;

  while (1) {
    command = pgm_read_byte(score_cursor++);
    opcode = command & 0xf0;
    chan = command & 0x0f;
    if (opcode == TUNE_OP_STOPNOTE) { /* stop note */
      Serial.println("Stop note");
      stopNote(chan);
    }
    else if (opcode == TUNE_OP_PLAYNOTE) { /* play note */
      Serial.println("Play note");
      all_muted = !outputEnabled();
      playNote(chan, pgm_read_byte(score_cursor++));
    }
    else if (opcode < 0x80) { /* wait count in msec. */
      duration = ((unsigned)command << 8) | (pgm_read_byte(score_cursor++));
      Serial.print("Wait "); Serial.println(duration);
      break;
    }
    else if (opcode == TUNE_OP_RESTART) { /* restart score */
      Serial.println("Restart score");
      score_cursor = score_start;
    }
    else if (opcode == TUNE_OP_STOP) { /* stop score */
      Serial.println("Stop score");
      tune_playing = false;
      break;
    }
  }
}

void ArduboyPlaytune::closeChannels()
{
  stopNote(0);
  stopNote(1);
  _tune_num_chans = 0;
  tune_playing = tone_playing = tone_only = mute_score = false;
}

void ArduboyPlaytune::tone(unsigned int frequency, unsigned long tone_duration)
{
  // don't output the tone if sound is muted or
  // the tone channel isn't initialised
  if (!outputEnabled() || _tune_num_chans < 2) {
    Serial.println("muted");
    return;
  }
  Serial.print("Play tone: "); Serial.println(frequency);
  tone_playing = true;
  mute_score = tone_mutes_score;

  sine2.amplitude(0);
  sine1.amplitude(ARCADA_MAX_VOLUME);
  playing_chan0 = true;
  playing_chan1 = false;
  sine1.frequency(frequency);
  duration = tone_duration;
  if (playing_chan0 || playing_chan1) {
    arcada.enableSpeaker(true);
  } else {
    arcada.enableSpeaker(false);
  }
}

void ArduboyPlaytune::toneMutesScore(boolean mute)
{
  tone_mutes_score = mute;
}

#endif
