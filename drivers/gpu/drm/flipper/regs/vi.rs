// SPDX-License-Identifier: GPL-2.0 or MIT

// We don't expect that all the registers and fields will be used, even in the
// future.
//#![allow(dead_code)]

use kernel::{
    io::Region,
    num,
    register, //
};

#[derive(Debug)]
#[repr(u16)]
enum Format {
    Ntsc = 0,
    Pal = 1,
    MPal = 2,
    Debug = 3,
}

impl From<num::Bounded<u16, 2>> for Format {
    fn from(num: num::Bounded<u16, 2>) -> Format {
        match num.get() {
            0 => Format::Ntsc,
            1 => Format::Pal,
            2 => Format::MPal,
            3 => Format::Debug,
            _ => unreachable!("Format can only have up to two bits!"),
        }
    }
}

impl From<Format> for num::Bounded<u16, 2> {
    fn from(mode: Format) -> num::Bounded<u16, 2> {
        match mode {
            Format::Ntsc => num::Bounded::<u16, 2>::new::<0>(),
            Format::Pal => num::Bounded::<u16, 2>::new::<1>(),
            Format::MPal => num::Bounded::<u16, 2>::new::<2>(),
            Format::Debug => num::Bounded::<u16, 2>::new::<3>(),
        }
    }
}

#[derive(Debug)]
#[repr(u16)]
enum Clock {
    _27MHz = 0,
    _54MHz = 1,
}

impl From<num::Bounded<u16, 1>> for Clock {
    fn from(num: num::Bounded<u16, 1>) -> Clock {
        match num.get() {
            0 => Clock::_27MHz,
            1 => Clock::_54MHz,
            _ => unreachable!("Clock can only have up to one bit!"),
        }
    }
}

impl From<Clock> for num::Bounded<u16, 1> {
    fn from(mode: Clock) -> num::Bounded<u16, 1> {
        match mode {
            Clock::_27MHz => num::Bounded::<u16, 1>::new::<0>(),
            Clock::_54MHz => num::Bounded::<u16, 1>::new::<1>(),
        }
    }
}

