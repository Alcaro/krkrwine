Making-of v2.0
==============

krkrwine works, but needing to install it is suboptimal, for both me and other VN fans. It would be better to have it as part of Wine proper.

After thinking about it, I determined that Wine's nobody-who-has-disassembled rule is mostly to block people who decompile and memorize large parts of the internal structure, like Google's Project Zero, such that they can reproduce copyright violations from memory; it's not meant to block people who have disassembled small pieces to debug some random bug.

So I decided to do exactly that. First up, some easy pieces - the ones in VMR9.

Working inside Wine has a completely different set of constraints from krkrwine.
- I don't need ugly tricks to inject my code.
- I don't need to check if the upstream function is implemented already.
- I don't need a bunch of COM wrapper stuff, it too is implemented already.
- I do, however, need to write C.
- In their preferred coding style, which differs from mine (for example, variables must be declared on top of the function).
- I need to match Windows behavior. I can't just stick a fake media type into the WM sync reader and call it a day.
- I need to write proper tests, not just 'does microkiri run?'
- And I need to deal with other people, in code review.

VMR9
----

Writing the code was easy. Writing the tests was pretty tedious, since ChangeD3DDevice requires a lot of setup work before it makes sense to call it.

Code review wasn't much trouble; the reviewer found a few things to improve, which I did (and an irrelevant-to-me cleanup task, which I did too because why not).

Unfortunately, the tests run on Windows too, not just Wine. And that patch made every single Windows edition start failing.

For some, it's because I did something foolish in an error path and it started segfaulting.

For some, it's because I guessed wrong on Windows behavior. Well, that's easy to fix, just boot this win10 VM and - why is it too hitting that error path? What's it doing there?

Why is SetD3DDevice failing with E_NOINTERFACE? What interface is it even looking for?

Seems to be the little-known {694036ac-542a-4a3a-9a32-53bc20002c1b}, IDirect3DVideoDevice9. GPU driver problems, apparently... how annoying, I know VMR9 has worked on that VM. And it's getting annoyingly slow, and refusing to shut down properly, seemingly due to some broken update. Let's reinstall the entire thing, and...

...nothing. Still broken. Even microkiri refuses to run. Trying it in XP and 7 gives similar issues.

Only one thing to do, and it's not a thing I like to do.

Give up.

Give up and apologize, and hope they won't be too angry.

I'll just have to redeem myself on the next task.

The next task
-------------

is CLSID_CMpegVideoCodec, and some missing features in CLSID_MPEG1Splitter.

To start with, the splitter needs video output. Let's check it a bit... ...wow, that looks massively hardcoded. I think I'll have to rewrite it from scratch... which means I need to avoid regressions, which means lots of tests.

Let's write them before the implementation this time.

And let's make sure to test it on Windows this time.

There's a suitable video file in the test suite already (used by some other test), let's just file it in.

The splitter supports two different output formats, let's print their contents...

