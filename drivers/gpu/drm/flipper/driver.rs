// SPDX-License-Identifier: GPL-2.0 or MIT

use core::arch::asm;
use kernel::{
    bindings,
    device::{
        Bound,
        Core,
        DeviceContext, //
    },
    drm,
    drm::ioctl,
    io::{
        mem::IoMem,
        BigEndianMmio,
        Io,
        IoBase,
        Region, //
    },
    new_mutex,
    of,
    platform,
    prelude::*,
    sync::{
        Arc,
        Mutex, //
    }, //
};

use crate::{
    file::FlipperDrmFileData,
    gem::Bo,
    regs, //
};

pub(crate) struct FlipperDrmDriver;

/// Convenience type alias for the DRM device type for this driver.
pub(crate) type FlipperDrmDevice<Ctx = drm::Normal> = drm::Device<FlipperDrmDriver, Ctx>;

pub(crate) struct FlipperPlatformDriver;

#[pin_data(PinnedDrop)]
pub(crate) struct FlipperPlatformDriverData<'bound> {
    #[pin]
    _reg: drm::Registration<'bound, FlipperDrmDriver>,
}

/// Data owned by the DRM [`Registration`].
///
/// This data can have references tied to the parent platform device binding scope
/// and is accessible only while the DRM device is registered with userspace.
#[pin_data]
pub(crate) struct FlipperDrmRegistrationData<'drm> {
    /// Parent platform device.
    pub(crate) pdev: &'drm platform::Device<Bound>,

    pub(crate) cp_regs: Arc<IoMem<'drm, 0x100>>,
    pub(crate) pe_regs: Arc<IoMem<'drm, 0x100>>,
    pub(crate) vi_regs: Arc<IoMem<'drm, 0x100>>,
    pub(crate) pi_regs: Arc<IoMem<'drm, 0x100>>,
    pub(crate) wg_pipe: Arc<IoMem<'drm, 4>>,
    //pub(crate) fifo: NonNull<[u8]>,
    //pub(crate) xfb: NonNull<[u8]>,
    /// Embedded framebuffer, the VRAM pretty much.
    pub(crate) efb: Arc<IoMem<'drm, 0x400000>>,

    #[pin]
    pub(crate) pos_vec: Mutex<KVec<u8>>,

    #[pin]
    pub(crate) color_vec: Mutex<KVec<u8>>,
}

macro_rules! mfspr {
    ($R:tt) => {{
        let mut output: u32;
        // SAFETY: TODO
        unsafe { asm!(concat!("mfspr {0}, ", stringify!($R)), out(reg) output, options(nostack)) };
        output
    }}
}

macro_rules! mtspr {
    ($R:tt, $v:tt) => {{
        let input: u32 = $v;
        // SAFETY: TODO
        unsafe { asm!(concat!("mtspr ", stringify!($R), ", {0}"), in(reg) input, options(nostack)) };
    }}
}

/*
fn bp(wg_pipe: &IoMem<4>, reg: u8, value: u32) {
    let data = ((reg as u32) << 24) | (value & 0x00ffffff);
    wg_pipe.write8(0x61, 0);
    wg_pipe.write32(data, 0);
}

fn xf(wg_pipe: &IoMem<4>, reg: u16, values: &[u32]) {
    wg_pipe.write8(0x10, 0);
    wg_pipe.write16((values.len() - 1) as u16, 0);
    wg_pipe.write16(reg, 0);
    for value in values {
        wg_pipe.write32(*value, 0);
    }
}

fn draw_triangle(wg_pipe: &IoMem<4>, vertices: &[[u8; 2]]) {
    wg_pipe.write8(0x90, 0);
    wg_pipe.write16(vertices.len() as u16, 0);
    for vertex in vertices {
        wg_pipe.write8(vertex[0], 0);
        wg_pipe.write8(vertex[1], 0);
    }
}
*/

