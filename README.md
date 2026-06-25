# sfm - suckless file manager

sfm is a fast, minimal terminal file manager inspired by suckless philosophy.

## Dependencies

Only POSIX, libc. (Its super minimal) <br>
sfm uses ANSI escape codes for rendering and the terminal's alternate screen buffer for display isolation.

## Installation

Edit config.mk to match your local setup (sfm is installed into the /usr/local namespace by default).

Afterwards enter the following command to build and install sfm (if necessary as root):

    make install

Alternatively if you want to just try sfm then run the following command :
    
    make && ./sfm

## Usage

    sfm [directory]

Start in the current or specified directory.
> [!NOTE]
> For default keybinds check out the config.def.h and if you want to change, feel free to make chnages in the config.h and recompile.

## Configuration

The configuration of sfm is done by creating a custom config.h and (re)compiling the source code.

    make clean && make

Settings can be overridden at runtime via environment variables:
FM_OPENER, OPENER, FM_EDITOR, EDITOR.
