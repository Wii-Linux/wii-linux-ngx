// SPDX-License-Identifier: GPL-2.0 or MIT

use kernel::{
    drm::gem,
    prelude::*, //
};

use crate::driver::{
    FlipperDrmDevice,
    FlipperDrmDriver, //
};

/// Flipper’s DriverObject type for GEM objects.
#[pin_data]
pub(crate) struct BoData {
    flags: u32,
}

/// Provides a way to pass arguments when creating BoData
/// as required by the gem::DriverObject trait.
pub(crate) struct BoCreateArgs {
    flags: u32,
}

impl gem::DriverObject for BoData {
    type Driver = FlipperDrmDriver;
    type Args = BoCreateArgs;

    fn new(_dev: &FlipperDrmDevice, _size: usize, args: BoCreateArgs) -> impl PinInit<Self, Error> {
        try_pin_init!(Self { flags: args.flags })
    }
}

/// Type alias for Flipper GEM buffer objects.
pub(crate) type Bo = gem::shmem::Object<BoData>;