pub(crate) fn print_cp_regs(cp: &BigEndianMmio<'_, Region<0x100>>) {
    let reg_00 = cp.read(regs::cp::STATUS);
    let reg_02 = cp.read(regs::cp::CONTROL);
    let reg_04 = cp.read(regs::cp::CLEAR);
    let reg_0e = cp.read(regs::cp::TOKEN);
    let reg_20 = cp.read16(0x20);
    let reg_22 = cp.read16(0x22);
    let reg_24 = cp.read16(0x24);
    let reg_26 = cp.read16(0x26);
    let reg_28 = cp.read16(0x28);
    let reg_2a = cp.read16(0x2a);
    let reg_2c = cp.read16(0x2c);
    let reg_2e = cp.read16(0x2e);
    let reg_30 = cp.read16(0x30);
    let reg_32 = cp.read16(0x32);
    let reg_34 = cp.read16(0x34);
    let reg_36 = cp.read16(0x36);
    let reg_38 = cp.read16(0x38);
    let reg_3a = cp.read16(0x3a);
    let reg_3c = cp.read16(0x3c);
    let reg_3e = cp.read16(0x3e);
    pr_info!("CP: 00: {reg_00:?}\n");
    pr_info!("CP: 02: {reg_02:?}\n");
    pr_info!("CP: 04: {reg_04:?}\n");
    pr_info!("CP: 0e: {reg_0e:?}\n");
    pr_info!("CP: 20: 0x{reg_22:04x}{reg_20:04x}\n");
    pr_info!("CP: 24: 0x{reg_26:04x}{reg_24:04x}\n");
    pr_info!("CP: 28: 0x{reg_2a:04x}{reg_28:04x}\n");
    pr_info!("CP: 2c: 0x{reg_2e:04x}{reg_2c:04x}\n");
    pr_info!("CP: 30: 0x{reg_32:04x}{reg_30:04x}\n");
    pr_info!("CP: 34: 0x{reg_36:04x}{reg_34:04x}\n");
    pr_info!("CP: 38: 0x{reg_3a:04x}{reg_38:04x}\n");
    pr_info!("CP: 3c: 0x{reg_3e:04x}{reg_3c:04x}\n");
}

pub(crate) fn print_pe_regs(pe: &BigEndianMmio<'_, Region<0x100>>) {
    let reg_00 = pe.read(regs::pe::ZCONF);
    let reg_02 = pe.read(regs::pe::ALPHA_CONF);
    let reg_04 = pe.read(regs::pe::DST_ALPHA_CONF);
    let reg_06 = pe.read(regs::pe::ALPHA_MODE);
    let reg_08 = pe.read(regs::pe::ALPHA_READ);
    let reg_0a = pe.read(regs::pe::CONTROL);
    let reg_0e = pe.read(regs::pe::TOKEN);
    let reg_10 = pe.read(regs::pe::BBOX_LEFT);
    let reg_12 = pe.read(regs::pe::BBOX_RIGHT);
    let reg_14 = pe.read(regs::pe::BBOX_TOP);
    let reg_16 = pe.read(regs::pe::BBOX_BOTTOM);
    pr_info!("PE: 00: {reg_00:?}\n");
    pr_info!("PE: 02: {reg_02:?}\n");
    pr_info!("PE: 04: {reg_04:?}\n");
    pr_info!("PE: 06: {reg_06:?}\n");
    pr_info!("PE: 08: {reg_08:?}\n");
    pr_info!("PE: 0a: {reg_0a:?}\n");
    pr_info!("PE: 0e: {reg_0e:?}\n");
    pr_info!("PE: 10: {reg_10:?}\n");
    pr_info!("PE: 12: {reg_12:?}\n");
    pr_info!("PE: 14: {reg_14:?}\n");
    pr_info!("PE: 16: {reg_16:?}\n");
}

