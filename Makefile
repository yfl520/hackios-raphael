SHELL := /bin/bash
PYTHON ?= python3

.DEFAULT_GOAL := help
.PHONY: help components deps wifi kernel mesa userland loader cellular persistence verify test test-host test-cellular test-installer audit

help:
	@printf '%s\n' \
	  'Build: make deps | wifi | kernel | mesa | userland | loader' \
	  '       make cellular | persistence | components' \
	  'Check: make verify | test | audit' \
	  'Docs:  docs/README.md'

# Retain the existing ordered build and script entry points.
components:
	bash scripts/build-components.sh all

deps wifi kernel mesa userland loader:
	bash scripts/build-components.sh $@

cellular:
	bash scripts/build-cellular-adapter.sh

persistence:
	bash scripts/build-persistent-init.sh

verify:
	$(PYTHON) -B scripts/verify-products.py --complete

test: test-host test-cellular test-installer

test-host:
	bash scripts/test-host.sh

test-cellular:
	bash scripts/test-cellular.sh

test-installer:
	$(PYTHON) -B -m unittest discover -s installer/tests -p 'test_*.py'

audit:
	$(PYTHON) -B scripts/audit-export.py
