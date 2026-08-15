/// Build configuration populated during `zig build`.
const Config = @This();

const std = @import("std");
const GitVersion = @import("GitVersion.zig");

/// Standard build configuration options.
optimize: std.builtin.OptimizeMode,
target: std.Build.ResolvedTarget,
strip: bool = false,

/// Project Sunrise DLL properties.
version: std.SemanticVersion = .{ .major = 0, .minor = 0, .patch = 0 },

/// Artifacts.
emit_dll: bool = false,

/// True when Project Sunrise is built as a dependency of another project.
is_dep: bool = false,

/// Environmental properties.
env: *const std.process.Environ.Map,

pub fn init(b: *std.Build, dllVersion: []const u8) !Config {
    const optimize = b.standardOptimizeOption(.{});
    const target = b.standardTargetOptions(.{
        .default_target = .{
            .cpu_arch = .x86_64,
            .os_tag = .windows,
            .abi = .msvc,
        },
    });
    if (target.result.cpu.arch != .x86_64 or
        target.result.os.tag != .windows or
        target.result.abi != .msvc)
    {
        std.log.err(
            "{s} is not a supported target for this project. " ++
                "Only x86_64-windows-msvc is supported.",
            .{target.result.zigTriple(b.allocator) catch "requested target"},
        );
        return error.UnsupportedTarget;
    }

    const is_dep = b.dep_prefix.len > 0;
    var config: Config = .{
        .optimize = optimize,
        .target = target,
        .is_dep = is_dep,
        .env = &b.graph.environ_map,
    };

    //---------------------------------------------------------------
    // Project Sunrise DLL properties

    const version_string = b.option(
        []const u8,
        "version-string",
        "A specific semantic version to use for the build. " ++
            "If not specified, git will be used.",
    );

    config.version = if (version_string) |version|
        try std.SemanticVersion.parse(version)
    else version: {
        const dll_version = try std.SemanticVersion.parse(dllVersion);

        // Dependency builds may not be inside the Sunrise Git checkout.
        if (is_dep) break :version dll_version;

        const git_version = GitVersion.detect(b) catch |err| switch (err) {
            error.GitNotFound,
            error.GitNotRepository,
            => break :version .{
                .major = dll_version.major,
                .minor = dll_version.minor,
                .patch = dll_version.patch,
                .pre = "dev",
                .build = "0000000",
            },

            else => return err,
        };
        if (git_version.tag) |tag| {
            if (!std.mem.eql(u8, tag, "tip")) {
                const expected = b.fmt("v{d}.{d}.{d}", .{
                    dll_version.major,
                    dll_version.minor,
                    dll_version.patch,
                });
                if (!std.mem.eql(u8, tag, expected)) {
                    @panic("tagged releases must be in vX.Y.Z format matching build.zig.zon");
                }

                break :version dll_version;
            }
        }

        break :version .{
            .major = dll_version.major,
            .minor = dll_version.minor,
            .patch = dll_version.patch,
            .pre = git_version.branch,
            .build = git_version.short_hash,
        };
    };

    //---------------------------------------------------------------
    // Binary properties

    config.strip = b.option(
        bool,
        "strip",
        "Strip the final DLL. Defaults to true for debug, fast, and small builds.",
    ) orelse switch (optimize) {
        // xwin omits the MSVC library PDBs unless splatted with
        // --include-debug-symbols; LLD otherwise emits fatal LNK4099 warnings.
        .Debug => true,
        .ReleaseSafe => false,
        .ReleaseFast, .ReleaseSmall => true,
    };

    //---------------------------------------------------------------
    // Artifacts to emit

    config.emit_dll = b.option(
        bool,
        "emit-dll",
        "Build and install the Sunrise DLL with 'build'.",
    ) orelse true;

    return config;
}

/// Configure the build options with our values.
pub fn addOptions(self: *const Config, step: *std.Build.Step.Options) !void {
    // Our version. We also add the string version so we don't need
    // to do any allocations at runtime. This has to be long enough to
    // accommodate realistic large branch names for dev versions.
    var dll_version_buf: [1024]u8 = undefined;
    step.addOption(std.SemanticVersion, "dll_version", self.version);
    step.addOption([:0]const u8, "dll_version_string", try std.fmt.bufPrintZ(
        &dll_version_buf,
        "{f}",
        .{self.version},
    ));
    step.addOption(
        ReleaseChannel,
        "release_channel",
        channel: {
            const pre = self.version.pre orelse break :channel .stable;
            if (pre.len == 0) break :channel .stable;
            break :channel .tip;
        },
    );
}

/// Rehydrate our Config from the comptime options. Note that not all
/// options are available at comptime, so look closely at this implementation
/// to see what is and isn't available.
pub fn fromOptions() Config {
    const options = @import("build_options");
    return .{
        // Unused at runtime.
        .optimize = undefined,
        .target = undefined,
        .env = undefined,

        .version = options.dll_version,
    };
}

/// Whether release artifacts should omit frame pointers.
pub fn omitFramePointer(self: *const Config) bool {
    return self.strip;
}

/// The release channel for the build.
pub const ReleaseChannel = enum {
    /// Unstable builds on every commit.
    tip,

    /// Stable tagged releases.
    stable,
};