pub(crate) fn print_pi_regs(pi: &BigEndianMmio<'_, Region<0x100>>) {
    let reg_00 = pi.read(regs::pi::INTERRUPT_CAUSE);
    let reg_04 = pi.read(regs::pi::INTERRUPT_MASK);
    // There is no reg_08.
    let reg_0c = pi.read(regs::pi::FIFO_BASE);
    let reg_10 = pi.read(regs::pi::FIFO_END);
    let reg_14 = pi.read(regs::pi::FIFO_WPTR);
    let reg_18 = pi.read(regs::pi::FIFO_RESET);
    let reg_1c = pi.read(regs::pi::ERROR_CAUSE);
    let reg_20 = pi.read(regs::pi::ERROR_ADDRESS);
    let reg_24 = pi.read(regs::pi::RESET_CODE);
    let reg_28 = pi.read(regs::pi::UNKNOWN);
    let reg_2c = pi.read(regs::pi::FLIPPER_REV);
    let reg_30 = pi.read(regs::pi::FLIPPER_BUS_STRENGTH);
    pr_info!("PI: 00: {reg_00:?}\n");
    pr_info!("PI: 04: {reg_04:?}\n");
    pr_info!("PI: 0c: {reg_0c:x?}\n");
    pr_info!("PI: 10: {reg_10:x?}\n");
    pr_info!("PI: 14: {reg_14:x?}\n");
    pr_info!("PI: 18: {reg_18:x?}\n");
    pr_info!("PI: 1c: {reg_1c:?}\n");
    pr_info!("PI: 20: {reg_20:x?}\n");
    pr_info!("PI: 24: {reg_24:?}\n");
    pr_info!("PI: 28: {reg_28:x?}\n");
    pr_info!("PI: 2c: {reg_2c:?}\n");
    pr_info!("PI: 30: {reg_30:x?}\n");
}

