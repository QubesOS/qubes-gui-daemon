const std = @import("std");

const csources = [_][]const u8{
    "xside.c",
    "xutils.c",
    "png.c",
    "trayicon.c",
    "xinput-plugin.c",
    "../gui-common/double-buffer.c",
    "../gui-common/txrx-vchan.c",
    "../gui-common/error.c",
    "../common/list.c",
};

// TODO:
// VCHAN_PKG = $(if $(BACKEND_VMM),vchan-$(BACKEND_VMM),vchan)
const VCHAN_PKG = "vchan";
const packages = .{
    "x11",
    "xext",
    "x11-xcb",
    "xcb",
    "xi",
    VCHAN_PKG,
    "libpng",
    "libnotify", // TODO: replace with `notify-send`
    "libconfig", // TODO: replace with simple toml parser
    "libunwind",
};

fn setup_dependencies(exe: *std.build.LibExeObjStep) void {
    exe.addIncludePath("../qubes-common/include");
    exe.addIncludePath("../include");
    // exe.addIncludePath("include");
    // exe.addLibraryPath("/usr/lib");
    // exe.addSystemIncludePath("/usr/include");
    // exe.addIncludePath("/usr/include");
    exe.linkLibC();
    inline for (packages) |package| {
        exe.linkSystemLibrary(package);
    }
}

pub fn build(b: *std.build.Builder) void {
    const target = b.standardTargetOptions(.{}); // -Dtarget
    const mode = b.standardReleaseOptions(); // -Drelease-fast -Drelease-safe -Drelease-small

    const exe = b.addExecutable("qubes-guid", "xside.c");
    exe.setTarget(target);
    exe.setBuildMode(mode);
    setup_dependencies(exe);
    exe.addCSourceFiles(csources[1..], &.{
        
    });
    
    // const obj_xinput_plug = b.addObject("qubes-daemon-xinput-plugin", "xinput-plugin.zig");
    // setup_dependencies(obj_xinput_plug);
    // obj_xinput_plug.addIncludePath(".");
    // exe.addObject(obj_xinput_plug);

    exe.install();

    const run_cmd = exe.run();
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);

    // const exe_tests = b.addTest("src/main.zig");
    // exe_tests.setTarget(target);
    // exe_tests.setBuildMode(mode);

    // const test_step = b.step("test", "Run unit tests");
    // test_step.dependOn(&exe_tests.step);
}
