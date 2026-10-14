// SPDX-License-Identifier: GPL-2.0 or MIT

//! Unified GX job submission.
//!
//! GX has a single command FIFO and a single set of state registers, but several things want to
//! use it: userspace clients submitting command streams, and the in-kernel conversion that turns
//! the fbdev RGB565 framebuffer into the YUYV image VI scans out. Everything that writes to the
//! FIFO does so while holding a [`GxGuard`], which serializes the clients and tells each of them
//! whether somebody else has run in between, so that it can set up its state again if needed.

use kernel::{
    new_mutex,
    prelude::*,
    sync::{
        atomic::{
            ordering::Relaxed,
            Atomic, //
        },
        Mutex,
        MutexGuard, //
    },
};

/// Who last used the GX pipeline.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum GxClient {
    /// Nobody has, or the state was reset.
    None,
    /// The framebuffer conversion in `gcnfb.c`.
    Framebuffer,
    /// A userspace client submitting through the DRM ioctls.
    User,
}

struct GxState {
    owner: GxClient,
}

/// The GX job queue, shared between the DRM side and `gcnfb.c`.
#[pin_data]
pub(crate) struct GxQueue {
    #[pin]
    state: Mutex<GxState>,
    /// The `struct drm_device` to notify about mode changes, or 0 while there is none.
    drm: Atomic<usize>,
}

/// Exclusive access to GX, released on drop.
pub(crate) struct GxGuard<'a> {
    _state: MutexGuard<'a, GxState>,
    state_lost: bool,
}

impl GxGuard<'_> {
    /// Whether another client used GX since this client's previous [`GxQueue::lock`].
    #[allow(dead_code)]
    pub(crate) fn state_lost(&self) -> bool {
        self.state_lost
    }
}

impl GxQueue {
    pub(crate) fn new() -> impl PinInit<Self> {
        pin_init!(Self {
            state <- new_mutex!(GxState { owner: GxClient::None }),
            drm: Atomic::new(0),
        })
    }

    /// Waits for exclusive access to GX on behalf of `client`.
    pub(crate) fn lock(&self, client: GxClient) -> GxGuard<'_> {
        let mut state = self.state.lock();
        let state_lost = state.owner != client;
        state.owner = client;
        GxGuard {
            _state: state,
            state_lost,
        }
    }

    pub(crate) fn set_drm(&self, drm: *mut kernel::bindings::drm_device) {
        self.drm.store(drm as usize, Relaxed);
    }

    pub(crate) fn drm(&self) -> *mut kernel::bindings::drm_device {
        self.drm.load(Relaxed) as *mut _
    }
}