pub(crate) fn print_vi_regs(vi: &BigEndianMmio<'_, Region<0x100>>) {
    let reg_00 = vi.read(regs::vi::VERTICAL_TIMING);
    let reg_02 = vi.read(regs::vi::CONTROL);
    let reg_04 = vi.read(regs::vi::HORIZONTAL_TIMING_0);
    let reg_08 = vi.read(regs::vi::HORIZONTAL_TIMING_1);
    let reg_0c = vi.read(regs::vi::VBLANK_TIMING_ODD);
    let reg_10 = vi.read(regs::vi::VBLANK_TIMING_EVEN);
    let reg_14 = vi.read(regs::vi::VBLANK_BURST_BLANKING_ODD);
    let reg_18 = vi.read(regs::vi::VBLANK_BURST_BLANKING_EVEN);
    let reg_1c = vi.read(regs::vi::FB_LEFT_TOP);
    let reg_20 = vi.read(regs::vi::FB_RIGHT_TOP);
    let reg_24 = vi.read(regs::vi::FB_LEFT_BOTTOM);
    let reg_28 = vi.read(regs::vi::FB_RIGHT_BOTTOM);
    let reg_2c = vi.read(regs::vi::VERTICAL_BEAM_POSITION);
    let reg_2e = vi.read(regs::vi::HORIZONTAL_BEAM_POSITION);
    let reg_30 = vi.read(regs::vi::DISPLAY_INTERRUPT_0);
    let reg_34 = vi.read(regs::vi::DISPLAY_INTERRUPT_1);
    let reg_38 = vi.read(regs::vi::DISPLAY_INTERRUPT_2);
    let reg_3c = vi.read(regs::vi::DISPLAY_INTERRUPT_3);
    let reg_40 = vi.read(regs::vi::DISPLAY_LATCH_0);
    let reg_44 = vi.read(regs::vi::DISPLAY_LATCH_1);
    let reg_48 = vi.read(regs::vi::HSCALEW);
    let reg_4a = vi.read(regs::vi::HSCALER);
    let reg_4c = vi.read(regs::vi::FILTER_COEF_0);
    let reg_50 = vi.read(regs::vi::FILTER_COEF_1);
    let reg_54 = vi.read(regs::vi::FILTER_COEF_2);
    let reg_58 = vi.read(regs::vi::FILTER_COEF_3);
    let reg_5c = vi.read(regs::vi::FILTER_COEF_4);
    let reg_60 = vi.read(regs::vi::FILTER_COEF_5);
    let reg_64 = vi.read(regs::vi::FILTER_COEF_6);
    let reg_68 = vi.read(regs::vi::UNKNOWN_AA_REG);
    let reg_6c = vi.read(regs::vi::CLOCK);
    let reg_6e = vi.read(regs::vi::DIV_STATUS);
    let reg_70 = vi.read(regs::vi::FBWIDTH);
    let reg_72 = vi.read(regs::vi::BORDER_BLANK_END);
    let reg_74 = vi.read(regs::vi::BORDER_BLANK_START);
    pr_info!("VI: 00: {reg_00:?}\n");
    pr_info!("VI: 02: {reg_02:?}\n");
    pr_info!("VI: 04: {reg_04:?}\n");
    pr_info!("VI: 08: {reg_08:?}\n");
    pr_info!("VI: 0c: {reg_0c:?}\n");
    pr_info!("VI: 10: {reg_10:?}\n");
    pr_info!("VI: 14: {reg_14:?}\n");
    pr_info!("VI: 18: {reg_18:?}\n");
    pr_info!("VI: 1c: {reg_1c:?}\n");
    pr_info!("VI: 20: {reg_20:?}\n");
    pr_info!("VI: 24: {reg_24:?}\n");
    pr_info!("VI: 28: {reg_28:?}\n");
    pr_info!("VI: 2c: {reg_2c:?}\n");
    pr_info!("VI: 2e: {reg_2e:?}\n");
    pr_info!("VI: 30: {reg_30:?}\n");
    pr_info!("VI: 34: {reg_34:?}\n");
    pr_info!("VI: 38: {reg_38:?}\n");
    pr_info!("VI: 3c: {reg_3c:?}\n");
    pr_info!("VI: 40: {reg_40:?}\n");
    pr_info!("VI: 44: {reg_44:?}\n");
    pr_info!("VI: 48: {reg_48:?}\n");
    pr_info!("VI: 4a: {reg_4a:?}\n");
    pr_info!("VI: 4c: {reg_4c:?}\n");
    pr_info!("VI: 50: {reg_50:?}\n");
    pr_info!("VI: 54: {reg_54:?}\n");
    pr_info!("VI: 58: {reg_58:?}\n");
    pr_info!("VI: 5c: {reg_5c:?}\n");
    pr_info!("VI: 60: {reg_60:?}\n");
    pr_info!("VI: 64: {reg_64:?}\n");
    pr_info!("VI: 68: {reg_68:?}\n");
    pr_info!("VI: 6c: {reg_6c:?}\n");
    pr_info!("VI: 6e: {reg_6e:?}\n");
    pr_info!("VI: 70: {reg_70:?}\n");
    pr_info!("VI: 72: {reg_72:?}\n");
    pr_info!("VI: 74: {reg_74:?}\n");
}

