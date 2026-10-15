// SPDX-License-Identifier: GPL-2.0 or MIT

use kernel::{
    bindings,
    drm::{
        self,
        Registered, //
    },
    io::{
        Io,
        IoBase, //
    },
    prelude::*,
    uaccess::UserSlice,
    uapi,
};

use crate::{
    driver::{
        FlipperDrmDevice,
        FlipperDrmDriver,
        FlipperDrmRegistrationData, //
    },
    gx::GxClient,
};

/// `PE_COPY_EXECUTE` bit selecting a copy to the XFB.
const PE_COPY_TO_XFB: u32 = 1 << 14;
/// BP 0x45 (`PE_DONE`) with the trigger bits set.
const PE_DONE_BP: u32 = (0x45 << 24) | 2;

#[pin_data]
pub(crate) struct FlipperDrmFileData {}

/// Convenience type alias for our DRM `File` type
pub(crate) type FlipperDrmFile = drm::file::File<FlipperDrmFileData>;

impl drm::file::DriverFile for FlipperDrmFileData {
    type Driver = FlipperDrmDriver;

    fn open(_dev: &drm::Device<Self::Driver>) -> Result<Pin<KBox<Self>>> {
        KBox::try_pin_init(try_pin_init!(Self {}), GFP_KERNEL)
    }
}

