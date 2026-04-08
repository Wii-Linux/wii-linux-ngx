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
enum CompareMode {
    Never = 0,
    Less = 1,
    Equal = 2,
    LEqual = 3,
    Greater = 4,
    NEqual = 5,
    GEqual = 6,
    Always = 7,
}

impl From<num::Bounded<u16, 3>> for CompareMode {
    fn from(num: num::Bounded<u16, 3>) -> CompareMode {
        match num.get() {
            0 => CompareMode::Never,
            1 => CompareMode::Less,
            2 => CompareMode::Equal,
            3 => CompareMode::LEqual,
            4 => CompareMode::Greater,
            5 => CompareMode::NEqual,
            6 => CompareMode::GEqual,
            7 => CompareMode::Always,
            _ => unreachable!("CompareMode can only have up to three bits!"),
        }
    }
}

impl From<CompareMode> for num::Bounded<u16, 3> {
    fn from(mode: CompareMode) -> num::Bounded<u16, 3> {
        match mode {
            CompareMode::Never => num::Bounded::<u16, 3>::new::<0>(),
            CompareMode::Less => num::Bounded::<u16, 3>::new::<1>(),
            CompareMode::Equal => num::Bounded::<u16, 3>::new::<2>(),
            CompareMode::LEqual => num::Bounded::<u16, 3>::new::<3>(),
            CompareMode::Greater => num::Bounded::<u16, 3>::new::<4>(),
            CompareMode::NEqual => num::Bounded::<u16, 3>::new::<5>(),
            CompareMode::GEqual => num::Bounded::<u16, 3>::new::<6>(),
            CompareMode::Always => num::Bounded::<u16, 3>::new::<7>(),
        }
    }
}

#[derive(Debug)]
#[repr(u16)]
enum SrcBlendFactor {
    Zero = 0,
    One = 1,
    DstClr = 2,
    InvDstClr = 3,
    SrcAlpha = 4,
    InvSrcAlpha = 5,
    DstAlpha = 6,
    InvDstAlpha = 7,
}

impl From<num::Bounded<u16, 3>> for SrcBlendFactor {
    fn from(num: num::Bounded<u16, 3>) -> SrcBlendFactor {
        match num.get() {
            0 => SrcBlendFactor::Zero,
            1 => SrcBlendFactor::One,
            2 => SrcBlendFactor::DstClr,
            3 => SrcBlendFactor::InvDstClr,
            4 => SrcBlendFactor::SrcAlpha,
            5 => SrcBlendFactor::InvSrcAlpha,
            6 => SrcBlendFactor::DstAlpha,
            7 => SrcBlendFactor::InvDstAlpha,
            _ => unreachable!("SrcBlendFactor can only have up to three bits!"),
        }
    }
}

impl From<SrcBlendFactor> for num::Bounded<u16, 3> {
    fn from(factor: SrcBlendFactor) -> num::Bounded<u16, 3> {
        match factor {
            SrcBlendFactor::Zero => num::Bounded::<u16, 3>::new::<0>(),
            SrcBlendFactor::One => num::Bounded::<u16, 3>::new::<1>(),
            SrcBlendFactor::DstClr => num::Bounded::<u16, 3>::new::<2>(),
            SrcBlendFactor::InvDstClr => num::Bounded::<u16, 3>::new::<3>(),
            SrcBlendFactor::SrcAlpha => num::Bounded::<u16, 3>::new::<4>(),
            SrcBlendFactor::InvSrcAlpha => num::Bounded::<u16, 3>::new::<5>(),
            SrcBlendFactor::DstAlpha => num::Bounded::<u16, 3>::new::<6>(),
            SrcBlendFactor::InvDstAlpha => num::Bounded::<u16, 3>::new::<7>(),
        }
    }
}

#[derive(Debug)]
#[repr(u16)]
enum DstBlendFactor {
    Zero = 0,
    One = 1,
    SrcClr = 2,
    InvSrcClr = 3,
    SrcAlpha = 4,
    InvSrcAlpha = 5,
    DstAlpha = 6,
    InvDstAlpha = 7,
}

