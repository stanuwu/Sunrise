const SharedDeps = @This();

const std = @import("std");
const xwin_sdk = @import("xwin_sdk");

const Config = @import("Config.zig");

config: *const Config,

options: *std.Build.Step.Options,
config_header: *std.Build.Step.ConfigHeader,
xwin: xwin_sdk.Paths,

pub fn init(b: *std.Build, cfg: *const Config) !SharedDeps {
    var result: SharedDeps = .{
        .config = cfg,

        // Setup by retarget
        .options = undefined,
        .config_header = undefined,
        .xwin = undefined,
    };
    try result.initTarget(b, cfg.target);

    return result;
}

/// Retarget our dependencies for another build target. Modifies in-place.
pub fn retarget(
    self: *const SharedDeps,
    b: *std.Build,
    target: std.Build.ResolvedTarget,
) !SharedDeps {
    var result = self.*;
    try result.initTarget(b, target);
    return result;
}

fn initTarget(
    self: *SharedDeps,
    b: *std.Build,
    target: std.Build.ResolvedTarget,
) !void {
    // Set our xwin paths
    self.xwin = try xwin_sdk.pathsForTarget(b, target.result);

    // Change our config
    const config = try b.allocator.create(Config);
    config.* = self.config.*;
    config.target = target;
    self.config = config;

    // Setup our shared build options
    self.options = b.addOptions();
    try self.config.addOptions(self.options);

    // Setup the equivalent C/C++ build options.
    self.config_header = b.addConfigHeader(.{
        .style = .blank,
        .include_path = "sunrise_build_config.h",
    }, .{
        .SUNRISE_VER_MAJOR = @as(i64, @intCast(self.config.version.major)),
        .SUNRISE_VER_MINOR = @as(i64, @intCast(self.config.version.minor)),
        .SUNRISE_VER_PATCH = @as(i64, @intCast(self.config.version.patch)),
        .SUNRISE_VER_BUILD = 0,
        .SUNRISE_VER_STRING = b.fmt("{f}", .{self.config.version}),
    });
}

pub fn add(self: *const SharedDeps, step: *std.Build.Step.Compile) !void {
    const b = step.step.owner;

    // Every Zig artifact gets build options populated. The current DLL has
    // only C++ sources, so attaching an import would create an otherwise empty
    // Zig root object that conflicts with MSVC's static CRT TLS definitions.
    if (step.root_module.root_source_file != null) {
        step.root_module.addOptions("build_options", self.options);
    }
    step.root_module.addConfigHeader(self.config_header);

    // MSVC C and C++ files select their runtime with -fms-runtime-lib.
    // Setting link_libc here would additionally force Zig's release CRT in
    // debug builds, mixing msvcrt.lib with the debug runtime directives.
    try xwin_sdk.addPaths(b, step);

    // Zig's bundled libc++ conflicts with the MSVC C++ runtime headers. The
    // xwin CRT include paths and -fms-runtime-lib flags provide that runtime.
    std.debug.assert(step.rootModuleTarget().abi == .msvc);
    std.debug.assert(step.rootModuleTarget().cpu.arch == self.config.target.result.cpu.arch);
}
