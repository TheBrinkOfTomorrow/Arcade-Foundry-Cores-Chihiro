# File lists

`<set>.h`: every file of a known-good copy of the game (path, size, CRC-32;
nothing of the files themselves), which the core's `check_roms` checks a
game folder against. Made on the Mac from the packed, checked copies
(`crtaxihr.tar` from the Rev B image):

    python3 common/tools/filelist/make_filelist.py <your Chihiro folder>/vcop3.tar -o filelists/vcop3.h

Paths are relative to the game folder (the tar's one top folder). A set
without a list builds without check_roms (the app says "Downloaded").