impl From<num::Bounded<u16, 3>> for DstBlendFactor {
    fn from(factor: num::Bounded<u16, 3>) -> DstBlendFactor {
        match factor.get() {
            0 => DstBlendFactor::Zero,
            1 => DstBlendFactor::One,
            2 => DstBlendFactor::SrcClr,
            3 => DstBlendFactor::InvSrcClr,
            4 => DstBlendFactor::SrcAlpha,
            5 => DstBlendFactor::InvSrcAlpha,
            6 => DstBlendFactor::DstAlpha,
            7 => DstBlendFactor::InvDstAlpha,
            _ => unreachable!("DstBlendFactor can only have up to three bits!"),
        }
    }
}

impl From<DstBlendFactor> for num::Bounded<u16, 3> {
    fn from(factor: DstBlendFactor) -> num::Bounded<u16, 3> {
        match factor {
            DstBlendFactor::Zero => num::Bounded::<u16, 3>::new::<0>(),
            DstBlendFactor::One => num::Bounded::<u16, 3>::new::<1>(),
            DstBlendFactor::SrcClr => num::Bounded::<u16, 3>::new::<2>(),
            DstBlendFactor::InvSrcClr => num::Bounded::<u16, 3>::new::<3>(),
            DstBlendFactor::SrcAlpha => num::Bounded::<u16, 3>::new::<4>(),
            DstBlendFactor::InvSrcAlpha => num::Bounded::<u16, 3>::new::<5>(),
            DstBlendFactor::DstAlpha => num::Bounded::<u16, 3>::new::<6>(),
            DstBlendFactor::InvDstAlpha => num::Bounded::<u16, 3>::new::<7>(),
        }
    }
}

#[derive(Debug)]
#[repr(u16)]
enum LogicOp {
    Clear = 0,
    And = 1,
    AndReverse = 2,
    Copy = 3,
    AndInverted = 4,
    NoOp = 5,
    Xor = 6,
    Or = 7,
    Nor = 8,
    Equiv = 9,
    Invert = 10,
    OrReverse = 11,
    CopyInverted = 12,
    OrInverted = 13,
    Nand = 14,
    Set = 15,
}

impl From<num::Bounded<u16, 4>> for LogicOp {
    fn from(num: num::Bounded<u16, 4>) -> LogicOp {
        match num.get() {
            0 => LogicOp::Clear,
            1 => LogicOp::And,
            2 => LogicOp::AndReverse,
            3 => LogicOp::Copy,
            4 => LogicOp::AndInverted,
            5 => LogicOp::NoOp,
            6 => LogicOp::Xor,
            7 => LogicOp::Or,
            8 => LogicOp::Nor,
            9 => LogicOp::Equiv,
            10 => LogicOp::Invert,
            11 => LogicOp::OrReverse,
            12 => LogicOp::CopyInverted,
            13 => LogicOp::OrInverted,
            14 => LogicOp::Nand,
            15 => LogicOp::Set,
            _ => unreachable!("LogicOp can only have up to four bits!"),
        }
    }
}

impl From<LogicOp> for num::Bounded<u16, 4> {
    fn from(op: LogicOp) -> num::Bounded<u16, 4> {
        match op {
            LogicOp::Clear => num::Bounded::<u16, 4>::new::<0>(),
            LogicOp::And => num::Bounded::<u16, 4>::new::<1>(),
            LogicOp::AndReverse => num::Bounded::<u16, 4>::new::<2>(),
            LogicOp::Copy => num::Bounded::<u16, 4>::new::<3>(),
            LogicOp::AndInverted => num::Bounded::<u16, 4>::new::<4>(),
            LogicOp::NoOp => num::Bounded::<u16, 4>::new::<5>(),
            LogicOp::Xor => num::Bounded::<u16, 4>::new::<6>(),
            LogicOp::Or => num::Bounded::<u16, 4>::new::<7>(),
            LogicOp::Nor => num::Bounded::<u16, 4>::new::<8>(),
            LogicOp::Equiv => num::Bounded::<u16, 4>::new::<9>(),
            LogicOp::Invert => num::Bounded::<u16, 4>::new::<10>(),
            LogicOp::OrReverse => num::Bounded::<u16, 4>::new::<11>(),
            LogicOp::CopyInverted => num::Bounded::<u16, 4>::new::<12>(),
            LogicOp::OrInverted => num::Bounded::<u16, 4>::new::<13>(),
            LogicOp::Nand => num::Bounded::<u16, 4>::new::<14>(),
            LogicOp::Set => num::Bounded::<u16, 4>::new::<15>(),
        }
    }
}

