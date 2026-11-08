# cezanne-voice: noise reduction and call echo cancelling

User request (2026-11-04): echo cancelling and noise reduction (RNNoise, the
Xiph / Mozilla network whose current model is trained with PyTorch) during
calls, the same noise reduction for everything recorded with the
microphone, and a switch for it in the Power app.

## Parts

- **cezanne-rnnoise.so** (LADSPA, `/usr/lib/ladspa`): mono 48 kHz, the
  system librnnoise 0.2 with its built-in model. 30 ms delay (one 10 ms
  frame of buffering + RNNoise's own two frames, measured by
  cross-correlation), reported on the `latency` port; off, it passes the
  input through with the same delay (bit-exact). On / off: the file
  `~/.config/cezanne/noise-reduction` ("0" = off, missing = on), watched
  by a thread per instance (inotify), so the switch acts at once on every
  running chain.
- **cezanne-voice.service** (user, with PipeWire): `pipewire -c
  cezanne-voice.conf`, a filter chain declared as a WirePlumber smart
  filter: streams that record from the default source are linked through
  it, its input follows the default source (internal mic FL = the main
  bottom mic, or the headset), and the default source stays the hardware
  one. Idle (no CPU) while nothing records.
- **call.conf.in**: the cellular call chain, run by cezanne-speechd
  (package cezanne-modem) as the desktop user for the length of a call:
  mic FL -> WebRTC AEC3 (its own NS / AGC off) -> RNNoise -> modem uplink;
  modem downlink -> the canceller's sink (its reference) -> default sink.
  The AFE then routes PCM 2 through the AP: uplink from DL2 (`hw:0,2`,
  Playback_2), downlink to UL_MONO_1 (`hw:0,18`, Capture_Mono_1); speechd
  creates those two ALSA nodes in the sound server (`pw-cli create-node`,
  found again by name in `pw-dump`) after setting the routes (a DPCM front
  end without a back end cannot be opened), and links the canceller's
  capture to the hardware source by hand: WirePlumber routes even a named
  target through the smart filter, and the canceller must see the mic
  before any non-linear processing. Without cezanne-voice, with SPH_AP=0,
  or when the chain does not come up, the call uses the direct path (mics
  and amps wired to the modem, no AP processing).
- **Power app** (cezanne-power 1.0-5): "Microphone - Noise reduction"
  writes the state file.

The echo canceller is for cellular calls only: in the general chain it would
need the speaker as reference all the time (capturing the sink's monitor
keeps the amps running whenever anything records). VoIP apps bring their
own (WebRTC); they get the noise reduction through the smart filter.

## Measurements (2026-11-04)

- Offline (IVR speech from a call + noise at 5 dB SNR, the plugin through a
  LADSPA host): noise alone -56 dB (white) / -37 dB (fan-like); speech
  pauses -23 / -28 dB; SNR in speech 9.1 -> 14.8 / 11.4 dB; clean speech
  through it -0.1 dB, correlation 0.981.
- Live, room noise through the smart filter: -65.4 dBFS on, -44.1 dBFS
  switched off with the state file (same running filter).
- CPU: 60 s of audio in 1.28 s on a big core (2.1 %), 6.6 s on a little
  one (11 %); only while something records.
- Call chain dry run (no call: no SIM in the phone since before
  2026-11-04): routes, nodes, chain and links up in 0.67 s, the uplink
  carries the processed mic (-61 dBFS room), clean teardown. Not yet in a
  real call: `mainline/modem/tools/call-test.sh 0639000 35 1 8` with
  INBAND=1 sends the DTMF as tones through the AP uplink (an IVR that
  reacts proves it).
