#![deny(unsafe_op_in_unsafe_fn)]
#![deny(clippy::undocumented_unsafe_blocks)]
#![deny(clippy::missing_safety_doc)]
#![deny(clippy::ptr_as_ptr)]
#![deny(clippy::cast_ptr_alignment)]

pub mod aligned;
pub mod args;
pub mod engine;
pub mod hash;
pub mod safety;
pub mod stats;
pub mod swab;
pub mod tui_device;
pub mod tui_render;
pub mod tui_state;

#[cfg(target_os = "linux")]
pub mod uring;
