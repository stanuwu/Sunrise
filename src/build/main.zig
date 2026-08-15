//! Build logic for Project Sunrise.

pub const Config = @import("Config.zig");
pub const GitVersion = @import("GitVersion.zig");
pub const SharedDeps = @import("SharedDeps.zig");

// Artifacts
pub const SunriseDll = @import("SunriseDll.zig");

// Helpers
pub const requireZig = @import("zig.zig").requireZig;
