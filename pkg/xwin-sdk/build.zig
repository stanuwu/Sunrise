const std = @import("std");

pub fn build(_: *std.Build) !void {}

// The cache uses the build allocator and intentionally lives for the duration
// of the build process.
pub const Cache = struct {
    const Key = struct {
        arch: std.Target.Cpu.Arch,
        os: std.Target.Os.Tag,
        abi: std.Target.Abi,
    };

    var map: std.AutoHashMapUnmanaged(Key, ?Paths) = .empty;
};

/// Paths in an xwin splat directory for a Windows MSVC target.
pub const Paths = struct {
    root: []const u8,
    arch: []const u8,
    libc: std.Build.LazyPath,

    pub fn path(paths: Paths, b: *std.Build, sub_path: []const u8) std.Build.LazyPath {
        return .{ .cwd_relative = paths.joinedPath(b, sub_path) };
    }

    pub fn joinedPath(paths: Paths, b: *std.Build, sub_path: []const u8) []const u8 {
        return b.pathJoin(&.{ paths.root, sub_path });
    }
};

/// Locate an xwin splat directory and prepare its paths for the target.
///
/// The directory can be selected with `-Dxwin-dir`, `XWIN_DIR`, or the
/// conventional `.xwin-cache` and `.xwin` directories in the build root.
pub fn pathsForTarget(b: *std.Build, target: std.Target) !Paths {
    if (target.os.tag != .windows or target.abi != .msvc) {
        return error.XwinUnsupportedTarget;
    }

    const arch = xwinArch(target.cpu.arch) orelse
        return error.XwinUnsupportedArchitecture;
    const gop = try Cache.map.getOrPut(b.allocator, .{
        .arch = target.cpu.arch,
        .os = target.os.tag,
        .abi = target.abi,
    });

    if (!gop.found_existing) {
        const root = findXwinRoot(b, arch) orelse {
            gop.value_ptr.* = null;
            return error.XwinSDKNotFound;
        };
        const files = b.addWriteFiles();
        const crt_lib = b.pathJoin(&.{ root, "crt", "lib", arch });
        const sdk_um_lib = b.pathJoin(&.{ root, "sdk", "lib", "um", arch });

        gop.value_ptr.* = .{
            .root = root,
            .arch = arch,
            .libc = files.add("xwin-libc.txt", b.fmt(
                \\include_dir={s}
                \\sys_include_dir={s}
                \\crt_dir={s}
                \\msvc_lib_dir={s}
                \\kernel32_lib_dir={s}
                \\gcc_dir=
                \\
            , .{
                b.pathJoin(&.{ root, "sdk", "include", "ucrt" }),
                b.pathJoin(&.{ root, "crt", "include" }),
                crt_lib,
                crt_lib,
                sdk_um_lib,
            })),
        };
    }

    return gop.value_ptr.* orelse error.XwinSDKNotFound;
}

/// Configure a compile step to use the headers and libraries from xwin.
pub fn addPaths(b: *std.Build, step: *std.Build.Step.Compile) !void {
    const paths = try pathsForTarget(b, step.rootModuleTarget());

    step.setLibCFile(paths.libc);

    for ([_][]const u8{
        "crt/include",
        "sdk/include/ucrt",
        "sdk/include/um",
        "sdk/include/shared",
        "sdk/include/winrt",
    }) |include| {
        step.root_module.addSystemIncludePath(paths.path(b, include));
    }

    for ([_][]const u8{
        "crt/lib",
        "sdk/lib/ucrt",
        "sdk/lib/um",
    }) |library| {
        step.root_module.addLibraryPath(paths.path(
            b,
            b.pathJoin(&.{ library, paths.arch }),
        ));
    }
}

fn findXwinRoot(b: *std.Build, arch: []const u8) ?[]const u8 {
    if (b.option([]const u8, "xwin-dir", "Path to an xwin splat directory")) |root| {
        if (isXwinRoot(b, root, arch)) return root;
        return null;
    }

    if (b.graph.environ_map.get("XWIN_DIR")) |root| {
        if (isXwinRoot(b, root, arch)) return root;
        return null;
    }

    for ([_][]const u8{ ".xwin-cache", ".xwin" }) |candidate| {
        const root = b.pathFromRoot(candidate);
        if (isXwinRoot(b, root, arch)) return root;
    }

    return null;
}

fn isXwinRoot(b: *std.Build, root: []const u8, arch: []const u8) bool {
    if (root.len == 0) return false;

    const library_dir = b.pathJoin(&.{ root, "sdk", "lib", "um", arch });
    var dir = if (std.fs.path.isAbsolute(library_dir))
        std.Io.Dir.openDirAbsolute(b.graph.io, library_dir, .{}) catch return false
    else
        b.build_root.handle.openDir(b.graph.io, library_dir, .{}) catch return false;
    defer dir.close(b.graph.io);
    return true;
}

fn xwinArch(arch: std.Target.Cpu.Arch) ?[]const u8 {
    return switch (arch) {
        .x86 => "x86",
        .x86_64 => "x86_64",
        .arm => "aarch",
        .aarch64 => "aarch64",
        else => null,
    };
}
