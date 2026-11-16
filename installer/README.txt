Groovebox - Skeer Audio
=======================

A drag-and-drop sample step sequencer: 16 steps, 12 drum channels, per-channel
pitch, pitch drop, ADSR and velocity, swing on a 1/8 or 1/16 grid, a timing
humanize that only ever plays late, a level touch that only ever perturbs what you
wrote, and a master trim with a peak meter. It ships as a VST3 plugin and as a
standalone app.


Where things went
-----------------

VST3 (the plugin hosts load) — one place, and only one:

  %ProgramFiles%\Common Files\VST3\Groovebox.vst3

  There is no per-user copy and no second copy in a DAW folder. A plugin that
  exists twice shows up twice in your browser and the two bundles drift apart
  the first time one is rebuilt, so this installer writes a single copy into
  the folder every host scans.

The setup asks for elevation. Windows will show a UAC prompt when it starts,
because %ProgramFiles% is a protected folder. That is intentional: it is the
cost of putting the plugin somewhere all your hosts see, rather than putting a
second copy somewhere they happen to scan as well.

If you installed an earlier Groovebox, check for the retired copies. Look in
%LOCALAPPDATA%\Programs\Common\VST3 and %USERPROFILE%\Documents\VST3. This
uninstaller does not reach into either — an elevated uninstaller resolves those
per-user paths to whoever is clicking, not to the account that owns them, so
deliberately nothing here deletes them. Remove a stray by hand, and only after
you have confirmed which copy is current: compare the file Groovebox.vst3
inside Contents\x86_64-win\ against the one in
%ProgramFiles%\Common Files\VST3\Groovebox.vst3\Contents\x86_64-win\. If the
hashes differ, the older copy is not a duplicate of your build — it is a
different build, and deleting it destroys the only copy of it.

Worth knowing which stray actually bites you. Live's own scan database records
742 bundles under %ProgramFiles%\Common Files\VST3 and 4 under
%LOCALAPPDATA%\Programs\Common\VST3, and none under Documents: a leftover in
the LOCALAPPDATA folder will rescan as a second Groovebox, while one in
Documents\VST3 is invisible until a host is told to look there.

Standalone app (no host needed):
  Groovebox.exe in this folder. Double-click it, or run it from a terminal if
  you want to see any startup message.

Quit your DAW before installing or reinstalling. The installer will not close
running programs to get at a file — if a host is holding Groovebox.vst3 open,
the copy stops with a message instead of killing your session. Close the host
and run the setup again.

After installing, rescan in your host. In Ableton Live: Options > Preferences >
Plug-Ins, tick "Use VST3 Plug-In System Folder", and click Rescan (hold Alt to
force a full rescan). Live lists it under Plug-Ins > VST3 > Earworm > Groovebox,
and it loads on a MIDI track -- it is an instrument, so it is not in the Audio
Effects list.

If you earlier added Documents\VST3 as Live's "VST3 Plug-In Custom Folder",
remove that entry now and rescan. Groovebox no longer installs there, and if
an old copy is still sitting in a folder Live scans, you get two entries for
one plugin.

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

Press "Chance" for the other number every step carries: how often it is allowed
to fire. The same drag sets it, and the bar hangs from the top of the cell, so
the gap underneath is the part that gets dropped. Only one of the two is ever on
the grid -- pressing one puts the other away -- and "100%" next to them resets
whichever number you are looking at. A reset or a drag across the cells is one
edit, so one undo takes the whole grid back rather than a step at a time.

You do not have to open "Chance" to know which hits are in play: a step the
machine is allowed to drop has its bottom-right corner cut off. It marks the
possibility, not the size of it -- the number itself is on the Chance overlay.
A step that fires every time has no cut, and neither does anything else while
the Variation knob is at 0%, because then nothing is being dropped.

The "Variation" knob decides how much of all that the machine is allowed to use.
At 0% the pattern plays exactly as drawn, whatever the cells say; at 100% every
step answers to its own probability. Which hits get dropped comes from the
number shown beside "Reroll": the same seed and the same grid drop the same hits
on the same steps every time round the loop, and Reroll deals a new take without
moving the pattern you drew. Write the number down if you want that take back --
it is a parameter, not an edit, so there is nothing to undo.

