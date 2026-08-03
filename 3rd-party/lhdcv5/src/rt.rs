// usbpods embedded runtime glue: global allocator + panic handler for the
// bare-metal (target_os = "none") build. Both delegate to the C side so the
// Rust heap IS the firmware heap (newlib malloc, PICO_MALLOC_PANIC=0 -> a
// failed alloc returns null and alloc_error trips, which we also route to C).
//
// Alignment: newlib malloc returns 8-byte-aligned blocks. Every allocation in
// this crate is a Vec/Box of i32/u32/u8/Complex{i32,i32} (max align 4), so
// align > 8 cannot occur; it is still checked and refused rather than assumed.

#[cfg(target_os = "none")]
mod embedded {
    use core::alloc::{GlobalAlloc, Layout};

    extern "C" {
        fn malloc(size: usize) -> *mut u8;
        fn free(ptr: *mut u8);
        /// C side (codec_lhdc.c): logs + reboots via the usbpods panic path.
        fn lhdcv5_rust_panic() -> !;
    }

    struct NewlibAlloc;

    unsafe impl GlobalAlloc for NewlibAlloc {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            if layout.align() > 8 {
                return core::ptr::null_mut();
            }
            malloc(layout.size())
        }
        unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
            free(ptr)
        }
    }

    #[global_allocator]
    static ALLOC: NewlibAlloc = NewlibAlloc;

    #[panic_handler]
    fn panic(_info: &core::panic::PanicInfo) -> ! {
        unsafe { lhdcv5_rust_panic() }
    }
}
