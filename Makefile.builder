ifneq ($(DIST),centos7)
RPM_SPEC_FILES := rpm_spec/gui-daemon.spec
endif
DEBIAN_BUILD_DIRS := debian
