multijack
=========

Lightweight multi-track audio recorder with Jack interface. Record one or two tracks whilst replaying a two track mix-down of any / all tracks. Default is to enable 16 tracks but more or fewer may be used.

This project is inspired by the need to run a multitrack recorder in a home recording studio on a small budget. It is tested on a Raspberry Pi Model B.
The Raspberry Pi is chosen as a low power, silent device. Files are saved to a USB flash drive and audio is via USB stereo soundcard.

Can record one or two channels of audio whilst playing back any / all tacks, mixed down to either or both of two output channels. This provides a method of recording whilst monitoring previously recorded tracks but it is intended to perform mixing and mastering in a separate dedicated DAW. A multichannel WAVE file contains all tracks which may be imported in to another application such as Ardour or Audacity.

There is a ncurses user interface, purposefully kept simple. It is intended to add other interfaces such as hardware buttons, MIDI, network, etc.

Key commands (subject to change):

up / down arrows - select channel
m - toggle selected channel mute
M - toggle selected channel mute and set all channels mute the same
a - toggle record from A (left) input
b - toggle record from B (right) input
l - toggle monitor track on left output
r - toggle monitor track on right output
C - pan centre
e - clear error count
q - Quit
space - start / stop
G - toggle record enable
home - move playhead to beginning
end - move playhead to end
< - move playhead 1 second earlier
> - move playhead 1 second later

Compile with:
    g++ -std=c++11 multijack.cpp -o multijack -lncurses -ljack
or:
    make
Note: Requires g++ 4.7 or later for c++11 support.
