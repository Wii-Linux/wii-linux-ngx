// SPDX-License-Identifier: GPL-2.0 or MIT

//! Glue to `gcnfb.c` and `flipper_kms.c`.
//!
//! `gcnfb.c` is the GX and VI bring-up code that used to be the gcnfb framebuffer driver: GX
//! initialization, VI timings, the Wii's A/V encoder, the VI and PE interrupts and the GX based
//! conversion of the fbdev framebuffer. `flipper_kms.c` is the small KMS pipeline that gives the
//! fbdev emulation something to drive.

use core::ptr::NonNull;

use kernel::{
    bindings,
    error::{
        from_err_ptr,
        to_result, //
    },
    ffi::{
        c_int,
        c_void, //
    },
    io::{
        mem::IoMem,
        IoBackend,
        IoBase,
        MmioBackend, //
    },
    prelude::*,
    sync::Arc,
};

use crate::gx::{
    GxClient,
    GxQueue, //
};

/// Register blocks owned by the DRM side, as seen by `gcnfb.c`.
#[repr(C)]
pub(crate) struct Regs {
    pub(crate) cp: *mut c_void,
    pub(crate) pe: *mut c_void,
    pub(crate) vi: *mut c_void,
    pub(crate) pi: *mut c_void,
    pub(crate) wgpipe: *mut c_void,
}

impl Regs {
    /// Returns the `__iomem` pointer of a mapping.
    pub(crate) fn raw<const SIZE: usize>(io: &IoMem<'_, SIZE>) -> *mut c_void {
        <MmioBackend as IoBackend>::as_ptr(io.as_view()).cast()
    }
}

type GxJob = unsafe extern "C" fn(data: *mut c_void, state_lost: bool) -> c_int;

#[repr(C)]
struct Ops {
    gx_submit: unsafe extern "C" fn(
        priv_: *mut c_void,
        job: Option<GxJob>,
        data: *mut c_void,
    ) -> c_int,
    mode_changed: Option<unsafe extern "C" fn(priv_: *mut c_void)>,
}

// SAFETY: `Ops` only contains function pointers.
unsafe impl Sync for Ops {}

// The bindings types are only ever passed around by pointer.
#[allow(improper_ctypes)]
extern "C" {
    fn gcnfb_create(
        dev: *mut bindings::device,
        regs: *const Regs,
        ops: *const Ops,
        priv_: *mut c_void,
    ) -> *mut c_void;
    fn gcnfb_destroy(ctl: *mut c_void);
    fn gcnfb_shutdown(ctl: *mut c_void);
    fn gcnfb_user_output(ctl: *mut c_void);
    fn gcnfb_user_fence_arm(ctl: *mut c_void) -> c_int;
    fn gcnfb_user_fence_wait(ctl: *mut c_void) -> c_int;

    fn flipper_kms_init(drm: *mut bindings::drm_device, ctl: *mut c_void) -> c_int;
    fn flipper_kms_register(drm: *mut bindings::drm_device);
    fn flipper_kms_mode_changed(drm: *mut bindings::drm_device);
    fn flipper_kms_shutdown(drm: *mut bindings::drm_device);
}

/// Runs a job from `gcnfb.c` with exclusive access to GX.
///
/// # Safety
///
/// `priv_` must point to a live [`GxQueue`], and `job` must be callable with `data`.
unsafe extern "C" fn gx_submit(priv_: *mut c_void, job: Option<GxJob>, data: *mut c_void) -> c_int {
    // SAFETY: By the safety requirements, `priv_` points to a live `GxQueue`.
    let queue = unsafe { &*priv_.cast::<GxQueue>() };
    let Some(job) = job else {
        return EINVAL.to_errno();
    };

    let guard = queue.lock(GxClient::Framebuffer);
    // SAFETY: By the safety requirements, `job` may be called with `data`.
    unsafe { job(data, guard.state_lost()) }
}

/// # Safety
///
/// `priv_` must point to a live [`GxQueue`].
unsafe extern "C" fn mode_changed(priv_: *mut c_void) {
    // SAFETY: By the safety requirements, `priv_` points to a live `GxQueue`.
    let queue = unsafe { &*priv_.cast::<GxQueue>() };
    let drm = queue.drm();
    if !drm.is_null() {
        // SAFETY: The pointer is cleared before the DRM device can go away.
        unsafe { flipper_kms_mode_changed(drm) };
    }
}

static OPS: Ops = Ops {
    gx_submit,
    mode_changed: Some(mode_changed),
};

