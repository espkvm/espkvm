# SPDX-License-Identifier: Apache-2.0

.PHONY: test-host test-console test-usb-video test-usb-video-transport test-usb-video-signing

test-host:
	sh tools/test.sh host

test-console:
	npm --prefix web test

test-usb-video:
	sh components/kvm_usb_video/test/run.sh

test-usb-video-transport:
	sh components/kvm_usb_video/test/run-transport.sh

test-usb-video-signing:
	sh components/kvm_usb_video/test/run-signing.sh
