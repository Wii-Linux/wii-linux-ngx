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
#[repr(u32)]
enum ErrorCause {
    NoError = 0,
    MisalignedAddress = 1,
    IncorrectTransferType = 2,
    UnsupportedTransferSize = 3,
    AddressOutOfRange = 4,
    WriteToROM = 5,
    ReadFromGXFIFO = 6,
    Reserved = 7,
}

impl From<num::Bounded<u32, 3>> for ErrorCause {
    fn from(num: num::Bounded<u32, 3>) -> ErrorCause {
        match num.get() {
            0 => ErrorCause::NoError,
            1 => ErrorCause::MisalignedAddress,
            2 => ErrorCause::IncorrectTransferType,
            3 => ErrorCause::UnsupportedTransferSize,
            4 => ErrorCause::AddressOutOfRange,
            5 => ErrorCause::WriteToROM,
            6 => ErrorCause::ReadFromGXFIFO,
            7 => ErrorCause::Reserved,
            _ => unreachable!("ErrorCause can only have up to three bits!"),
        }
    }
}

impl From<ErrorCause> for num::Bounded<u32, 3> {
    fn from(error: ErrorCause) -> num::Bounded<u32, 3> {
        match error {
            ErrorCause::NoError => num::Bounded::<u32, 3>::new::<0>(),
            ErrorCause::MisalignedAddress => num::Bounded::<u32, 3>::new::<1>(),
            ErrorCause::IncorrectTransferType => num::Bounded::<u32, 3>::new::<2>(),
            ErrorCause::UnsupportedTransferSize => num::Bounded::<u32, 3>::new::<3>(),
            ErrorCause::AddressOutOfRange => num::Bounded::<u32, 3>::new::<4>(),
            ErrorCause::WriteToROM => num::Bounded::<u32, 3>::new::<5>(),
            ErrorCause::ReadFromGXFIFO => num::Bounded::<u32, 3>::new::<6>(),
            ErrorCause::Reserved => num::Bounded::<u32, 3>::new::<7>(),
        }
    }
}

#[derive(Debug)]
#[repr(u32)]
enum FlipperRevision {
    A = 0x046500B0,
    B = 0x146500B1,
    C = 0x246500B1,
    Unknown,
}

impl From<num::Bounded<u32, 32>> for FlipperRevision {
    fn from(num: num::Bounded<u32, 32>) -> FlipperRevision {
        match num.get() {
            0x046500B0 => FlipperRevision::A,
            0x146500B1 => FlipperRevision::B,
            0x246500B1 => FlipperRevision::C,
            _ => FlipperRevision::Unknown,
        }
    }
}

impl From<FlipperRevision> for num::Bounded<u32, 32> {
    fn from(rev: FlipperRevision) -> num::Bounded<u32, 32> {
        match rev {
            FlipperRevision::A => num::Bounded::<u32, 32>::new::<0x046500B0>(),
            FlipperRevision::B => num::Bounded::<u32, 32>::new::<0x146500B1>(),
            FlipperRevision::C => num::Bounded::<u32, 32>::new::<0x246500B1>(),
            FlipperRevision::Unknown => unreachable!("This should never happen!!!"),
        }
    }
}

impl FLIPPER_REV {
    pub(crate) const A: FLIPPER_REV = FLIPPER_REV { inner: 0x046500B0 };
    pub(crate) const B: FLIPPER_REV = FLIPPER_REV { inner: 0x146500B1 };
    pub(crate) const C: FLIPPER_REV = FLIPPER_REV { inner: 0x246500B1 };
}

