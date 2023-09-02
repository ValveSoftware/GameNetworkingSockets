This folder contains optional modules in other github repositories.
Most of these repositories are private.

The file structure underneath each module is parallel to the directory
structure of the main project, and sylinks in the main directory structure
point into the files here.  (Those symlinks will be broken links if you do
not have the corresponding module.)

- partner: This has the VPC tool (Valve's bespoke project generator) and
  support for Steam Datagram Relay (SDR).  This module is required.
- ps5: PS4 and PS5
- nswitch: Nintento Switch
- xboxone: Xbox One
- steam: Used for compiling the standalone library to support Steam.
  We discourage this if at all possible and ask developers to get SDR support
  by using the Steamworks SDK if possible.