impl FlipperDrmFileData {
    pub(crate) fn run_cmds(
        _ddev: &FlipperDrmDevice<Registered>,
        reg_data: &FlipperDrmRegistrationData<'_>,
        cmd_list: &mut uapi::drm_flipper_run_cmds,
        _file: &FlipperDrmFile,
    ) -> Result<u32> {
        // GX is shared with the framebuffer conversion, which runs at every retrace and leaves
        // its own state behind, so nothing may be assumed to survive past this submission.
        let _gx = reg_data.gx.lock(GxClient::User);

        // Every submission is synchronous: PE_DONE is appended and waited for below.
        reg_data.gcn.fence_arm()?;

        let cmds_len = cmd_list.cmds_len as usize;
        //pr_info!("Got {cmds_len} commands: {cmd_list:?}\n");
        pr_debug!("Got {cmds_len} commands\n");


        // Warning: huge hack!
        //const FIFO_END: u32 = 0x01774000;
        //const POS_ADDR: u32 = 0x01774000;
        //const COLOR_ADDR: u32 = 0x01774800;

        let wg_pipe = reg_data.wg_pipe.as_view().big_endian();

        for i in 0..cmds_len {
            let cmd = cmd_list.cmds[i];
            let pointer = cmd.pointer as usize;
            let size = cmd.size as usize;
            let mut reader = UserSlice::new(UserPtr::from_addr(pointer), size).reader();
            pr_debug!(
                "Command {}, run {size} bytes from 0x{pointer:08x}\n",
                cmd.command
            );

            match cmd.command {
                uapi::drm_flipper_command_FIFO => {
                    while let Ok(command) = reader.read() {
                        match command {
                            // NOP
                            0x00 => {
                                //pr_info!("FIFO: NOP\n");
                                wg_pipe.write8(command, 0x0);
                            }
                            // CP
                            0x08 => {
                                let reg: u8 = reader.read()?;
                                let mut value: u32 = reader.read()?;
                                // Warning: huge hack!
                                if reg == 0xa0 {
                                    let old_value = value;
                                    let mut pos_vec = reg_data.pos_vec.lock();
                                    //value = POS_ADDR;
                                    // SAFETY: TODO
                                    value = unsafe {
                                        bindings::vmalloc_to_phys(
                                            pos_vec.as_mut_ptr().cast::<c_void>(),
                                        )
                                    } as u32;
                                    pr_debug!("Patching CP 0x{reg:02x} from 0x{old_value:08x} to 0x{value:08x}\n");
                                } else if reg == 0xa2 {
                                    let old_value = value;
                                    let mut color_vec = reg_data.color_vec.lock();
                                    //value = COLOR_ADDR;
                                    // SAFETY: TODO
                                    value = unsafe {
                                        bindings::vmalloc_to_phys(
                                            color_vec.as_mut_ptr().cast::<c_void>(),
                                        )
                                    } as u32;
                                    pr_debug!("Patching CP 0x{reg:02x} from 0x{old_value:08x} to 0x{value:08x}\n");
                                }
                                //pr_info!("FIFO: Set CP 0x{reg:02x} to 0x{value:08x}\n");
                                wg_pipe.write8(command, 0x0);
                                wg_pipe.write8(reg, 0x0);
                                wg_pipe.write32(value, 0x0);
                            }
                            // XF
                            0x10 => {
                                let num: u16 = reader.read()?;
                                let reg: u16 = reader.read()?;
                                pr_debug!("FIFO: Set {} XF regs, from {reg:04x}\n", num + 1);
                                wg_pipe.write8(command, 0x0);
                                wg_pipe.write16(num, 0x0);
                                wg_pipe.write16(reg, 0x0);
                                for _ in 0..num + 1 {
                                    let value = reader.read()?;
                                    wg_pipe.write32(value, 0x0);
                                    //if num < 16 {
                                    //pr_info!("0x{value:08x}\n");
                                    //}
                                }
                            }
                            // Invalidate vertex cache
                            0x48 => {
                                pr_debug!("FIFO: Invalidate vertex cache\n");
                                wg_pipe.write8(command, 0x0);
                            }
                            // BP
                            0x61 => {
                                let data: u32 = reader.read()?;
                                let reg = data >> 24;
                                let value = data & 0x00_ffffff;
                                // An EFB-to-XFB copy: from now on userspace owns the picture.
                                if reg == 0x52 && value & PE_COPY_TO_XFB != 0 {
                                    reg_data.gcn.mark_user_output();
                                }
                                //pr_info!("FIFO: Set BP 0x{reg:02x} to 0x{value:06x}\n");
                                wg_pipe.write8(command, 0x0);
                                wg_pipe.write32((reg << 24) | value, 0x0);
                            }
                            // Draw triangle
                            0x90 => {
                                let len: u16 = reader.read()?;
                                wg_pipe.write8(command, 0x0);
                                wg_pipe.write16(len, 0x0);
                                pr_debug!("FIFO: Draw triangles ({len} vertices)\n");
                                for _ in 0..len {
                                    let a = reader.read()?;
                                    let b = reader.read()?;
                                    pr_debug!("{a:02x} {b:02x}\n");
                                    wg_pipe.write8(a, 0x0);
                                    wg_pipe.write8(b, 0x0);
                                }
                            }
                            _ => {
                                pr_err!("FIFO: Warning: Unknown command {command:02x}!\n");
                                wg_pipe.write8(command, 0x0);
                                //return Err(EINVAL);
                            }
                        }
                    }
                    // Flush the write-gather pipe (by padding it out to a full burst, so it gets issued to
                    // the PI FIFO), since the user probably wants to run stuff asap.
                    for _ in 0..31 {
                        wg_pipe.write8(0x00, 0x0);
                    }
                }
                /*
                uapi::drm_flipper_command_UPDATE_VERTEX => {
                    if i == 1 {
                        //let mut pos_vec = KVec::with_capacity(32, GFP_KERNEL)?;
                        reader.read_all(&mut *tdev.pos_vec.borrow_mut(), GFP_KERNEL)?;

                        //let pos_ptr = unsafe { bindings::ioremap(POS_ADDR, 0x800) };
                        //unsafe { core::ptr::copy_nonoverlapping(pos_vec.as_ptr(), pos_ptr.cast(), 32) };
                        //unsafe { bindings::iounmap(pos_ptr) };
                    } else if i == 2 {
                        //let mut color_vec = KVec::with_capacity(32, GFP_KERNEL)?;
                        reader.read_all(&mut *tdev.color_vec.borrow_mut(), GFP_KERNEL)?;

                        //let color_ptr = unsafe { bindings::ioremap(COLOR_ADDR, 0x800) };
                        //unsafe { core::ptr::copy_nonoverlapping(color_vec.as_ptr(), color_ptr.cast(), 32) };
                        //unsafe { bindings::iounmap(color_ptr) };
                    } else {
                        return Err(EFAULT);
                    }
                }
                */
                _ => return Err(EINVAL),
            }
        }

        // Signal PE finish once everything before is done, push it out of the write-gather pipe
        // and wait for it.
        wg_pipe.write8(0x61, 0x0);
        wg_pipe.write32(PE_DONE_BP, 0x0);
        for _ in 0..31 {
            wg_pipe.write8(0x00, 0x0);
        }
        reg_data.gcn.fence_wait()?;

        /*
        {
            let cp = tdev.cp_regs.try_access().ok_or(EBUSY)?;
            crate::driver::print_cp_regs(cp.big_endian());
        }
        {
            let pe = tdev.pe_regs.try_access().ok_or(EBUSY)?;
            crate::driver::print_pe_regs(pe.big_endian());
        }
        {
            let pi = tdev.pi_regs.try_access().ok_or(EBUSY)?;
            crate::driver::print_pi_regs(pi.big_endian());
        }
        {
            let vi = tdev.vi_regs.try_access().ok_or(EBUSY)?;
            crate::driver::print_vi_regs(vi.big_endian());
        }
        */

        Ok(0)
    }