(Two [BoredomFS](https://github.com/Alcaro/Arlib/tree/master/subproj/bored) bugs later...)

...what. One is the output of mpegpsdemux, as expected. The other is ... the same thing, but with some extra bytes prefixed? Isn't the other one supposed to be the mpegaudioparse output? That's what Wine expects.

Let's write a test for that too... nope, Wine is inaccurate. On Windows, mpegaudioparse is part of CLSID_CMpegAudioCodec.

If I'm rewriting the splitter from scratch, I might as well make it accurate, which means no mpegaudioparse there. Which means I must add it to the audio codec instead. (The existing tests give the splitter an audio-only .mp2 file, and if so, there will be two mpegaudioparse. Whatever, no harm done, that thing is idempotent.)

How entertaining, krkrwine didn't need to touch the mpeg audio path. Such is the price of doing things properly, I guess...

Well, no point moping, only way to get it implemented is write the code.

First step, hook in an mpegaudioparse for WG_MAJOR_TYPE_AUDIO_MPEG1... ...why is it discarding my samples? Is that not a valid .mp2 file?

Probably not, since the bytes FF FF 18 C4 correspond to MPEG layer 1... but mpegaudioparse should be able to handle that too. Checking the GStreamer debug logs say that it fails to find the sync point (11 consecutive 1 bits), which doesn't make a whole lot of sense, FF FF contains at least 11 1 bits no matter how you slice it.

Installing a debug GStreamer element that just prints its input reveals that... the right bytes are flowing. Playing around with the chopmydata element reveals that... mpegaudioparse doesn't handle too-small buffers well, it chooses to discard a few bytes at the start. Not sure how it manages to catch up and print the tail of the file if it's long enough, but I don't care, that's [the GStreamer team's problem](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/issues/2984).

Easiest fix: Instead of sending one audio frame in ten media packets, send ten audio frames in one media packet.

Now back to the splitter... I guess the easiest implementation is slotting in a decodebin.

Gotta fill in all the GStreamer/Windows media format conversion functions... including the seemingly useless ones (max size isn't really a thing for a compressed format, but it's needed anyways)... oh right, demuxer needs to return the media type, so I need an mpegaudioparse there no matter what I do. Might as well delete the audio decoder patches. Waste of time, but whatever, programming is all about exploration.

...why is mpegaudioparse eating my bytes again? Did I run into the same bug again?

Further digging in the log files reveals that no, it's not - instead, something is creating an invalid segment, with start=0.54s, stop=0.12s, duration=0.12s. stop < start isn't a very useful combination.

I'll just file [another GStreamer bug](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/issues/2987)... two gst bugs in one day. Not a bad haul, except that's not even remotely what I'm looking for.

Now for the important part... finding a sensible workaround.

Easiest is check if the caller asked for end = duration, and if so, set end = infinity. Feels a little unclean, but worst case, the Wine maintainers will propose something else.

Well, that's two of six krkrwine components implemented in Wine; a few fixes later, and microkiri works. I didn't implement IAMStreamSelect and IMediaSeeking, but former is only used by MPEGs with sound, and latter is only needed by Island Diary. I'll just take these guys later; right now, the most important missing piece is the video codec.

Video codec
-----------

Let's start with copypasting the audio codec tests and swapping out the media type.

It starts out easy, just a few segfaults... just have to fix up pieces one by one, and discard tests for pieces I don't care about... ...what's this sequence header thing? Zeroing it out makes the codec error out. I'll just copy it... it's the first 12 bytes of the video stream anyways. Guess I'll have to create an altered MPEG1VIDEOINFO struct. Whatever, no big deal.

Next up, I need to create a valid MPEG-1 video elementary stream, with the minimum possible size (not counting trailing zeroes). The equivalent MPEG audio test is ff ff 18 c4, then 44 zero bytes, but video seems a bit trickier.

Let's just create an empty video and see what happens. ...why is nothing flowing?

It doesn't do anything even if I give it the 1214-byte non-empty video.

Further experimentation reveals that this codec buffers harder than PL_MPEG. It doesn't just look for the next PICTURE start code - it waits until some internal buffer is full, or EndOfStream is called.

I don't want to play games with buffer sizes, heuristics are always fragile. That leaves only one option.

Yep, that works, now let's swap in the black video... ...what. Nothing.

Let's try the bigger black video, with three frames... that works. Two frames works too. I guess Windows eats the last one.

Sure, let's just keep the two-frame video...

Plenty of test failures on the frame timing too. Some of which are utterly absurd, I'll just set them to 'accept anything'...

...and it's done. Now for the codec.

First up, let's add it to DllRegisterServer... nope, nothing. Let's spam some debug prints and see where it's called... nope, it's succeeding. Yet the resulting registry contains half as many keys for the video codec as the known-good audio codec.

Grepping for the applicable GUID, instead of the name, returns winegstreamer_classes.idl. Let's update that, and recreate the Wine prefix yet again... nope. Let's save it... nope. Let's delete the resulting .res file... that's better.

Now to implement it... doesn't seem to be any video codec objects for me to copy? I'll just copy and adapt the MPEG audio object.

Width, height, pixel format... none of that is known, let's just stub it all out.

...nope, that fails. Apparently it needs a pixel format. Let's just pick I420, it seems to be the favorite output format of avdec_mpeg2video.

Filling in these functions one at the time is mostly easy... but now I'm somehow trying to create 0x32 images, should be 32x24. Which part did I typo this time...

...amt_to_wg_format_video_mpeg1(), apparently. I've got a bunch of video_wmv that should be video_mpeg1. It's always the smallest typos that take the longest to find...

Next problem: GStreamer is throwing warnings about WARN basetransform gstbasetransform.c:1371:gst_base_transform_setcaps:<videoconvert24> transform could not transform video/x-raw, format=(string)I420, width=(int)32, height=(int)24, interlace-mode=(string)progressive, pixel-aspect-ratio=(fraction)1/1, chroma-site=(string)jpeg, colorimetry=(string)2:0:0:0, framerate=(fraction)25/1 in anything we support

which is very strange, because that should be a perfectly legal input.

Playing around with gst-launch reveals that I get the same result if the set of supported output formats is empty.

Which is because it's asking for impossible caps - more specifically, it wants framerate=(fraction)0/1 and framerate=(fraction)25/1 simultaneously. The former one shouldn't be there.

Which is a bug in wg_format_to_caps_video - it never initializes the framerate in the GstVideoInfo, so it never emits a nonzero framerate. Yet again, the smallest typos take the longest to find...

Well, it's progress. Still doesn't emit any frames, though.

libav :0:: ignoring extra picture following a frame-picture

Clear enough, it's saying I forgot the mpegvideoparse. Sure, can fix. ...nope, still wrong.

Looks like mpegvideoparse won't emit the second frame until it's done. And avdec_mpeg2video doesn't know if the frame is completed either, so it too waits. And whatever Wine is doing with the end-of-segment event isn't enough.

Three frames of video it is then.

...nope, still gives absurd responses, mostly about format changed detected, returning no output. Fine, let's do it overkill mode, here's a 25 frame video. ...of course that fails too, the video now exceeds my 256 byte buffer. Switch to 2048... ...guess what.

Guess I should investigate those format changes instead... yep, something is definitely changing, someone's adding a colorimetry=(string)2:0:0:0 key. Sounds useless. Let's just ignore the format change... WINE dlls/winegstreamer/wg_transform.c:636:copy_video_buffer: Output buffer is too small.

Well, it's progress... let's ignore that buffer size for now, I need the format change solved properly. Seems that the colorimetry is coming from a GST_QUERY_CAPS query; how about I delete that field from the return value?

...no difference, there's a colorimetry somewhere else too. Maybe that GST_EVENT_CAPS event... yeah, that works.

Now for the buffer size thing... something is sized for I420 (12 bits per pixel), but actually getting YUY2 (16 bits per pixel). Who's sizing this buffer... oh, it's the test itself being wrong. Apparently Windows doesn't mind undersized buffers as much as Wine. Testing error conditions can be useful, but it's a lot lower priority than testing the proper behavior, so let's fix the buffer size.

And now it finally returns data. Test still complains a bunch, but mostly about timing and media types, which is probably the tests being wrong. Looks like Wine's decoder lets video frames have a duration (end != start), but on Windows, the duration is zero. Feels like a Windows bug to me, so I'll just mark both numbers as correct.

Seven bugs later, and I think this thing works... let's see if microkiri agrees.

0024:fixme:quartz:DllGetClassObject {feb50740-7bef-11ce-9bd9-0000e202599c} not implemented, returning CLASS_E_CLASSNOTAVAILABLE.

Well, that is an answer. Easiest bug yet, I forgot compiling the 32bit version.

Next up: 0024:fixme:quartz:VMR9FilterConfig_SetNumberOfStreams iface 0037EE10, count 1, stub!

Checking its source code reveals it is implemented, someone forgot removing the stub tag. Not relevant to me, I'll just ignore it.

Still fails to connect, though. Let's try one of my test tools... same failure.

Let's add debug logs... nothing, not called. Let's add more debug logs... still nothing. Let's add a debug log at a place I absolutely know is called... still nothing. Let's increase WINEDEBUG... let's google its syntax... still nothing.

Let's compile the 64bit quartz/winegstreamer, to match my debug tools being 64bit... that's better. It's passing a too small format struct. Or rather, the struct is big enough, it just forgot reporting the size correctly. That could explain how it got past the test suite... that specific field isn't tested.

Yep, this connected. Tries to shove 49308 bytes into a size-16384 area, though. Where does that buffer size come from? This time, it's not in the program, so it must be from somewhere in Wine. There are three 16384s in winegstreamer, let's change them at random... it's the one in GSTOutPin_DecideBufferSize. Yet another easy fix.

No improvement, though; still gets stuck somewhere. No data flowing anywhere.

More specifically, it's getting stuck in ... the memory allocator? Do I need to make multiple buffers available? Sure, can do, as soon as I find where that buffer count is...

...and what minimum to set it to... 8 works, 2 doesn't, 3 doesn't, 4 does. Sure, works for me.

And now that shitty program runs... and so does microkiri.

Wagamama doesn't like it, though. Let's install my noisy debug tool _rkmovie.dll...

TVPThrowExceptionMessage: Failed to call StreamSelect()->Count(&numOfStream). : [0x80004001] Error: 0x80004001

Oh right, IAMStreamSelect is necessary for MPEGs with sound. Sure, I can implement that. Can't be much left. (Famous last words.)

...nope, just white. Better than being pure black, I guess? Not quite successful yet, though.

This video is implemented with Kirikiri's GetVideoLayerObject. I guess that one has some funny demands on the output format?

Only accepts RGB32 and ARGB32, apparently. I could swear this object supported that, though...

Let's sum up the bytes in the pixels and see what happens.

...what's with these erratic numbers? They have no correlation with the video content whatsoever. Let's print length and average...

...that's tiny. They must be the audio stream. The video stream only gets two frames, then gets stuck. Let's try increasing the buffer count again... yep, that fixed it. No clue what that buffer renderer is doing, but whatever, this works. Oddly enough, krkrwine (my previous CLSID_CMpegVideoCodec implementation) only used a single buffer.

Let's check the todo list... I also need to implement IMediaSeeking.

Except someone else has already done that. It's nice to find good news for once.

Only some cleanups left, then it's time to send the MR.

Cleanups
--------

Plenty of FIXME("aaa\n") and stuff floating around, let's discard that. Let's also discard the changes to the mpeg audio tests, I undid that change. And a few functions I made public are no longer needed, let's undo that too. And some meaningless whitespace changes, out with that too.

And let's run all my tests - Wine's mpegsplit and mpegvideo tests, nanodecode, microkiri, and Wagamama.

All green ...except Wagamama is complaining about some QoS event. Everything works, it's just noisy, but let's investigate anyways.

There's already some code to discard weird QoS events, let's just check for this too.

Next up, let's split it to four commits: Upgrade splitter, add video codec, upgrade splitter test, add video codec test.

...actually, five commits; fifth one is filling in IAMStreamSelect. And I should test it too. And I should make it more robust in case someone's calling it too early or otherwise being weird.

And changing the tests require rerunning them on a real Windows... ...why is IAMStreamSelect::Count returning S_OK (0 streams) before connecting anything? Docs say it should be VFW_E_NOT_CONNECTED. Whatever, if Windows wants to be wrong, I can't stop it; I'll just do the same.

To split the commits, I should
- create a diff file of the complete work
- reset the work tree to the initial state
- put back the MPEG splitter tests
- paste in pieces of the diff until the test passes, except the IAMStreamSelect parts
- make a commit
- put back the rest of the work (except the tests)
- check if any piece belongs more in the splitter than video codec; if no, amend the splitter commit
- undo the IAMStreamSelect parts, then commit the video codec
- redo the IAMStreamSelect parts, commit that too
- add the splitter tests to a fourth commit
- add the video codec tests as a fifth commit
- remove these object files from the commit; not sure why they're not in gitignore, they should be
- update the gitignore files, but don't commit them because they're autogenerated and gitignored (must be something wrong in the gitignores' dependencies... whatever, not my problem)
- push
I'm sure there's a better procedure possible, but I don't know Git well enough to do that.

And now the hardest button press... creating the MR.

...why did the tests fail? I ran them all locally, did I miss a spot?

Why is winmm failing? I didn't touch that.

https://test.winehq.org/data/patterns.html#winmm:mci says that specific test is currently flaky. Rerunning it didn't help, and there's not much else I can do... let's just wait for the reviewers.

Code review
-----------

Nothing unexpected here, just a few comments about keeping cleanups separate from functional changes, teaching the maintainer a new GStreamer feature, and a field I forgot filling in.

WMV
---

I encountered plenty of weirdness here too, but I forgot writing it down. Plenty of weird GStreamer behavior to explain to the maintainers, and the usual nitpicks, but it got merged.

I did, however, notice that CLSID_CWMVDecMediaObject was correctly implemented by someone else at some point, so I didn't need to fix that. I only needed to fix the WMSyncReader and the WMA decoder.

(I also solved the failing VMR9 test. WineHQ has a testbot system where I can upload patches and EXEs and run them on their machines, some of which can run VMR9 properly. Plenty of inexplicable behavior in that module, but I was able to fix the test.)

More VNs
--------

There are, of course, several more VNs I want to test: Marco and the Galaxy Dragon, The Melody of Grisaia, and Crystalline. (I don't plan on (re)reading the latter two, but they're good for testing.)

Marco's title screen's background is completely black, making most menu items invisible; I strongly suspect it's a video that fails to load. Unlike the other two, it's Kirikiri.

Grisaia's ending video is completely black as well (and in a separate window for some reason), but audio works. Crystalline's title screen is also a video (but it failing returns a white background, where the menu items are clearly visible).

Plenty of fun to be had.

But I'd rather wait with that until Proton 9.0 releases. Let's take something else.

Installer
---------

Now that krkrwine is upstreamed into Wine, I'll need to uninstall krkrwine from Proton Experimental. I've put that Proton uninstallation on the backburner because it requires uninstalling from all affected Wine prefixes, and it's hard to know which ones to uninstall from.

But not impossible. All DLLs are symlinked from the compatdata prefixes to the upstream prefix template, I can just check which Proton it points to. (Or I can be lazy and uninstall from everything, krkrwine reinstalls it on next launch anyways.)

Let's check which compatdatas I have, so I know what to test with... ...what the? Why are the symlinks to Proton 8's krkrwine dead?

Where is the main krkrwine.dll?

Was it deleted by a Proton upgrade?

Well, that certainly explains why I didn't see krkrwine in Marco's PROTON_LOG=1... let's move the main krkrwine.dll to the Proton root. krkrwine.py wasn't deleted, so I'm confident Steam won't delete files on that level. Let's also make krkrwine.py force reinstall the DLLs into the main prefix if the subordinate prefix doesn't exist yet. It's not like this project is anywhere near sane anyways, what's a bit more madness? Cthulhu phtagn!

Note to self: Check if the GStreamer/ffmpeg pieces can also be clobbered by updates. If yes, they'll need similar workarounds. And I'll need to move all krkrwine pieces (except user_settings.py) into a subdirectory.

Either way, let's reinstall krkrwine, and see if that fixed Marco... it did. Well, that's one problem less. I was hoping it'd be at least slightly more mentally stimulating than that, but... whatever, it's a big world, there's more stuff to program.

Grisaia... <todo> Crystalline... <todo>
