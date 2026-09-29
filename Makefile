.DEFAULT_GOAL := help
.PHONY: help check validate render secrets baseline test

help:
	@echo "NomadLink — project gates"
	@echo ""
	@echo "  make check      run every gate (validate + secrets + render + baseline build)"
	@echo "  make validate   enforce project tracker invariants"
	@echo "  make render     regenerate the HTML dashboards from JSON"
	@echo "  make secrets    scan committable files for credentials (ARGS=--history for all commits)"
	@echo "  make baseline   compile every hardware regression sketch"
	@echo ""
	@echo "check must pass before any commit."

check: validate secrets render baseline
	@echo ""
	@echo "All gates passed."

validate:
	@python3 scripts/project-tracker/validate.py

render:
	@python3 scripts/project-tracker/render.py

secrets:
	@python3 scripts/project-tracker/secrets_scan.py $(ARGS)

baseline:
	@bash scripts/project-tracker/build-baseline.sh