    pub(crate) fn write_tex_mem(
        _ddev: &FlipperDrmDevice<Registered>,
        reg_data: &FlipperDrmRegistrationData<'_>,
        cmd: &mut uapi::drm_flipper_write_tex_mem,
        _file: &FlipperDrmFile,
    ) -> Result<u32> {
        let pointer = cmd.pointer as usize;
        let size = cmd.size as usize;
        //let reader = UserSlice::new(UserPtr::from_addr(pointer), size).reader();

        pr_info!(
            "Not writing {size} bytes of texture memory from 0x{pointer:08x}, unimplemented!\n"
        );

        Ok(0)
    }

    pub(crate) fn map_efb(
        _ddev: &FlipperDrmDevice<Registered>,
        reg_data: &FlipperDrmRegistrationData<'_>,
        cmd: &mut uapi::drm_flipper_map_efb,
        _file: &FlipperDrmFile,
    ) -> Result<u32> {
        let pointer = cmd.pointer as usize;

        //const EFB_ADDRESS: usize = 0x08_000_000;
        //const EFB_SIZE: usize = 0x00_400_000;
        //let efb_raw = MmioRaw::<EFB_SIZE>::new(EFB_ADDRESS, EFB_SIZE)?;
        //let efb = unsafe { Mmio::from_raw(&efb_raw) };

        let efb = reg_data.efb.as_view().big_endian();
        //let phys = unsafe { bindings::vmalloc_to_phys(efb.addr() as *mut c_void) } as u32;
        //pr_info!("EFB is mapped at 0x{:08x}, phys 0x{phys:08x}\n", efb.addr());

        efb.write32(0xffffffff, 0);
        let top_left_pixel = efb.read32(0);
        pr_info!("top-left pixel is 0x{top_left_pixel:08x}");
        //writer.write(&top_left_pixel)?;

        /*
        let black = efb.read32(0);
        writer.write(&black)?;
        let middle = efb.read32((264 << 12) | (320 << 2));
        writer.write(&middle)?;
        */

        /*
        for y in 0..128/*528*/ {
            for x in 0..640 {
                let addr = (y << 12) | (x << 2);
                //pr_info!("Read at addr 0x{addr:06x}\n");
                let pixel = core::hint::black_box(efb.read32(addr));
                //pr_info!("Got pixel 0x{pixel:08x}\n");
                //writer.write(&pixel)?;
                //writer.write_slice(&pixel.to_be_bytes())?;
                //pr_info!("Wrote pixel!\n");
            }
        }
        */

        Ok(0)
    }
}