fn setup_fifo(
    pdev: &platform::Device<Core<'_>>,
    //fifo: &NonNull<[u8]>,
    cp_regs: &Arc<IoMem<'_, 256>>,
    pe_regs: &Arc<IoMem<'_, 256>>,
    pi_regs: &Arc<IoMem<'_, 256>>,
    vi_regs: &Arc<IoMem<'_, 256>>,
    wg_pipe: &Arc<IoMem<'_, 4>>,
) -> Result<u32> {
    // Enable the write-gather pipe (HID2[WPE]). Once enabled, the CPU sees the write-gather
    // pipe, not the PI FIFO, at the address in WPAR: stores there are collected by the pipe, and
    // it issues burst transactions to that address once enough data has accumulated.
    let hid2 = mfspr!(920) | 0x4000_0000;
    dev_info!(pdev, "HID2 = 0x{hid2:08x}");
    mtspr!(920, hid2);

    // Point the write-gather pipe's bursts at the PI FIFO register block (0x0c008000). That
    // is where the pipe's output lands on the bus; the PI FIFO itself is what's there, from the
    // bus's perspective. Hardcoded in the device-tree…
    mtspr!(921, 0x0c008000);
    const FIFO_BASE: u32 = 0x01734000;
    const FIFO_BASE_LO: u16 = (FIFO_BASE & 0xffff) as u16;
    const FIFO_BASE_HI: u16 = (FIFO_BASE >> 16) as u16;
    const FIFO_END: u32 = 0x01774000;
    const FIFO_END_LO: u16 = (FIFO_END & 0xffff) as u16;
    const FIFO_END_HI: u16 = (FIFO_END >> 16) as u16;
    const HI_WATERMARK: u32 = (FIFO_END - FIFO_BASE) * 3 / 4;
    const HI_WATERMARK_LO: u16 = (HI_WATERMARK & 0xffff) as u16;
    const HI_WATERMARK_HI: u16 = (HI_WATERMARK >> 16) as u16;

    /*
    let fifo_start = unsafe { vmalloc_to_phys(fifo.as_ptr() as *mut c_void) };
    let fifo_end = fifo_start + fifo.len() as u32;
    let hi_watermark = (fifo_end - fifo_start) * 3 / 4;
    dev_info!(pdev, "0x{fifo_start:08x} to 0x{fifo_end:08x}\n");
    */

    let cp = cp_regs.as_view().big_endian();
    let pe = pe_regs.as_view().big_endian();
    let pi = pi_regs.as_view().big_endian();
    let vi = vi_regs.as_view().big_endian();
    let wg_pipe = wg_pipe.as_view().big_endian();

    print_cp_regs(&cp);
    print_pe_regs(&pe);
    print_pi_regs(&pi);
    print_vi_regs(&vi);

    // FIFO Control
    cp.write_reg(regs::cp::STATUS::zeroed());

    // FIFO Clear
    cp.write_reg(
        regs::cp::CLEAR::zeroed()
            .with_metrics(true)
            .with_fifo_underflow(true)
            .with_fifo_overflow(true),
    );

    // FIFO Start
    cp.write_reg(regs::cp::FIFO_BASE_LO::zeroed().with_const_lo::<FIFO_BASE_LO>());
    cp.write_reg(regs::cp::FIFO_BASE_HI::zeroed().with_const_hi::<FIFO_BASE_HI>());

    // FIFO End
    cp.write_reg(regs::cp::FIFO_END_LO::zeroed().with_const_lo::<FIFO_END_LO>());
    cp.write_reg(regs::cp::FIFO_END_HI::zeroed().with_const_hi::<FIFO_END_HI>());

    // Set watermarks, high at 75%, low at 0%
    cp.write_reg(regs::cp::FIFO_HI_WATERMARK_LO::zeroed().with_const_lo::<HI_WATERMARK_LO>());
    cp.write_reg(regs::cp::FIFO_HI_WATERMARK_HI::zeroed().with_const_hi::<HI_WATERMARK_HI>());
    cp.write_reg(regs::cp::FIFO_LO_WATERMARK_LO::zeroed());
    cp.write_reg(regs::cp::FIFO_LO_WATERMARK_HI::zeroed());

    // Set R/W pointers to fifo start
    cp.write_reg(regs::cp::FIFO_RW_DISTANCE_LO::zeroed());
    cp.write_reg(regs::cp::FIFO_RW_DISTANCE_HI::zeroed());
    cp.write_reg(regs::cp::FIFO_WRITE_POINTER_LO::zeroed().with_const_lo::<FIFO_BASE_LO>());
    cp.write_reg(regs::cp::FIFO_WRITE_POINTER_HI::zeroed().with_const_hi::<FIFO_BASE_HI>());
    cp.write_reg(regs::cp::FIFO_READ_POINTER_LO::zeroed().with_const_lo::<FIFO_BASE_LO>());
    cp.write_reg(regs::cp::FIFO_READ_POINTER_HI::zeroed().with_const_hi::<FIFO_BASE_HI>());

    // Set the PI FIFO bounds (this is where the write-gather pipe's bursts end up)
    pi.write_reg(regs::pi::FIFO_BASE::zeroed().with_const_address::<FIFO_BASE>());
    pi.write_reg(regs::pi::FIFO_END::zeroed().with_const_address::<FIFO_END>());

    // Set the PI FIFO write pointer
    pi.write_reg(regs::pi::FIFO_WPTR::zeroed().with_const_address::<FIFO_BASE>());
    for _ in 0..7 {
        wg_pipe.write32(0, 0);
    }
    wg_pipe.write16(0, 0);
    wg_pipe.write8(0, 0);
    pi.write_reg(regs::pi::FIFO_WPTR::zeroed().with_const_address::<FIFO_BASE>());

    /*
    // Draw a triangle!
    bp(wg_pipe, 0x40, 0x000017);
    bp(wg_pipe, 0x41, 0x0004bc);
    bp(wg_pipe, 0x43, 0x000040);
    for _ in 0..8 {
        wg_pipe.write32(0, 0);
    }
    xf(
        wg_pipe,
        0x101a,
        &[
            0x43a00000, 0xc3700000, 0x4b7fffff, 0x44258000, 0x44118000, 0x4b7fffff,
        ],
    );
    wg_pipe.write32(0x48, 0);
    bp(wg_pipe, 0x0f, 0x000000);
    bp(wg_pipe, 0x66, 0x001000);
    bp(wg_pipe, 0x66, 0x001100);
    bp(wg_pipe, 0x0f, 0x000000);
    xf(
        wg_pipe,
        0x0000,
        &[
            0x3f7fffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3f7ffffe, 0x00000000,
            0x00000000, 0x00000000, 0x00000000, 0x3f7fffff, 0xc247ffff,
        ],
    );
    draw_triangle(wg_pipe, &[[0, 0], [1, 1], [2, 2]]);
    bp(wg_pipe, 0x45, 0x000002);
    for _ in 0..8 {
        wg_pipe.write32(0, 0);
    }
    bp(wg_pipe, 0x40, 0x000017);
    bp(wg_pipe, 0x41, 0x0004bc);
    bp(wg_pipe, 0x40, 0x00001f);
    bp(wg_pipe, 0x41, 0x0004bc);
    bp(wg_pipe, 0x43, 0x000000);
    bp(wg_pipe, 0x49, 0x000000);
    bp(wg_pipe, 0x4a, 0x077e7f);
    bp(wg_pipe, 0x4d, 0x000028);
    bp(wg_pipe, 0x4b, 0xbba000);
    bp(wg_pipe, 0x52, 0x004c03);
    */

    // enable read & GP link
    cp.write_reg(
        regs::cp::STATUS::zeroed()
            .with_breakpoint(true)
            .with_overflow_hi_watermark(true),
    );

    let flipper_rev = match pi.read(regs::pi::FLIPPER_REV) {
        regs::pi::FLIPPER_REV::A => 'A',
        regs::pi::FLIPPER_REV::B => 'B',
        regs::pi::FLIPPER_REV::C => 'C',
        _ => '?',
    };
    dev_info!(pdev, "Flipper rev: {flipper_rev}\n");

    pe.write_reg(
        regs::pe::CONTROL::zeroed()
            .with_token_enable(true)
            .with_finish_enable(true)
            .with_token(true)
            .with_finish(true),
    );
    cp.write_reg(
        regs::cp::CONTROL::zeroed()
            .with_gple(true)
            .with_fifo_enable(true),
    );

    print_cp_regs(&cp);
    print_pe_regs(&pe);
    print_pi_regs(&pi);

    Ok(0)
}

