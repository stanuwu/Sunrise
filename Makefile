init:
	@echo You probably want to run "zig build" instead.
.PHONY: init

clean:
	rm -rf \
		zig-out .zig-cache \
		build
.PHONY: clean
