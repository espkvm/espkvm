/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/* 16x16 colour marks for what the target is, for the colour panels: the same
   marks as the console's OsIcon.vue. One character a pixel: '.' is the
   background, the rest are colours (icon16_colour in gc9a01.c). */
#pragma once

static const char icon16_linux[16][17] = {
    "......GGGG......",
    ".....GKKKKG.....",
    "....GKWKKWKG....",
    "....GKKKKKKG....",
    "....GKYYYYKG....",
    "...GKKKYYKKKG...",
    "...GKWWWWWWKG...",
    "..GKWWWWWWWWKG..",
    "..GKWWWWWWWWKG..",
    ".GKWWWWWWWWWWKG.",
    ".GKWWWWWWWWWWKG.",
    ".GKWWWWWWWWWWKG.",
    "..GKWWWWWWWWKG..",
    ".YYYKWWWWWWKYYY.",
    "YYYYYKKKKKKYYYYY",
    ".YYYY......YYYY.",
};

static const char icon16_windows[16][17] = {
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "................",
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "BBBBBBB.BBBBBBB.",
    "................",
};

static const char icon16_macos[16][17] = {
    "........LL......",
    ".......LL.......",
    ".......L........",
    "...LLLL.LLLL....",
    "..LLLLLLLLLLLL..",
    ".LLLLLLLLLLLL...",
    ".LLLLLLLLLL.....",
    ".LLLLLLLLL......",
    ".LLLLLLLLL......",
    ".LLLLLLLLLL.....",
    ".LLLLLLLLLLLL...",
    "..LLLLLLLLLLLL..",
    "..LLLLLLLLLLLL..",
    "...LLLLLLLLLL...",
    "....LLL..LLL....",
    "................",
};

static const char icon16_android[16][17] = {
    "...N........N...",
    "....N......N....",
    ".....NNNNNN.....",
    "...NNNNNNNNNN...",
    "..NNN.NNNN.NNN..",
    "..NNNNNNNNNNNN..",
    "................",
    "..NNNNNNNNNNNN..",
    "N.NNNNNNNNNNNN.N",
    "N.NNNNNNNNNNNN.N",
    "N.NNNNNNNNNNNN.N",
    "..NNNNNNNNNNNN..",
    "...NNNNNNNNNN...",
    "....NN....NN....",
    "....NN....NN....",
    "................",
};

static const char icon16_switch[16][17] = {
    "...CCC....RRR...",
    "..CCCCC..RRRRR..",
    "..CCCCC..RRRRR..",
    "..CCKCC..RRRRR..",
    "..CKKKC..RRRRR..",
    "..CCKCC..RRRRR..",
    "..CCCCC..RRRRR..",
    "..CCCCC..RRRRR..",
    "..CCCCC..RRRRR..",
    "..CCCCC..RRKRR..",
    "..CCCCC..RKKKR..",
    "..CCCCC..RRKRR..",
    "..CCCCC..RRRRR..",
    "..CCCCC..RRRRR..",
    "...CCC....RRR...",
    "................",
};

static const char icon16_steamdeck[16][17] = {
    "........W.......",
    "......WWW.......",
    "....WWW.........",
    "...WWW..........",
    "...WW....S......",
    "..WW...SSSSS....",
    "..WW..SSSSSSS...",
    "..WW..SSSSSSS...",
    "..WW..SSSSSSS...",
    "..WW..SSSSSSS...",
    "..WW...SSSSS....",
    "...WW....S......",
    "...WWW..........",
    "....WWW.........",
    "......WWW.......",
    "........W.......",
};