kernel::of_device_table!(
    OF_TABLE,
    <FlipperPlatformDriver as platform::Driver>::IdInfo,
    [
        (of::DeviceId::new(c"nintendo,hollywood-vi"), ()),
        (of::DeviceId::new(c"nintendo,flipper-vi"), ())
    ]
);

impl platform::Driver for FlipperPlatformDriver {
    type IdInfo = ();
    type Data<'bound> = FlipperPlatformDriverData<'bound>;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        _info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        pin_init::pin_init_scope(move || {
        dev_info!(pdev, "Starting to init flipper!");
        let request = pdev.io_request_by_index(0).ok_or(ENODEV)?;
        let cp_regs = Arc::new(request.iomap_sized::<256>()?, GFP_KERNEL)?;
        let request = pdev.io_request_by_index(1).ok_or(ENODEV)?;
        let pe_regs = Arc::new(request.iomap_sized::<256>()?, GFP_KERNEL)?;
        let request = pdev.io_request_by_index(2).ok_or(ENODEV)?;
        let vi_regs = Arc::new(request.iomap_sized::<256>()?, GFP_KERNEL)?;
        let request = pdev.io_request_by_index(3).ok_or(ENODEV)?;
        let pi_regs = Arc::new(request.iomap_sized::<256>()?, GFP_KERNEL)?;
        let request = pdev.io_request_by_index(4).ok_or(ENODEV)?;
        let wg_pipe = Arc::new(request.iomap_sized::<4>()?, GFP_KERNEL)?;
        let request = pdev.io_request_by_index(5).ok_or(ENODEV)?;
        let efb = Arc::new(request.iomap_sized::<0x00400000>()?, GFP_KERNEL)?;

        /*
        // Allocate 256 KiB for the FIFO, same as most homebrew.
        const _: () = assert!(
            256 * 1024 < uapi::KMALLOC_MAX_SIZE,
            "Failed to allocate 256 KiB FIFO because KMALLOC_MAX_SIZE is too small"
        );
        let layout = Layout::from_size_align(256 * 1024, 4096).map_err(|_| ENOMEM)?;
        let layout = Kmalloc::aligned_layout(layout);
        let fifo = Kmalloc::alloc(layout, GFP_KERNEL, NumaNode::NO_NODE)?;
        let phys = unsafe { bindings::vmalloc_to_phys(fifo.as_ptr() as *mut c_void) };
        dev_info!(pdev, "Allocated FIFO: {} bytes at {fifo:p} (0x{:08x})\n", fifo.len(), phys);
        */

        /*
        // Allocate 640×480 for the XFB, single-buffered.
        const _: () = assert!(
            640 * 480 * 2 < uapi::KMALLOC_MAX_SIZE,
            "Failed to allocate 640×480 XFB because KMALLOC_MAX_SIZE is too small"
        );
        let layout = Layout::from_size_align(640 * 480 * 2, 4096).map_err(|_| ENOMEM)?;
        let layout = Kmalloc::aligned_layout(layout);
        let xfb = Kmalloc::alloc(layout, GFP_KERNEL, NumaNode::NO_NODE)?;
        let phys = unsafe { bindings::vmalloc_to_phys(xfb.as_ptr() as *mut c_void) };
        dev_info!(pdev, "Allocated XFB: {} bytes at {xfb:p} (0x{:08x})\n", xfb.len(), phys);
        */

        /*
        {
            const XFB_START: u32 = 0x01774000;

            let vi = vi_regs.access(pdev.as_ref())?;

            let addr = vi.read32(0x1c);
            dev_info!(pdev, "XFB currently at 0x{addr:08x}\n");
            vi.write32((1 << 28) | (XFB_START >> 5), 0x1c);
            dev_info!(pdev, "Now at 0x{XFB_START:08x}\n");

            let xfb = unsafe { uapi::ioremap(XFB_START, 640 * 448 * 2) } as *mut u16;
            let phys = unsafe { bindings::vmalloc_to_phys(xfb as *mut c_void) };
            dev_info!(pdev, "xfb {xfb:p} (phys 0x{phys:08x})\n");
            for i in 0..(640 * 448) {
                unsafe { xfb.add(i).write_volatile(0x7fff_u16) };
            }
            unsafe { uapi::iounmap(xfb as *mut c_void) };
        }
        */

        setup_fifo(
            pdev, /*, &fifo*/
            &cp_regs, &pe_regs, &pi_regs, &vi_regs, &wg_pipe,
        )?;

        /*
        {
            let efb = efb.access(pdev.as_ref()).unwrap();
            efb.write32(0xffffffff, 0);
            let top_left_pixel = efb.read32(0);
            pr_info!("top-left pixel is 0x{top_left_pixel:08x}");
            /*
            efb.write32(0xff00ff00, 0x00_000_004);
            efb.write32(0xff00ff00, 0x00_000_008);
            efb.write32(0xff00ff00, 0x00_000_00c);
            efb.write32(0x00ffffff, 0x00_00f_000);
            efb.write32(0x00ffffff, 0x00_00f_004);
            efb.write32(0x00ffffff, 0x00_00f_008);
            efb.write32(0x00ffffff, 0x00_00f_00c);
            */
        }
        */

        let mut pos_vec = KVec::from_elem(0u8, 32, GFP_KERNEL)?;
        let mut color_vec = KVec::from_elem(0u8, 32, GFP_KERNEL)?;

        // SAFETY: TODO
        let pos_phys = unsafe { bindings::vmalloc_to_phys(pos_vec.as_mut_ptr().cast()) };
        dev_info!(
            pdev,
            "Got pos_vec at {:p}, and pos_phys at 0x{pos_phys:08x}",
            pos_vec.as_ptr()
        );

        // SAFETY: TODO
        let color_phys = unsafe { bindings::vmalloc_to_phys(color_vec.as_mut_ptr().cast()) };
        dev_info!(
            pdev,
            "Got color_vec at {:p}, and color_phys at 0x{color_phys:08x}",
            color_vec.as_ptr()
        );

        let unreg_dev = drm::UnregisteredDevice::<FlipperDrmDriver>::new(pdev, Ok(()))?;

        Ok(try_pin_init!(FlipperPlatformDriverData {
            // SAFETY: `_reg` is stored in `FlipperPlatformDriverData` and dropped when the
            // driver is unbound; it is never forgotten.
            _reg <- unsafe { drm::Registration::new(
                pdev.as_ref(),
                unreg_dev,
                pin_init!(FlipperDrmRegistrationData {
                    pdev,
                    cp_regs,
                    pe_regs,
                    vi_regs,
                    pi_regs,
                    wg_pipe,
                    //fifo,
                    //xfb,
                    efb,
                    pos_vec <- new_mutex!(pos_vec),
                    color_vec <- new_mutex!(color_vec),
                }),
            )},
            _: { dev_dbg!(pdev, "Flipper initialized correctly.") },
        }))
        })
    }
}

