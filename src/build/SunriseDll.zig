const SunriseDll = @This();

const std = @import("std");
const Config = @import("Config.zig");
const SharedDeps = @import("SharedDeps.zig");

const project_flags: []const []const u8 = &.{
    "-std=c++20",
    "-fms-extensions",
    "-fms-runtime-lib=static",
    "-Wno-braced-scalar-init",
};
const project_debug_flags: []const []const u8 = &.{
    "-std=c++20",
    "-fms-extensions",
    "-fms-runtime-lib=static_dbg",
    "-Wno-braced-scalar-init",
};
const vendor_flags: []const []const u8 = &.{
    "-std=c++20",
    "-fms-extensions",
    "-fms-runtime-lib=static",
    "-w",
};
const vendor_debug_flags: []const []const u8 = &.{
    "-std=c++20",
    "-fms-extensions",
    "-fms-runtime-lib=static_dbg",
    "-w",
};

/// The primary Sunrise dynamic library.
dll: *std.Build.Step.Compile,

/// The install step for the dynamic library and its import library.
install_step: *std.Build.Step.InstallArtifact,

pub fn init(
    b: *std.Build,
    cfg: *const Config,
    deps: *const SharedDeps,
) !SunriseDll {
    const dll = b.addLibrary(.{
        .name = "steam_api64",
        .linkage = .dynamic,
        .root_module = b.createModule(.{
            .target = cfg.target,
            .optimize = cfg.optimize,
            .strip = cfg.strip,
            .omit_frame_pointer = cfg.omitFramePointer(),
            .unwind_tables = if (cfg.strip) .none else .sync,
            .sanitize_c = .off,
        }),
        .use_llvm = true,
    });
    const install_step = b.addInstallArtifact(dll, .{});

    try deps.add(dll);
    addIncludePaths(b, dll.root_module);
    addDefinitions(dll.root_module, cfg.optimize);
    try addSources(b, dll.root_module, cfg.optimize);
    addSystemLibraries(dll.root_module, cfg.optimize);

    dll.rc_includes = .none;
    dll.root_module.addWin32ResourceFile(.{
        .file = b.path("Sunrise/resources/sunrise.rc"),
        .flags = &.{ "/d", "SUNRISE_ZIG_BUILD=1" },
        .include_paths = &.{
            b.path("Sunrise"),
            deps.config_header.getOutputDir(),
            deps.xwin.path(b, "crt/include"),
            deps.xwin.path(b, "sdk/include/ucrt"),
            deps.xwin.path(b, "sdk/include/um"),
            deps.xwin.path(b, "sdk/include/shared"),
        },
    });

    return .{
        .dll = dll,
        .install_step = install_step,
    };
}

/// Add the Sunrise DLL to the install target.
pub fn install(self: *const SunriseDll) void {
    const b = self.install_step.step.owner;
    b.getInstallStep().dependOn(&self.install_step.step);
}

fn addSources(
    b: *std.Build,
    module: *std.Build.Module,
    optimize: std.builtin.OptimizeMode,
) !void {
    const project_sources = try collectSources(b, "Sunrise/src", ".cpp");
    module.addCSourceFiles(.{
        .files = project_sources,
        .flags = if (optimize == .Debug) project_debug_flags else project_flags,
        .language = .cpp,
    });

    module.addCSourceFiles(.{
        .files = &.{
            "Sunrise/vendor/detours/detours.cpp",
            "Sunrise/vendor/detours/disasm.cpp",
            "Sunrise/vendor/detours/modules.cpp",
            "Sunrise/vendor/imgui/imgui.cpp",
            "Sunrise/vendor/imgui/imgui_draw.cpp",
            "Sunrise/vendor/imgui/imgui_tables.cpp",
            "Sunrise/vendor/imgui/imgui_widgets.cpp",
            "Sunrise/vendor/imgui/backends/imgui_impl_win32.cpp",
            "Sunrise/vendor/imgui/backends/imgui_impl_dx11.cpp",
        },
        .flags = if (optimize == .Debug) vendor_debug_flags else vendor_flags,
        .language = .cpp,
    });
}

fn addIncludePaths(b: *std.Build, module: *std.Build.Module) void {
    module.addIncludePath(b.path("Sunrise/src"));
    module.addSystemIncludePath(b.path("Sunrise/vendor"));
    module.addSystemIncludePath(b.path("Sunrise/vendor/detours"));
    module.addSystemIncludePath(b.path("Sunrise/vendor/imgui"));
    module.addSystemIncludePath(b.path("Sunrise/vendor/imgui/backends"));
}

fn addDefinitions(module: *std.Build.Module, optimize: std.builtin.OptimizeMode) void {
    const definitions = [_][]const u8{
        "WIN32",
        "_WINDOWS",
        "_USRDLL",
        "SUNRISE_ZIG_BUILD",
        "UNICODE",
        "_UNICODE",
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX",
    };
    for (definitions) |name| module.addCMacro(name, "1");
    module.addCMacro(if (optimize == .Debug) "_DEBUG" else "NDEBUG", "1");
    module.addCMacro("IMGUI_USER_CONFIG", "\"core/ui/imgui_user_config.h\"");
}

fn addSystemLibraries(
    module: *std.Build.Module,
    optimize: std.builtin.OptimizeMode,
) void {
    const runtime_libraries: []const []const u8 = if (optimize == .Debug)
        &.{ "libcpmtd", "libcmtd", "vcruntimed", "ucrtd" }
    else
        &.{ "libcpmt", "libcmt", "vcruntime", "ucrt" };
    for (runtime_libraries) |library| module.linkSystemLibrary(library, .{});

    for ([_][]const u8{
        "oldnames",
        "kernel32",
        "user32",
        "gdi32",
        "shell32",
        "dwmapi",
        "bcrypt",
        "ws2_32",
        "d3dcompiler",
        "synchronization",
    }) |library| {
        module.linkSystemLibrary(library, .{});
    }
}

fn collectSources(
    b: *std.Build,
    root: []const u8,
    extension: []const u8,
) ![]const []const u8 {
    const io = b.graph.io;
    var directory = try b.build_root.handle.openDir(io, root, .{ .iterate = true });
    defer directory.close(io);

    var walker = try directory.walk(b.allocator);
    defer walker.deinit();

    var files = std.array_list.Managed([]const u8).init(b.allocator);
    while (try walker.next(io)) |entry| {
        if (entry.kind != .file or !std.mem.endsWith(u8, entry.path, extension)) continue;
        try files.append(b.pathJoin(&.{ root, entry.path }));
    }
    std.mem.sort([]const u8, files.items, {}, struct {
        fn lessThan(_: void, left: []const u8, right: []const u8) bool {
            return std.mem.lessThan(u8, left, right);
        }
    }.lessThan);
    return files.toOwnedSlice();
}