/// The `gcnfb.c` context: GX and VI brought up, VI and PE interrupts live.
///
/// # Invariants
///
/// `ctl` was returned by `gcnfb_create()` and is destroyed on drop. The register mappings and the
/// [`GxQueue`] passed to `gcnfb_create()` outlive it.
pub(crate) struct Gcnfb {
    ctl: NonNull<c_void>,
}

// SAFETY: The context is internally synchronized.
unsafe impl Send for Gcnfb {}
// SAFETY: The context is internally synchronized.
unsafe impl Sync for Gcnfb {}

impl Gcnfb {
    /// Brings up GX and VI. Scanout stays off until the display pipeline enables it.
    ///
    /// # Safety
    ///
    /// `drm` must be a valid DRM device whose parent is a bound platform device, and the
    /// mappings in `regs` and the `queue` must outlive the returned [`Gcnfb`], which has to be
    /// dropped before them.
    pub(crate) unsafe fn new(
        drm: *mut bindings::drm_device,
        regs: &Regs,
        queue: &Arc<GxQueue>,
    ) -> Result<Self> {
        let priv_ = Arc::as_ptr(queue).cast_mut().cast::<c_void>();
        // SAFETY: By the safety requirements, `drm` is valid; so is its parent device.
        let dev = unsafe { (*drm).dev };
        // SAFETY: `dev` is a bound platform device, `regs` and `OPS` are valid for the call and
        // `priv_` points to a `GxQueue` which the caller keeps alive.
        let ctl = from_err_ptr(unsafe { gcnfb_create(dev, regs, &OPS, priv_) })?;
        // INVARIANT: `gcnfb_create()` returns an error pointer or a valid context.
        Ok(Self {
            ctl: NonNull::new(ctl).ok_or(EINVAL)?,
        })
    }

    /// Sets up the display pipeline. Must be called before the DRM device is registered.
    pub(crate) fn init_kms(&self, drm: *mut bindings::drm_device) -> Result {
        // SAFETY: `drm` is a valid, not yet registered DRM device and `self.ctl` is a valid context.
        to_result(unsafe { flipper_kms_init(drm, self.ctl.as_ptr()) })
    }

    /// Stops scanout and the VI interrupt, e.g. at system shutdown.
    #[allow(dead_code)]
    pub(crate) fn shutdown(&self) {
        // SAFETY: `self.ctl` is a valid context.
        unsafe { gcnfb_shutdown(self.ctl.as_ptr()) };
    }
}

/// Userspace GX submissions. These must only be called while holding the [`GxQueue`].
impl Gcnfb {
    /// Userspace rendered to the XFB: stop converting the fbdev framebuffer over it.
    pub(crate) fn mark_user_output(&self) {
        // SAFETY: `self.ctl` is a valid context.
        unsafe { gcnfb_user_output(self.ctl.as_ptr()) };
    }

    /// Arms the completion that [`Self::fence_wait()`] waits for.
    pub(crate) fn fence_arm(&self) -> Result {
        // SAFETY: `self.ctl` is a valid context.
        to_result(unsafe { gcnfb_user_fence_arm(self.ctl.as_ptr()) })
    }

    /// Flushes the write-gather pipe and waits for the `PE_DONE` written since [`Self::fence_arm()`].
    pub(crate) fn fence_wait(&self) -> Result {
        // SAFETY: `self.ctl` is a valid context.
        to_result(unsafe { gcnfb_user_fence_wait(self.ctl.as_ptr()) })
    }
}

impl Drop for Gcnfb {
    fn drop(&mut self) {
        // SAFETY: By the type invariants, `self.ctl` is valid and not used again.
        unsafe { gcnfb_destroy(self.ctl.as_ptr()) };
    }
}

/// Starts the in-kernel DRM clients (the fbdev console).
///
/// # Safety
///
/// `drm` must be a registered DRM device set up with [`Gcnfb::init_kms()`].
pub(crate) unsafe fn start_clients(drm: *mut bindings::drm_device) {
    // SAFETY: By the safety requirements.
    unsafe { flipper_kms_register(drm) };
}

/// Turns all outputs off.
///
/// # Safety
///
/// `drm` must be a DRM device set up with [`Gcnfb::init_kms()`], and the matching [`Gcnfb`] must
/// still be alive.
pub(crate) unsafe fn stop_outputs(drm: *mut bindings::drm_device) {
    // SAFETY: By the safety requirements.
    unsafe { flipper_kms_shutdown(drm) };
}