#[derive(Debug)]
#[repr(u16)]
enum AlphaReadMode {
    /// Always read 0x00.
    Read00 = 0,

    /// Always read 0xFF.
    ReadFF = 1,

    /// Always read the real alpha value
    ReadNone = 2,

    /// TODO
    Unknown = 3,
}

impl From<num::Bounded<u16, 2>> for AlphaReadMode {
    fn from(num: num::Bounded<u16, 2>) -> AlphaReadMode {
        match num.get() {
            0 => AlphaReadMode::Read00,
            1 => AlphaReadMode::ReadFF,
            2 => AlphaReadMode::ReadNone,
            3 => AlphaReadMode::Unknown,
            _ => unreachable!("AlphaReadMode can only have up to two bits!"),
        }
    }
}

impl From<AlphaReadMode> for num::Bounded<u16, 2> {
    fn from(mode: AlphaReadMode) -> num::Bounded<u16, 2> {
        match mode {
            AlphaReadMode::Read00 => num::Bounded::<u16, 2>::new::<0>(),
            AlphaReadMode::ReadFF => num::Bounded::<u16, 2>::new::<1>(),
            AlphaReadMode::ReadNone => num::Bounded::<u16, 2>::new::<2>(),
            AlphaReadMode::Unknown => num::Bounded::<u16, 2>::new::<3>(),
        }
    }
}

register! {
    base: Region<0x100>;

    /// Depth configuration
    pub(crate) ZCONF(u16) @ 0x00 {
        4:4 z_update_enable => bool;
        3:1 function => CompareMode;
        0:0 z_comparator_enable => bool;
    }

    /// Alpha configuration
    pub(crate) ALPHA_CONF(u16) @ 0x02 {
        15:12   logic_op => LogicOp;
        11:11   subtract => bool;
        10:8    src_factor => SrcBlendFactor;
        7:5     dst_factor => DstBlendFactor;
        4:4     alpha_update_enable => bool;
        3:3     color_update_enable => bool;
        2:2     dither => bool;
        1:1     logic => bool;
        0:0     blend => bool;
    }

    /// TODO
    pub(crate) DST_ALPHA_CONF(u16) @ 0x04 {
        8:8 enable => bool;
        7:0 alpha => u8;
    }

    /// Alpha mode configuration
    pub(crate) ALPHA_MODE(u16) @ 0x06 {
        10:8    compare_mode => CompareMode;
        7:0     threshold => u8;
    }

    /// Alpha read
    pub(crate) ALPHA_READ(u16) @ 0x08 {
        1:0 read_mode => AlphaReadMode;
    }

    /// TODO
    pub(crate) CONTROL(u16) @ 0x0a {
        3:3 finish => bool;
        2:2 token => bool;
        1:1 finish_enable => bool;
        0:0 token_enable => bool;
    }

    // TODO: Figure out if there is a register 0x0c.

    /// TODO
    pub(crate) TOKEN(u16) @ 0x0e {
        15:0    token;
    }

    /// The leftmost pixel drawn since the previous bounding-box clear.
    ///
    /// Since the PE works only on 2×2 blocks, this value is always even.
    pub(crate) BBOX_LEFT(u16) @ 0x10 {
        9:0 bbox;
    }

    /// The rightmost pixel drawn since the previous bounding-box clear.
    ///
    /// Since the PE works only on 2×2 blocks, this value is always even.
    pub(crate) BBOX_RIGHT(u16) @ 0x12 {
        9:0 bbox;
    }

    /// The topmost pixel drawn since the previous bounding-box clear.
    ///
    /// Since the PE works only on 2×2 blocks, this value is always odd.
    pub(crate) BBOX_TOP(u16) @ 0x14 {
        9:0 bbox;
    }

    /// The bottommost pixel drawn since the previous bounding-box clear.
    ///
    /// Since the PE works only on 2×2 blocks, this value is always odd.
    pub(crate) BBOX_BOTTOM(u16) @ 0x16 {
        9:0 bbox;
    }

    // TODO: Add the remaining perf registers.
}
