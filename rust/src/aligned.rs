use std::alloc::{alloc_zeroed, dealloc, handle_alloc_error, Layout};
use std::ops::{Deref, DerefMut};

/// A memory buffer guaranteed to be aligned to a given power-of-two boundary
/// (defaulting to 4096-byte memory page boundaries for Linux O_DIRECT, macOS F_NOCACHE,
/// and Windows FILE_FLAG_NO_BUFFERING).
pub struct AlignedBuffer {
    ptr: *mut u8,
    layout: Layout,
    capacity: usize,
}

impl AlignedBuffer {
    /// Allocates an aligned zeroed memory buffer.
    pub fn new(capacity: usize, alignment: usize) -> Self {
        let align = alignment.max(4096);
        let alloc_size = capacity.next_multiple_of(align).max(align);
        let layout = Layout::from_size_align(alloc_size, align)
            .expect("Invalid alignment layout specification");

        let ptr = unsafe { alloc_zeroed(layout) };
        if ptr.is_null() {
            handle_alloc_error(layout);
        }

        Self {
            ptr,
            layout,
            capacity,
        }
    }

    #[allow(dead_code)]
    #[inline]
    pub fn as_ptr(&self) -> *const u8 {
        self.ptr
    }

    #[allow(dead_code)]
    #[inline]
    pub fn as_mut_ptr(&mut self) -> *mut u8 {
        self.ptr
    }
}

impl Deref for AlignedBuffer {
    type Target = [u8];

    #[inline]
    fn deref(&self) -> &Self::Target {
        unsafe { std::slice::from_raw_parts(self.ptr, self.capacity) }
    }
}

impl DerefMut for AlignedBuffer {
    #[inline]
    fn deref_mut(&mut self) -> &mut Self::Target {
        unsafe { std::slice::from_raw_parts_mut(self.ptr, self.capacity) }
    }
}

impl Drop for AlignedBuffer {
    fn drop(&mut self) {
        unsafe {
            dealloc(self.ptr, self.layout);
        }
    }
}

unsafe impl Send for AlignedBuffer {}
unsafe impl Sync for AlignedBuffer {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_aligned_buffer_4096_boundary() {
        let buf = AlignedBuffer::new(65536, 4096);
        let addr = buf.as_ptr() as usize;
        assert_eq!(
            addr % 4096,
            0,
            "Buffer address must be 4096-byte page aligned"
        );
        assert_eq!(buf.len(), 65536);
    }
}
