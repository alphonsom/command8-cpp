# Sending the Command|8 quirk upstream

The patch in this directory is a plain diff. The kernel wants a git commit with
a Signed-off-by, sent inline as email. This is the whole procedure.

## 1. Get the right tree

Sound patches go through Takashi Iwai's tree, not mainline directly:

```sh
git clone https://git.kernel.org/pub/scm/linux/kernel/git/tiwai/sound.git
cd sound
git checkout for-next
```

`for-next` is the branch new material is based on. Basing on `master` invites a
"please rebase" reply.

## 2. Apply the change and commit

```sh
patch -p1 < /path/to/0001-ALSA-usb-audio-add-Digidesign-Command8-MIDI-quirk.patch
git add sound/usb/quirks-table.h
git commit -s          # -s adds Signed-off-by from your git identity
```

`Signed-off-by` is a legal statement (the Developer's Certificate of Origin), so
`user.name` must be your real name — a handle will be rejected.

Suggested commit message, using what we actually measured:

```
ALSA: usb-audio: Add quirk for Digidesign Command|8

The Digidesign Command|8 control surface (0dba:8000) exposes a
MIDIStreaming interface whose class-specific bulk-IN endpoint descriptor
declares bNumEmbMIDIJack 3 but carries only two jack IDs. bLength 6 is
correct for the two that are present, so the count is the field in error,
not the length. The interface also declares only two Embedded MIDI OUT
jacks, and the MS header's wTotalLength (98) disagrees with the
descriptors actually present (82).

As a result snd-usb-audio binds the device and creates a card, but only
output ports:

  $ amidi -l
  Dir Device    Name
   O  hw:2,0,0  Command8 MIDI 1
   O  hw:2,0,1  Command8 MIDI 2
   O  hw:2,0,2  Command8 MIDI 3

With no input port the surface's faders, encoders and buttons are
unreadable, which makes the device useless as a control surface.

The declared jack topology does not describe the hardware either: both
Embedded MIDI OUT jacks are sourced from external (DIN) input jacks, yet
the surface's own data is observed arriving on cable 0. In practice the
device presents three inputs (the surface plus two DIN) and three
outputs. Rather than trying to repair individual fields, ignore the
descriptors and force fixed endpoints with 3 in and 3 out cables on the
MIDIStreaming interface; the bulk endpoints 0x01/0x81 are auto-detected.

Tested on a Command|8 with firmware 02.01.02.

Signed-off-by: Your Name <you@example.com>
```

Adjust the firmware version if `tools/probe_command8.sh` reports a different
one, and drop the "Tested on" line only if it is not true.

## 3. Check it before sending

```sh
./scripts/checkpatch.pl --strict -g HEAD
```

Fix anything it reports. Warnings about long lines in quoted output are usually
tolerated, but style errors in the code are not.

## 4. Find the recipients

Do not guess the addresses — ask the tree:

```sh
git format-patch -1
./scripts/get_maintainer.pl 0001-*.patch
```

That will list Takashi Iwai (sound maintainer), the sound mailing list and
`linux-kernel@vger.kernel.org`. Send to the maintainers, CC the lists.

## 5. Send it

```sh
git send-email --to=<maintainer> --cc=<lists> --cc=<yourself> 0001-*.patch
```

It must be plain-text and inline. Attachments and HTML mail are silently
dropped by the lists. If `git send-email` is not configured, `b4 send` is the
modern alternative and handles most of the setup for you.

## What to expect

Quirk-table additions are routine and usually applied quickly. The one question
a reviewer may reasonably ask is why 3 in-cables when only two Embedded MIDI OUT
jacks are declared. The answer is in the commit message and the code comment:
the descriptors are internally inconsistent and contradict observed behaviour,
so they are not a usable basis for anything — hence the fixed-endpoint quirk.

Reply in-thread, plain text, no top-posting. If asked for changes, send a v2
with a `---`-delimited changelog below the commit message.