register! {
    base: Region<0x100>;

    /// The cause of an interrupt.
    pub(crate) INTERRUPT_CAUSE(u32) @ 0x00 {
        /// ResetButtonState (1 = unpressed, 0 = pressed) it's a state, not maskable
        16:16   rst_button => bool;
        // There is no 15:15 here.
        /// Wii IPC
        14:14   wii_ipc => bool;
        /// High Speed Port (from sdram controller)
        13:13   hsp => bool;
        /// Debugger (from devkit)
        12:12   debug => bool;
        /// Command Fifo
        11:11   cp => bool;
        /// GP Finished
        10:10   pe_finish => bool;
        /// GP Token
        9:9     pe_token => bool;
        /// Video interface
        8:8     vi => bool;
        /// Memory interface
        7:7     memory => bool;
        /// DSP interface
        6:6     dsp => bool;
        /// Audio Interface Streaming
        5:5     ai => bool;
        /// Expansion interface
        4:4     exi => bool;
        /// Serial interface
        3:3     si => bool;
        /// DVD interrupt
        2:2     di => bool;
        /// Reset Switch
        1:1     rsw => bool;
        /// YAGCD says: GP runtime error
        0:0     pi => bool;
    }

    /// The mask of the interrupt.
    pub(crate) INTERRUPT_MASK(u32) @ 0x04 {
        /// Wii IPC
        14:14   wii_ipc => bool;
        /// High Speed Port (from sdram controller)
        13:13   hsp => bool;
        /// Debugger (from devkit)
        12:12   debug => bool;
        /// Command Fifo
        11:11   cp => bool;
        /// GP Finished
        10:10   pe_finish => bool;
        /// GP Token
        9:9     pe_token => bool;
        /// Video interface
        8:8     vi => bool;
        /// Memory interface
        7:7     memory => bool;
        /// DSP interface
        6:6     dsp => bool;
        /// Audio Interface Streaming
        5:5     ai => bool;
        /// Expansion interface
        4:4     exi => bool;
        /// Serial interface
        3:3     si => bool;
        /// DVD interrupt
        2:2     di => bool;
        /// Reset Switch
        1:1     rsw => bool;
        /// YAGCD says: GP runtime error
        0:0     pi => bool;
    }

    // There is no register at 0x08 I would know of.

    // All of these registers with addresses have a mask of 0x1fffffe0 on the Wii, and of 0x03ffffe0
    // on the GameCube, but since the Wii is a superset we encode it that way for now.
    //
    // We also say that the address goes to bit 0 instead of to bit 5, because that way users of
    // this API can pass direct addresses, instead of having to shift them right by 5 all the time.

    // These registers describe the PI FIFO: the bus-side target which burst writes to the FIFO
    // register block (0x0c008000) land in. It is not the write-gather pipe; that lives inside
    // the CPU, and merely issues its bursts to whatever address WPAR points at (normally here).
    // Any other store which reaches that block (plain stores, locked cache stores, etc.) feeds
    // the PI FIFO just the same.

    /// Address of the base of the PI FIFO.
    pub(crate) FIFO_BASE(u32) @ 0x0c {
        /// Address
        31:0 address;
    }

    /// Address of the end of the PI FIFO.
    pub(crate) FIFO_END(u32) @ 0x10 {
        /// Address
        31:0 address;
    }

    /// Address to which we will write next.
    pub(crate) FIFO_WPTR(u32) @ 0x14 {
        /// Address
        31:0 address;
    }

    /// TODO
    pub(crate) FIFO_RESET(u32) @ 0x18 {
        /// TODO
        31:0 reset;
    }

    /// The kind of error which happened.
    pub(crate) ERROR_CAUSE(u32) @ 0x1c {
        /// The cause of the error.
        2:0 cause => ErrorCause;
    }

    /// Address at which the error happened.
    pub(crate) ERROR_ADDRESS(u32) @ 0x20 {
        /// Address
        31:0 address;
    }

    /// TODO
    pub(crate) RESET_CODE(u32) @ 0x24 {
        /// TODO
        31:0 code;
    }

    pub(crate) UNKNOWN(u32) @ 0x28 {
        /// Only the write mask is known, at boot this register is set to 0x1ff, but it can go from
        /// 0x000 to 0x3ff.
        9:0 unknown;
    }

    /// The GPU revision in this console.
    ///
    /// The only known revisions are:
    /// - 0x046500B0 rev A
    /// - 0x146500B1 rev B
    /// - 0x246500B1 rev C
    pub(crate) FLIPPER_REV(u32) @ 0x2c {
        /// The revision.
        31:0 revision => FlipperRevision;
    }

    /// // BS1 writes 0x0245248A to it - controls the strength of the signal on the bus.  0 means
    /// the bus is dead and Flipper will not respond any longer, increasing it from the default
    /// value can reduce the noise in the Game Boy Player.
    pub(crate) FLIPPER_BUS_STRENGTH(u32) @ 0x30 {
        /// The bus strength.
        26:0 bus_strength;
    }
}
