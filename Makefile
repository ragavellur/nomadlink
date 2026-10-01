.DEFAULT_GOAL := help
.PHONY: help check validate render secrets baseline product test

help:
	@echo "NomadLink — project gates"
	@echo ""
	@echo "  make check      run every gate (validate + secrets + render + baseline + product build)"
	@echo "  make validate   enforce project tracker invariants"
	@echo "  make render     regenerate the HTML dashboards from JSON"
	@echo "  make secrets    scan committable files for credentials (ARGS=--history for all commits)"
	@echo "  make baseline   compile every hardware regression sketch"
	@echo "  make product    compile the product firmware sketch (firmware/nomadlink)"
	@echo ""
	@echo "check must pass before any commit."

check: validate secrets render baseline test-position product
	@echo ""
	@echo "All gates passed."

validate:
	@python3 scripts/project-tracker/validate.py

# Host unit tests for the NMEA reader. It is pure C with no ESP-IDF dependency
# precisely so it can be run here. Part of `check` because GGA parsing is where a
# previous build shipped satsUsed=99 unnoticed -- and where the first version of
# this parser read ddmm.mmmm as decimal degrees and dropped every real fix.
test-position:
	@echo "==> position: NMEA unit tests (host)"
	@cc -std=c11 -Wall -Wextra -Wno-unused-parameter \
	  -I firmware/nat_router/components/position/include \
	  -o /tmp/nomadlink_test_nmea \
	  firmware/nat_router/components/position/nmea.c \
	  firmware/nat_router/components/position/test/test_nmea.c -lm
	@/tmp/nomadlink_test_nmea
	@echo "==> position: FreeRTOS task-return guard"
	@python3 -c "import re,sys; \
src=open('firmware/nat_router/components/position/position.c').read(); \
m=re.search(r'static void (nmea_task|position_task)\(.*?\n\}', src, re.S); \
[sys.exit('FAIL: '+n+' contains a bare return; a FreeRTOS task returning panics with IllegalInstruction') \
 for n in ('nmea_task','position_task') \
 for b in [re.search(r'static void '+n+r'\(.*?\n\}', src, re.S).group(0)] if re.search(r'^\s*return\s*;', b, re.M)]; \
print('  o task-return guard ok')"
	@echo "==> position: no abort-on-failure in the optional component"
	@! grep -vE '^[[:space:]]*(/\\*|\\*|//)' firmware/nat_router/components/position/position.c \
	   | grep -q ESP_ERROR_CHECK \
	  || (echo "FAIL: ESP_ERROR_CHECK in position.c code would abort the router" && false)

render:
	@python3 scripts/project-tracker/render.py

secrets:
	@python3 scripts/project-tracker/secrets_scan.py $(ARGS)

baseline:
	@bash scripts/project-tracker/build-baseline.sh

product:
	@bash scripts/project-tracker/build-product.sh

# Build the product firmware and stage it where the web installer serves it from.
# Run this after any firmware change that must reach users, or the installer keeps
# flashing the previous image while the source says otherwise.
stage-firmware:
	@bash scripts/project-tracker/stage-firmware.sh

flash:
ifndef SKETCH
	$(error set SKETCH, e.g. make flash SKETCH=tracker)
endif
	@bash scripts/project-tracker/flash-sketch.sh $(SKETCH)
