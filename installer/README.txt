Groovebox - Workshop Audio / Skeer Audio
========================================

A drag-and-drop sample step sequencer: 16 steps, 12 drum channels, per-channel
pitch, ADSR and velocity, swing on a 1/8 or 1/16 grid, and a master trim with a
peak meter. It ships as a VST3 plugin and as a standalone app.


Where things went
-----------------

VST3 (the plugin hosts load):
  per-user install  -> %LOCALAPPDATA%\Programs\Common\VST3\Groovebox.vst3
  all-users install -> %ProgramFiles%\Common Files\VST3\Groovebox.vst3
  with the Ableton task -> %USERPROFILE%\Documents\VST3\Groovebox.vst3

Standalone app (no host needed):
  Groovebox.exe in this folder. Double-click it, or run it from a terminal if
  you want to see any startup message.

After installing, rescan in your host. In Ableton Live: Options > Preferences >
Plug-Ins, tick "Use VST3 Plug-In System Folder", and click Rescan (hold Alt to
force a full rescan). If Groovebox does not appear, tick "VST3 Plug-In Custom
Folder" and point it at one of the folders above, then rescan again. Live lists
it under Plug-Ins > VST3 > Earworm > Groovebox, and it loads on a MIDI track --
it is an instrument, so it is not in the Audio Effects list.

Run only one at a time. The standalone takes the audio device for itself, so
start it after closing your host, or it will fail to open the driver -- and
close it before your host needs ASIO back.


Loading sounds
--------------

Drag a .wav / .aiff / .mp3 / .flac / .ogg onto a channel's row, or click that
row's sound button and browse to the file. Each channel plays one sample at a
time, so loading a new one replaces it.

The session remembers sounds by file path. A sample you have since moved or
deleted comes back silent, so keep the kit in one place.


Playing it
----------

Click cells in the grid to write a pattern, or drag across a row to paint a run.
The white column is the playhead, and a row lights up while it is firing.

Press "Velocity" in the top bar and drag up or down inside a step to set how
hard that step hits; the bar that rises shows the value. Steps keep their own
level, so a pattern can have accents.

Each strip's A D S R rows shape that channel: drag a row up or down, double-click
to reset just that one. Sustain 0 (the default) makes the channel a one-shot --
raise it and the sound holds until you release the note.

The -12 / - / + / +12 buttons transpose a channel by semitones, and the two
arrows shift its whole row one step earlier or later, velocities included.

MIDI notes C1 to B1 (36 to 47) play channels 1 to 12, with the note's velocity
setting the hit's level. Notes play whether or not the transport is running, so
you can play the kit over a stopped pattern. Notes outside that range are
ignored, and a muted channel ignores its note.


Levels
------

The channel faders top out above unity, and the Master Trim in the top bar spans
-24 to +6 dB (default -2 dB). Nothing in the signal path scales your mix behind
your back, and there is no limiter: the trim and the faders are what you have.

The meter beside the trim is the master output. Anything reaching 0 dBFS latches
the clip indicator, and clicking the meter is the only thing that clears the
latch.

In a host, every continuous control is a normal plugin parameter -- levels,
pitch, envelopes, mutes, swing, tempo and the master trim -- so you can automate
them, and the panel moves to follow what you drew. The pattern itself is not a
parameter; it is saved with the project.


Size and limits
---------------

One 16-step bar per channel. There is no song mode or pattern chaining yet, so a
long arrangement is built in the host.

Groovebox makes no network calls. The installer writes one registry key,
HKCU\Software\Skeer\Groovebox, and the uninstaller removes it -- along with every
copy of the plugin it installed.

This build is not code-signed, so Windows may warn when the installer starts.