#[pinned_drop]
impl PinnedDrop for FlipperPlatformDriverData<'_> {
    fn drop(self: Pin<&mut Self>) {}
}

const INFO: drm::DriverInfo = drm::DriverInfo {
    major: 0,
    minor: 1,
    patchlevel: 0,
    name: c"flipper",
    desc: c"Nintendo Flipper DRM driver",
};

#[vtable]
impl drm::Driver for FlipperDrmDriver {
    type Data = ();
    type RegistrationData<'drm> = FlipperDrmRegistrationData<'drm>;
    type File = FlipperDrmFileData;
    type Object = Bo;
    type ParentDevice<Ctx: DeviceContext> = platform::Device<Ctx>;

    const INFO: drm::DriverInfo = INFO;
    const FEAT_RENDER: bool = true;

    kernel::declare_drm_ioctls! {
        (FLIPPER_RUN_CMDS, drm_flipper_run_cmds, ioctl::RENDER_ALLOW, FlipperDrmFileData::run_cmds),
        (FLIPPER_WRITE_TEX_MEM, drm_flipper_write_tex_mem, ioctl::RENDER_ALLOW, FlipperDrmFileData::write_tex_mem),
        (FLIPPER_MAP_EFB, drm_flipper_map_efb, ioctl::RENDER_ALLOW, FlipperDrmFileData::map_efb),
    }
}
