const std = @import("std");
const buildpkg = @import("src/build/main.zig");

/// Sunrise DLL version from build.zig.zon.
const dll_zon_version = @import("build.zig.zon").version;

/// Minimum required zig version.
const minimum_zig_version = @import("build.zig.zon").minimum_zig_version;

comptime {
    buildpkg.requireZig(minimum_zig_version);
}

pub fn build(b: *std.Build) !void {
    // This defines all the available build options (e.g. `-D`). If you
    // want to know what options are available, you can run `--help` or
    // you can read `src/build/Config.zig`.

    // If we have a VERSION file (present in source tarballs) then we
    // use that as the version source of truth. Otherwise we fall back
    // to what is in the build.zig.zon.
    const file_version: ?[]const u8 = if (b.build_root.handle.readFileAlloc(
        b.graph.io,
        "VERSION",
        b.allocator,
        .limited(128),
    )) |content| std.mem.trim(
        u8,
        content,
        &std.ascii.whitespace,
    ) else |_| null;

    const config = try buildpkg.Config.init(
        b,
        file_version orelse dll_zon_version,
    );

    // Sunrise dependencies used by many artifacts.
    const deps = try buildpkg.SharedDeps.init(b, &config);

    // Sunrise DLL, the actual library loaded by the game.
    const dll = try buildpkg.SunriseDll.init(b, &config, &deps);
    if (config.emit_dll) dll.install();
}