The "Humanize" fader next to it decides how far *behind* the grid a hit is
allowed to start. It only goes one way: nothing plays early, nothing is added
and nothing is dropped -- the hits that fire are the ones the grid and the take
already chose, they just arrive a fraction of a step late. At 0% every hit is on
its onset and the feature costs nothing. At 100% the most a hit can be late is a
quarter of a step, which is half the closest two swung onsets can ever be, so a
late hit can join the one after it but can never pass it: the pattern you drew
stays the pattern that comes out. The offset follows the tempo, like swing does
-- a quarter of a step at 120 is a quarter of a step at 240 -- and most hits
barely move: the draw is weighted towards the grid, so the average sits a third
of the way there and half of them stay inside its first quarter. Which hits are
late comes from the same seed as which hits fire, but from different bits of it,
so moving this fader never changes your take, and "Reroll" re-deals both at once.
A note you play on a keyboard is not pushed late by this knob -- you are already
early or late by however you struck it.

The "Touch" fader after it decides how far off the velocity you wrote a hit is
allowed to be struck. This one goes both ways: a hit can land a little harder or a
little softer than the step says, and never by more than a quarter of that step's
own level. At 0% every hit is struck exactly where you set it. Because the travel
is a proportion of the level rather than a fixed amount, a rest has nothing to move
and stays a rest, and a step at 100% can be taken down but never above full -- so
this control cannot make the set louder than you made it, and cannot put a hit
where the grid drew none. Most hits barely move, the same way timing works: the
draw is weighted towards the number you wrote, so the average sits a third of the
way to the limit and half of them stay inside its first quarter. An accent keeps
its place: a hit written at twice another is still struck above it, because the
travel is a proportion of each hit's own level, and the two would only cross if
the travel were far larger than this knob allows.

What moves is the level, and level is also tone in this engine: a soft hit is
played through a lowpass whose corner rises with how hard it was struck, and at
full velocity the filter is off entirely. A jittered hit can therefore come out a
little darker or a little brighter as well as quieter or louder. That is the tone
law doing what it was built to do, not a second effect, and it is why the loudest
thing the machine plays is still your recording exactly as it was sampled. Which
hits are off their level comes from the same seed as which hits fire and when they
land, but from the 16 bits between them, so moving this fader changes neither your
take nor your groove, and "Reroll" re-deals all three at once. A note you play on a
pad or keyboard is not struck by this knob -- it uses how hard you hit it.

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

The meter beside the trim is the master output. The bar is the sample peak; the
thin tick is the true peak, the highest point of the waveform between the samples,
which is what a converter actually puts out. The tick turns red at 0 dBFS while the
bar can still be short of it -- one hit through this master measures 1.05 dB of that
difference. It is a reading and nothing more: it gates no level and lights no
indicator. Anything reaching 0 dBFS at the sample latches the clip indicator, and
clicking the meter is the only thing that clears the latch.

In a host, every continuous control is a normal plugin parameter -- levels, pitch,
pitch drop, envelopes, mutes, choke groups, swing, swing grid, tempo, the master
trim, the Variation knob, the Humanize and Touch faders and the seed -- so you can
automate them, and the panel moves to follow what you drew. The pattern itself is
not a parameter; it is saved with the project. A session saved by an older build
opens with any parameter it did not have at that build's default, not at whatever
was live when you saved it.


Size and limits
---------------

One 16-step bar per channel. There is no song mode or pattern chaining yet, so a
long arrangement is built in the host.

Humanize is timing only, and late only: it does not start hits ahead of the grid.

Touch is level only, and a deviation rather than a redraw: it cannot turn a rest
into a hit, cannot strike a step more than a quarter of its own level away from
what you wrote, and does not move a hit in time.

Groovebox makes no network calls. The installer writes one registry key,
HKLM\Software\Skeer\Groovebox, and the uninstaller removes it -- along with every
copy of the plugin it installed.

This build is not code-signed, so Windows may warn when the installer starts.