register! {
    base: Region<0x100>;

    /// TODO
    pub(crate) VERTICAL_TIMING(u16) @ 0x00 {
        /// Active video in lines per field (seems always zero)
        13:4    acv;
        /// Equalization pulse in half lines
        3:0     equ;
    }

    /// TODO
    pub(crate) CONTROL(u16) @ 0x02 {
        /// 0: NTSC, 1: PAL, 2: MPAL, 3: Debug
        9:8     format => Format;
        /// TODO
        7:6     le1;
        /// Display Latch; 0: Off, 1: On for 1 field, 2: On for 2 fields, 3: Always on
        5:4     le0;
        /// Selects 3D Display Mode
        3:3     dlr => bool;
        /// 0: Interlaced, 1: Non-Interlaced: top field drawn at field rate and bottom field is not displayed
        2:2     nin => bool;
        /// Clears all data requests and puts VI into its idle state
        1:1     reset => bool;
        /// Enables video timing generation and data request
        0:0     enable => bool;
    }

    /// TODO
    pub(crate) HORIZONTAL_TIMING_0(u32) @ 0x04 {
        /// Horizontal Sync Start to Color Burst Start
        30:24   hcs;
        /// Horizontal Sync Start to Color Burst End
        22:16   hce;
        /// Halfline Width (W*16 = Width (720))
        8:0     hlw;
    }

    /// TODO
    pub(crate) HORIZONTAL_TIMING_1(u32) @ 0x08 {
        /// Half line to horizontal blanking start
        26:17   hbs;
        /// Horizontal Sync Start to horizontal blank end
        16:7    hbe;
        /// Horizontal Sync Width
        6:0     hsy;
    }

    /// TODO
    pub(crate) VBLANK_TIMING_ODD(u32) @ 0x0c {
        /// Post blanking in half lines
        25:16   psb;
        /// Pre-blanking in half lines
        9:0     prb;
    }

    /// TODO
    pub(crate) VBLANK_TIMING_EVEN(u32) @ 0x10 {
        /// Post blanking in half lines
        25:16   psb;
        /// Pre-blanking in half lines
        9:0     prb;
    }

    /// TODO
    pub(crate) VBLANK_BURST_BLANKING_ODD(u32) @ 0x14 {
        31:21   be3;
        20:16   bs3;
        15:5    be1;
        4:0     bs1;
    }

    /// TODO
    pub(crate) VBLANK_BURST_BLANKING_EVEN(u32) @ 0x18 {
        31:21   be4;
        20:16   bs4;
        15:5    be2;
        4:0     bs2;
    }

    /// TODO
    pub(crate) FB_LEFT_TOP(u32) @ 0x1c {
        /// When this bit is set, the framebuffer address is calculated as (address>>5).
        28:28   right_shift_address_by_five => bool;
        /// Horizontal Offset of the left-most pixel within the first word of the fetched picture.
        27:24   xof;
        /// bit 23 - bit 9 of XFB Address (*2)
        ///
        /// If bit 28 is cleared, highest possible Address: 0x80fffe00 (set register to 0x00fffe00) (aligned to 9bit)
        23:9    fbb;
    }

    /// TODO
    pub(crate) FB_RIGHT_TOP(u32) @ 0x20 {
        31:0    todo;
    }

    /// TODO
    pub(crate) FB_LEFT_BOTTOM(u32) @ 0x24 {
        /// When this bit is set, the framebuffer address is calculated as (address>>5).
        28:28   right_shift_address_by_five => bool;
        /// Horizontal Offset of the left-most pixel within the first word of the fetched picture.
        27:24   xof;
        /// bit 23 - bit 9 of XFB Address (*2)
        ///
        /// If bit 28 is cleared, highest possible Address: 0x80fffe00 (set register to 0x00fffe00) (aligned to 9bit)
        23:9    fbb;
    }

    /// TODO
    pub(crate) FB_RIGHT_BOTTOM(u32) @ 0x28 {
        31:0    todo;
    }

    /// TODO
    pub(crate) VERTICAL_BEAM_POSITION(u16) @ 0x2c {
        /// Current vertical position of raster beam.
        10:0    vct;
    }

    /// TODO
    pub(crate) HORIZONTAL_BEAM_POSITION(u16) @ 0x2e {
        /// Current horizontal position of raster beam.
        10:0    hct;
    }

    /// TODO
    pub(crate) DISPLAY_INTERRUPT_0(u32) @ 0x30 {
        /// Interrupt Status (1=Active) (Write to clear)
        31:31   interrupt => bool;
        /// Interrupt Enable Bit
        28:28   enable => bool;
        /// Vertical Position
        25:16   vct;
        /// Horizontal Position
        9:0     hct;
    }

    /// TODO
    pub(crate) DISPLAY_INTERRUPT_1(u32) @ 0x34 {
        /// Interrupt Status (1=Active) (Write to clear)
        31:31   interrupt => bool;
        /// Interrupt Enable Bit
        28:28   enable => bool;
        /// Vertical Position
        25:16   vct;
        /// Horizontal Position
        9:0     hct;
    }

    /// TODO
    pub(crate) DISPLAY_INTERRUPT_2(u32) @ 0x38 {
        /// Interrupt Status (1=Active) (Write to clear)
        31:31   interrupt => bool;
        /// Interrupt Enable Bit
        28:28   enable => bool;
        /// Vertical Position
        25:16   vct;
        /// Horizontal Position
        9:0     hct;
    }

    /// TODO
    pub(crate) DISPLAY_INTERRUPT_3(u32) @ 0x3c {
        /// Interrupt Status (1=Active) (Write to clear)
        31:31   interrupt => bool;
        /// Interrupt Enable Bit
        28:28   enable => bool;
        /// Vertical Position
        25:16   vct;
        /// Horizontal Position
        9:0     hct;
    }

    /// TODO
    pub(crate) DISPLAY_LATCH_0(u32) @ 0x40 {
        /// Trigger Flag
        31:31   trigger => bool;
        /// Vertical Count
        26:16   vct;
        /// Horizontal Count
        10:0    hct;
    }

    /// TODO
    pub(crate) DISPLAY_LATCH_1(u32) @ 0x44 {
        /// Trigger Flag
        31:31   trigger => bool;
        /// Vertical Count
        26:16   vct;
        /// Horizontal Count
        10:0    hct;
    }

    /// TODO
    pub(crate) HSCALEW(u16) @ 0x48 {
        14:8    wpl;
        7:0     std;
    }

    /// TODO
    pub(crate) HSCALER(u16) @ 0x4a {
        /// Enable Horizontal Scaling
        12:12   enable => bool;
        /// Horizontal stepping size (U1.8 Scaler Value) (0x160 Works for 320)
        8:0     stp;
    }

    /// TODO
    pub(crate) FILTER_COEF_0(u32) @ 0x4c {
        29:20   tap2;
        19:10   tap1;
        9:0     tap0;
    }

    /// TODO
    pub(crate) FILTER_COEF_1(u32) @ 0x50 {
        29:20   tap5;
        19:10   tap4;
        9:0     tap3;
    }

    /// TODO
    pub(crate) FILTER_COEF_2(u32) @ 0x54 {
        29:20   tap8;
        19:10   tap7;
        9:0     tap6;
    }

    /// TODO
    pub(crate) FILTER_COEF_3(u32) @ 0x58 {
        31:24   tap12;
        23:16   tap11;
        15:8    tap10;
        7:0     tap9;
    }

    /// TODO
    pub(crate) FILTER_COEF_4(u32) @ 0x5c {
        31:24   tap16;
        23:16   tap15;
        15:8    tap14;
        7:0     tap13;
    }

    /// TODO
    pub(crate) FILTER_COEF_5(u32) @ 0x60 {
        31:24   tap20;
        23:16   tap19;
        15:8    tap18;
        7:0     tap17;
    }

    /// TODO
    pub(crate) FILTER_COEF_6(u32) @ 0x64 {
        23:16   tap23;
        15:8    tap22;
        7:0     tap21;
    }

    /// TODO
    pub(crate) UNKNOWN_AA_REG(u32) @ 0x68 {
        31:0    todo;
    }

    /// TODO
    pub(crate) CLOCK(u16) @ 0x6c {
        0:0 clock => Clock;
    }

    /// TODO
    pub(crate) DIV_STATUS(u16) @ 0x6e {
        1:0 visel;
    }

    /// TODO
    pub(crate) FBWIDTH(u16) @ 0x70 {
        9:0 width;
    }

    /// Used in debug video mode only!
    pub(crate) BORDER_BLANK_END(u16) @ 0x72 {
        /// Border Enable
        15:15   enable => bool;
        /// Border Horizontal Blank End
        9:0     hbe656;
    }

    /// Used in debug video mode only!
    pub(crate) BORDER_BLANK_START(u16) @ 0x74 {
        /// Border Horizontal Blank start
        9:0     hbs656;
    }
}
