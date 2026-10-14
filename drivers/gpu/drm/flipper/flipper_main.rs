// SPDX-License-Identifier: GPL-2.0 or MIT

//! Nintendo Flipper DRM driver.

use crate::driver::FlipperPlatformDriver;

mod driver;
mod file;
mod gcnfb;
mod gem;
mod gx;
mod regs;

kernel::module_platform_driver! {
    type: FlipperPlatformDriver,
    name: "flipper",
    authors: ["Link Mauve <linkmauve@linkmauve.fr>"],
    description: "Nintendo Flipper DRM driver",
    license: "Dual MIT/GPL",
}
